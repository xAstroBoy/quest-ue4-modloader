// modloader/src/reflection/class_rebuilder.cpp
// Live class rebuild system — navigate Class → Instance → Property/Function
// Instance tracking via ProcessEvent tick (tracks spawns and destroys)
// All offsets from live UProperty->Offset_Internal — never hardcoded

#include "modloader/class_rebuilder.h"
#include "modloader/reflection_walker.h"
#include "modloader/process_event_hook.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"
#include <cstring>
#include <algorithm>
#include <atomic>

namespace rebuilder
{

    static std::unordered_map<std::string, RebuiltClass> s_classes;
    static std::mutex s_global_mutex;

    // Fast path: skip tick() entirely when nothing is registered
    static std::atomic<bool> s_has_any_tracking{false};

    // Per-instance hooks (keyed by UObject* address)
    struct InstanceHook
    {
        HookId id;
        ue::UObject *obj;
        std::string name; // prop or func name
        bool is_prop;
        PropHookCallback prop_cb;
        FuncPreCallback func_pre;
        FuncPostCallback func_post;
    };

    static std::vector<InstanceHook> s_instance_hooks;
    static std::mutex s_instance_hooks_mutex;
    static std::atomic<HookId> s_next_hook_id{1000000};

    // ═══ RebuiltProperty implementation ═════════════════════════════════════
    void RebuiltProperty::read(const ue::UObject *obj, void *out_buf) const
    {
        if (!obj || !out_buf)
            return;
        const uint8_t *base = reinterpret_cast<const uint8_t *>(obj);
        std::memcpy(out_buf, base + offset, element_size);
    }

    void RebuiltProperty::write(ue::UObject *obj, const void *in_buf) const
    {
        if (!obj || !in_buf)
            return;
        uint8_t *base = reinterpret_cast<uint8_t *>(obj);
        std::memcpy(base + offset, in_buf, element_size);
    }

    bool RebuiltProperty::read_bool(const ue::UObject *obj) const
    {
        if (!obj)
            return false;
        const uint8_t *base = reinterpret_cast<const uint8_t *>(obj);
        uint8_t byte_val = base[offset + bool_byte_offset_extra];
        return (byte_val & bool_byte_mask) != 0;
    }

    void RebuiltProperty::write_bool(ue::UObject *obj, bool value) const
    {
        if (!obj)
            return;
        uint8_t *base = reinterpret_cast<uint8_t *>(obj);
        if (value)
        {
            base[offset + bool_byte_offset_extra] |= bool_byte_mask;
        }
        else
        {
            base[offset + bool_byte_offset_extra] &= ~bool_byte_mask;
        }
    }

    // ═══ RebuiltClass lookup helpers ════════════════════════════════════════
    RebuiltProperty *RebuiltClass::find_property(const std::string &name)
    {
        auto it = prop_map.find(name);
        return (it != prop_map.end()) ? it->second : nullptr;
    }

    RebuiltFunction *RebuiltClass::find_function(const std::string &name)
    {
        auto it = func_map.find(name);
        return (it != func_map.end()) ? it->second : nullptr;
    }

    bool RebuiltClass::has_property(const std::string &name) const
    {
        return prop_map.find(name) != prop_map.end();
    }

    bool RebuiltClass::has_function(const std::string &name) const
    {
        return func_map.find(name) != func_map.end();
    }

    void RebuiltClass::add_instance(ue::UObject *obj)
    {
        std::lock_guard<std::mutex> lock(instances_mutex);
        live_instances.insert(obj);
    }

    void RebuiltClass::remove_instance(ue::UObject *obj)
    {
        std::lock_guard<std::mutex> lock(instances_mutex);
        live_instances.erase(obj);
    }

    ue::UObject *RebuiltClass::get_first_instance()
    {
        std::lock_guard<std::mutex> lock(instances_mutex);
        for (auto *obj : live_instances)
        {
            if (ue::is_valid_uobject(obj))
            {
                std::string name = reflection::get_short_name(obj);
                if (!ue::is_default_object(name.c_str()))
                    return obj;
            }
        }
        return nullptr;
    }

    ue::UObject *RebuiltClass::get_instance(int index)
    {
        std::lock_guard<std::mutex> lock(instances_mutex);
        int i = 0;
        for (auto *obj : live_instances)
        {
            if (!ue::is_valid_uobject(obj))
                continue;
            std::string name = reflection::get_short_name(obj);
            if (ue::is_default_object(name.c_str()))
                continue;
            if (i == index)
                return obj;
            i++;
        }
        return nullptr;
    }

    std::vector<ue::UObject *> RebuiltClass::get_all_instances()
    {
        std::lock_guard<std::mutex> lock(instances_mutex);
        std::vector<ue::UObject *> result;
        for (auto *obj : live_instances)
        {
            if (!ue::is_valid_uobject(obj))
                continue;
            std::string name = reflection::get_short_name(obj);
            if (ue::is_default_object(name.c_str()))
                continue;
            result.push_back(obj);
        }
        return result;
    }

    int RebuiltClass::instance_count()
    {
        std::lock_guard<std::mutex> lock(instances_mutex);
        return static_cast<int>(live_instances.size());
    }

    // ═══ Convert reflection info to rebuilt structures ══════════════════════
    static RebuiltProperty convert_prop(const reflection::PropertyInfo &pi)
    {
        RebuiltProperty rp;
        rp.name = pi.name;
        rp.type = pi.type;
        rp.offset = pi.offset;
        rp.element_size = pi.element_size;
        rp.flags = pi.flags;
        rp.raw = pi.raw;
        rp.bool_byte_mask = pi.bool_byte_mask;
        rp.bool_field_mask = pi.bool_field_mask;
        rp.bool_byte_offset_extra = pi.bool_byte_offset;

        // Human-readable type name
        switch (pi.type)
        {
        case reflection::PropType::BoolProperty:
            rp.type_name = "bool";
            break;
        case reflection::PropType::FloatProperty:
            rp.type_name = "float";
            break;
        case reflection::PropType::DoubleProperty:
            rp.type_name = "double";
            break;
        case reflection::PropType::IntProperty:
            rp.type_name = "int32";
            break;
        case reflection::PropType::Int64Property:
            rp.type_name = "int64";
            break;
        case reflection::PropType::ByteProperty:
            rp.type_name = "uint8";
            break;
        case reflection::PropType::NameProperty:
            rp.type_name = "FName";
            break;
        case reflection::PropType::StrProperty:
            rp.type_name = "FString";
            break;
        case reflection::PropType::ObjectProperty:
            rp.type_name = pi.inner_type_name.empty() ? "UObject*" : pi.inner_type_name;
            break;
        case reflection::PropType::StructProperty:
            rp.type_name = pi.inner_type_name.empty() ? "struct" : pi.inner_type_name;
            break;
        case reflection::PropType::ArrayProperty:
            rp.type_name = "TArray<" + pi.inner_type_name + ">";
            break;
        case reflection::PropType::EnumProperty:
            rp.type_name = pi.inner_type_name.empty() ? "enum" : pi.inner_type_name;
            break;
        default:
            rp.type_name = "unknown";
            break;
        }

        return rp;
    }

    static RebuiltFunction convert_func(const reflection::FunctionInfo &fi)
    {
        RebuiltFunction rf;
        rf.name = fi.name;
        rf.flags = fi.flags;
        rf.parms_size = fi.parms_size;
        rf.return_offset = fi.return_value_offset;
        rf.num_parms = fi.num_parms;
        rf.raw = fi.raw;
        rf.params = fi.params;
        // DO NOT copy fi.return_prop — it points into fi.params which will be
        // destroyed when the source FunctionInfo goes out of scope.
        // We'll fix up return_prop to point into rf.params after vectors settle.
        rf.return_prop = nullptr;
        return rf;
    }

    // Fix up return_prop pointers in a vector of RebuiltFunctions.
    // Must be called AFTER the vector is in its final memory location
    // (no more push_back / reallocation).
    static void fixup_return_props(std::vector<RebuiltFunction> &funcs)
    {
        for (auto &rf : funcs)
        {
            rf.return_prop = nullptr;
            for (auto &p : rf.params)
            {
                if (p.flags & ue::CPF_ReturnParm)
                {
                    rf.return_prop = &p;
                    break;
                }
            }
        }
    }

    // ═══ Main rebuilder API ═════════════════════════════════════════════════
    void init()
    {
        logger::log_info("REBUILD", "Class rebuilder initialized");
    }

    RebuiltClass *rebuild(const std::string &class_name)
    {
        // Check cache under lock — release lock BEFORE calling rebuild(UClass*)
        // to avoid recursive mutex deadlock (std::mutex is NOT recursive).
        {
            std::lock_guard<std::mutex> lock(s_global_mutex);
            auto it = s_classes.find(class_name);
            if (it != s_classes.end())
                return &it->second;
        } // Lock released here

        // Find the class in reflection data (boot-time cache)
        reflection::ClassInfo *ci = reflection::find_class(class_name);
        if (ci)
        {
            // rebuild(UClass*) takes its own lock — safe now that we released ours
            return rebuild(ci->raw);
        }

        // Class not in boot-time reflection cache — try live GUObjectArray scan.
        // This handles late-spawned Blueprint classes like DebugMenu_C that don't
        // exist in the reflection walker's boot-time snapshot.
        ue::UClass *live_cls = reflection::find_class_ptr(class_name);
        if (live_cls)
        {
            logger::log_info("REBUILD", "Class '%s' not in boot cache — rebuilding from live UClass*",
                             class_name.c_str());
            return rebuild(live_cls);
        }

        return nullptr;
    }

    RebuiltClass *rebuild(ue::UClass *cls)
    {
        if (!cls)
            return nullptr;

        std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        if (name.empty())
            return nullptr;

        // Refresh the /proc/self/maps snapshot BEFORE walking properties/functions.
        // The game allocates new memory regions after boot, and the snapshot taken
        // during walk_all() may be stale — causing is_readable_ptr() to reject
        // valid ChildProperties/Children pointers as "unmapped". This was the root
        // cause of slot classes (AC_CollectibleSlot_C, BP_TrophyCollectibleSlot_C,
        // etc.) rebuilding with 0 properties and 0 functions.
        reflection::refresh_memory_map();

        std::lock_guard<std::mutex> lock(s_global_mutex);

        auto it = s_classes.find(name);
        if (it != s_classes.end())
            return &it->second;

        RebuiltClass rc;
        rc.name = name;
        rc.raw = cls;
        rc.properties_size = ue::ustruct_get_properties_size(reinterpret_cast<const ue::UStruct *>(cls));

        ue::UStruct *super = ue::ustruct_get_super(reinterpret_cast<const ue::UStruct *>(cls));
        if (super && ue::is_valid_ptr(super))
        {
            rc.parent_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(super));
        }

        // Own properties
        auto own_props = reflection::walk_properties(reinterpret_cast<const ue::UStruct *>(cls), false);
        for (const auto &p : own_props)
        {
            rc.properties.push_back(convert_prop(p));
        }

        // All properties (including inherited)
        auto all_props = reflection::walk_properties(reinterpret_cast<const ue::UStruct *>(cls), true);
        for (const auto &p : all_props)
        {
            rc.all_properties.push_back(convert_prop(p));
        }

        // Own functions
        auto own_funcs = reflection::walk_functions(reinterpret_cast<const ue::UStruct *>(cls), false);
        for (const auto &f : own_funcs)
        {
            rc.functions.push_back(convert_func(f));
        }

        // All functions (including inherited)
        auto all_funcs = reflection::walk_functions(reinterpret_cast<const ue::UStruct *>(cls), true);
        for (const auto &f : all_funcs)
        {
            rc.all_functions.push_back(convert_func(f));
        }

        // Build lookup maps — point into all_properties/all_functions
        s_classes.erase(name);
        auto [emp_it, emp_ok] = s_classes.emplace(std::piecewise_construct,
                                                  std::forward_as_tuple(name), std::forward_as_tuple());
        auto &inserted = emp_it->second;
        inserted.name = std::move(rc.name);
        inserted.parent_name = std::move(rc.parent_name);
        inserted.properties = std::move(rc.properties);
        inserted.all_properties = std::move(rc.all_properties);
        inserted.functions = std::move(rc.functions);
        inserted.all_functions = std::move(rc.all_functions);

        // Fix up return_prop pointers — they were set to nullptr in convert_func()
        // because the source FunctionInfo.params gets destroyed. Now that the vectors
        // are in their final memory location, re-scan for CPF_ReturnParm.
        fixup_return_props(inserted.functions);
        fixup_return_props(inserted.all_functions);

        for (auto &p : inserted.all_properties)
        {
            inserted.prop_map[p.name] = &p;
        }
        for (auto &f : inserted.all_functions)
        {
            inserted.func_map[f.name] = &f;
        }

        logger::log_info("REBUILD", "Rebuilt '%s': %zu own props, %zu all props, %zu own funcs, %zu all funcs",
                         name.c_str(),
                         inserted.properties.size(), inserted.all_properties.size(),
                         inserted.functions.size(), inserted.all_functions.size());

        // Enable tick() tracking now that at least one class is rebuilt
        s_has_any_tracking.store(true, std::memory_order_release);

        return &inserted;
    }

    RebuiltClass *get(const std::string &class_name)
    {
        std::lock_guard<std::mutex> lock(s_global_mutex);
        auto it = s_classes.find(class_name);
        return (it != s_classes.end()) ? &it->second : nullptr;
    }

    // ═══ Hooks ══════════════════════════════════════════════════════════════
    HookId hook_property(const std::string &class_name, const std::string &prop_name,
                         PropHookCallback callback)
    {
        auto *rc = rebuild(class_name);
        if (!rc)
            return 0;

        HookId id = s_next_hook_id.fetch_add(1);
        RebuiltClass::PropHookEntry entry;
        entry.id = id;
        entry.prop_name = prop_name;
        entry.callback = callback;
        rc->prop_hooks.push_back(entry);

        logger::log_info("REBUILD", "Property hook #%lu: %s.%s (class-wide)",
                         (unsigned long)id, class_name.c_str(), prop_name.c_str());
        return id;
    }

    HookId hook_property_instance(ue::UObject *obj, const std::string &prop_name,
                                  PropHookCallback callback)
    {
        HookId id = s_next_hook_id.fetch_add(1);
        std::lock_guard<std::mutex> lock(s_instance_hooks_mutex);
        InstanceHook ih;
        ih.id = id;
        ih.obj = obj;
        ih.name = prop_name;
        ih.is_prop = true;
        ih.prop_cb = callback;
        s_instance_hooks.push_back(ih);
        return id;
    }

    HookId hook_function(const std::string &class_name, const std::string &func_name,
                         FuncPreCallback pre, FuncPostCallback post)
    {
        auto *rc = rebuild(class_name);
        if (!rc)
            return 0;

        HookId id = s_next_hook_id.fetch_add(1);
        RebuiltClass::FuncHookEntry entry;
        entry.id = id;
        entry.func_name = func_name;
        entry.pre = pre;
        entry.post = post;
        rc->func_hooks.push_back(entry);

        // Also register with ProcessEvent hook system for dispatch
        auto *rf = rc->find_function(func_name);
        if (rf && rf->raw)
        {
            if (pre)
            {
                pe_hook::register_pre_ptr(rf->raw, [pre](ue::UObject *self, ue::UFunction *func, void *parms) -> bool
                                          { return pre(self, func, parms); });
            }
            if (post)
            {
                pe_hook::register_post_ptr(rf->raw, [post](ue::UObject *self, ue::UFunction *func, void *parms)
                                           { post(self, func, parms); });
            }
        }

        logger::log_info("REBUILD", "Function hook #%lu: %s.%s (class-wide, pre=%s post=%s)",
                         (unsigned long)id, class_name.c_str(), func_name.c_str(),
                         pre ? "yes" : "no", post ? "yes" : "no");
        return id;
    }

    HookId hook_function_instance(ue::UObject *obj, const std::string &func_name,
                                  FuncPreCallback pre, FuncPostCallback post)
    {
        HookId id = s_next_hook_id.fetch_add(1);
        std::lock_guard<std::mutex> lock(s_instance_hooks_mutex);
        InstanceHook ih;
        ih.id = id;
        ih.obj = obj;
        ih.name = func_name;
        ih.is_prop = false;
        ih.func_pre = pre;
        ih.func_post = post;
        s_instance_hooks.push_back(ih);
        return id;
    }

    void unhook(HookId id)
    {
        // Remove from class hooks
        {
            std::lock_guard<std::mutex> lock(s_global_mutex);
            for (auto &pair : s_classes)
            {
                auto &rc = pair.second;
                rc.prop_hooks.erase(
                    std::remove_if(rc.prop_hooks.begin(), rc.prop_hooks.end(),
                                   [id](const RebuiltClass::PropHookEntry &e)
                                   { return e.id == id; }),
                    rc.prop_hooks.end());
                rc.func_hooks.erase(
                    std::remove_if(rc.func_hooks.begin(), rc.func_hooks.end(),
                                   [id](const RebuiltClass::FuncHookEntry &e)
                                   { return e.id == id; }),
                    rc.func_hooks.end());
            }
        }

        // Remove from instance hooks
        {
            std::lock_guard<std::mutex> lock(s_instance_hooks_mutex);
            s_instance_hooks.erase(
                std::remove_if(s_instance_hooks.begin(), s_instance_hooks.end(),
                               [id](const InstanceHook &ih)
                               { return ih.id == id; }),
                s_instance_hooks.end());
        }

        pe_hook::unregister(id);
    }

    // ═══ Tick — called from ProcessEvent hook ═══════════════════════════════
    void tick(ue::UObject *self, ue::UFunction *func, void *parms)
    {
        // FAST PATH: if no classes have been rebuilt and no instance hooks exist,
        // this is a single atomic load (< 1 nanosecond) and return.
        // This makes tick() effectively free when ClassRebuilder is unused.
        if (!s_has_any_tracking.load(std::memory_order_relaxed))
            return;

        if (!self || !ue::is_valid_ptr(self))
            return;

        // Get the class name of the calling object
        ue::UClass *cls = ue::uobj_get_class(self);
        if (!cls || !ue::is_valid_ptr(cls))
            return;

        std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        if (class_name.empty())
            return;

        // Skip Default__ objects
        std::string obj_name = reflection::get_short_name(self);
        if (ue::is_default_object(obj_name.c_str()))
            return;

        // Track instances — add to the rebuilt class if we have one
        {
            std::lock_guard<std::mutex> lock(s_global_mutex);
            auto it = s_classes.find(class_name);
            if (it != s_classes.end())
            {
                it->second.add_instance(self);
            }

            // Also check parent classes
            ue::UStruct *super = ue::ustruct_get_super(reinterpret_cast<const ue::UStruct *>(cls));
            while (super && ue::is_valid_ptr(super))
            {
                std::string super_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(super));
                auto sit = s_classes.find(super_name);
                if (sit != s_classes.end())
                {
                    sit->second.add_instance(self);
                }
                super = ue::ustruct_get_super(super);
            }
        }

        // Check for BeginDestroy to remove instances
        std::string func_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
        if (func_name == "BeginDestroy" || func_name == "EndPlay")
        {
            std::lock_guard<std::mutex> lock(s_global_mutex);
            for (auto &pair : s_classes)
            {
                pair.second.remove_instance(self);
            }
        }

        // Dispatch per-instance function hooks
        {
            std::lock_guard<std::mutex> lock(s_instance_hooks_mutex);
            for (auto &ih : s_instance_hooks)
            {
                if (ih.is_prop)
                    continue;
                if (ih.obj != self)
                    continue;
                if (ih.name != func_name)
                    continue;
                if (ih.func_pre)
                {
                    bool blocked = ih.func_pre(self, func, parms);
                    if (blocked)
                        return; // BLOCK
                }
            }
        }
    }

    ue::UObject *get_cdo(const std::string &class_name)
    {
        ue::UClass *cls = reflection::find_class_ptr(class_name);
        if (!cls)
            return nullptr;

        // Walk GUObjectArray to find the Default__ instance
        int32_t count = reflection::get_object_count();
        for (int32_t i = 0; i < count; i++)
        {
            // Inlined object access — same as reflection_walker
            ue::UObject *obj = nullptr;
            uintptr_t guoa = reinterpret_cast<uintptr_t>(symbols::GUObjectArray);
            if (!guoa)
                continue;
            uintptr_t obj_objects = ue::read_field<uintptr_t>(
                reinterpret_cast<const void *>(guoa), ue::GUOBJECTARRAY_TO_OBJECTS);
            if (!ue::is_valid_ptr(reinterpret_cast<const void *>(obj_objects)))
                continue;
            int32_t chunk_idx = i / static_cast<int32_t>(ue::FUOBJECTITEM_CHUNK_SIZE);
            int32_t within = i % static_cast<int32_t>(ue::FUOBJECTITEM_CHUNK_SIZE);
            uintptr_t chunk_ptr = ue::read_field<uintptr_t>(
                reinterpret_cast<const void *>(obj_objects), chunk_idx * 8);
            if (!ue::is_valid_ptr(reinterpret_cast<const void *>(chunk_ptr)))
                continue;
            obj = ue::read_field<ue::UObject *>(
                reinterpret_cast<const void *>(chunk_ptr + within * FUOBJECTITEM_SIZE), 0);

            if (!obj || !ue::is_valid_ptr(obj))
                continue;
            if (ue::uobj_get_class(obj) != cls)
                continue;
            std::string name = reflection::get_short_name(obj);
            if (name.find("Default__") == 0)
                return obj;
        }
        return nullptr;
    }

    bool is_valid_instance(ue::UObject *obj)
    {
        if (!ue::is_valid_uobject(obj))
            return false;
        std::string name = reflection::get_short_name(obj);
        return !ue::is_default_object(name.c_str());
    }

    const std::unordered_map<std::string, RebuiltClass> &get_all()
    {
        return s_classes;
    }

} // namespace rebuilder
