// modloader/src/reflection/lua_dump_generator.cpp
// SDK Generator — produces 3 output formats:
//
// 1. CXXHeaderDump/ — UE4SS-identical C++ headers per-package
//    {Package}.hpp + {Package}_enums.hpp
//    Properties: {:85} // 0x{:04X} (size: 0x{:X})
//
// 2. Lua/ — UE4SS-identical LuaLS annotations per-package
//    {Package}.lua + {Package}_enums.lua
//    ---@meta, ---@class, ---@field, ---@enum
//
// 3. SDK/ — Legacy per-class/struct/enum Lua files (kept for backward compat)

#include "modloader/lua_dump_generator.h"
#include "modloader/reflection_walker.h"
#include "modloader/paths.h"
#include "modloader/logger.h"
#include "modloader/symbols.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

namespace sdk_gen
{

    static int s_class_count = 0;
    static int s_struct_count = 0;
    static int s_enum_count = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Utility helpers
    // ═══════════════════════════════════════════════════════════════════════

    // Recursively create all parent directories for a filepath
    static void ensure_parent_dirs(const std::string &filepath)
    {
        for (size_t i = 1; i < filepath.size(); i++)
        {
            if (filepath[i] == '/')
            {
                std::string dir = filepath.substr(0, i);
                mkdir(dir.c_str(), 0755);
            }
        }
    }

    // Sanitize package name for use as a filename.
    // UE5 package names are like "/Script/Engine", "/Script/CoreUObject" —
    // the leading slash and internal slashes break file paths on Android.
    // Replace '/' with '_' and strip leading underscores.
    static std::string sanitize_pkg_name(const std::string &pkg)
    {
        std::string result = pkg;
        for (auto &c : result)
        {
            if (c == '/' || c == '\\')
                c = '_';
        }
        // Strip leading underscores (from leading slash)
        size_t start = result.find_first_not_of('_');
        if (start != std::string::npos && start > 0)
            result = result.substr(start);
        return result;
    }

    // Group items by package_name → vector of indices
    struct PackageData
    {
        std::vector<size_t> class_indices;
        std::vector<size_t> struct_indices;
        std::vector<size_t> enum_indices;
    };

    static std::map<std::string, PackageData> build_package_map(
        const std::vector<reflection::ClassInfo> &classes,
        const std::vector<reflection::StructInfo> &structs,
        const std::vector<reflection::EnumInfo> &enums)
    {
        std::map<std::string, PackageData> pkgs;
        for (size_t i = 0; i < classes.size(); i++)
        {
            std::string pkg = classes[i].package_name;
            if (pkg.empty())
                pkg = "UnknownPackage";
            pkgs[pkg].class_indices.push_back(i);
        }
        for (size_t i = 0; i < structs.size(); i++)
        {
            std::string pkg = structs[i].package_name;
            if (pkg.empty())
                pkg = "UnknownPackage";
            pkgs[pkg].struct_indices.push_back(i);
        }
        for (size_t i = 0; i < enums.size(); i++)
        {
            std::string pkg = enums[i].package_name;
            if (pkg.empty())
                pkg = "UnknownPackage";
            pkgs[pkg].enum_indices.push_back(i);
        }
        return pkgs;
    }

    // Pad a string to N characters with spaces (right-pad)
    static std::string pad_right(const std::string &s, size_t width)
    {
        if (s.size() >= width)
            return s;
        return s + std::string(width - s.size(), ' ');
    }

    // Determine if a class name likely represents a struct (F-prefix convention)
    static bool is_struct_prefix(const std::string &name)
    {
        if (name.empty())
            return false;
        return name[0] == 'F';
    }

    // Determine C++ class prefix keyword
    static const char *cxx_keyword(const std::string &name, bool is_struct_type)
    {
        if (is_struct_type)
            return "struct";
        return "class";
    }

    // ═══════════════════════════════════════════════════════════════════════
    // CXX type name conversion (PropType → C++ type string)
    // ═══════════════════════════════════════════════════════════════════════

    static std::string cxx_type_name(const reflection::PropertyInfo &pi)
    {
        using PT = reflection::PropType;
        switch (pi.type)
        {
        case PT::BoolProperty:
            return "bool";
        case PT::ByteProperty:
            if (!pi.inner_type_name.empty())
                return "TEnumAsByte<" + pi.inner_type_name + ">";
            return "uint8";
        case PT::Int8Property:
            return "int8";
        case PT::Int16Property:
            return "int16";
        case PT::IntProperty:
            return "int32";
        case PT::Int64Property:
            return "int64";
        case PT::UInt16Property:
            return "uint16";
        case PT::UInt32Property:
            return "uint32";
        case PT::UInt64Property:
            return "uint64";
        case PT::FloatProperty:
            return "float";
        case PT::DoubleProperty:
            return "double";
        case PT::NameProperty:
            return "FName";
        case PT::StrProperty:
            return "FString";
        case PT::TextProperty:
            return "FText";
        case PT::ObjectProperty:
            return pi.inner_type_name.empty() ? "UObject*" : (pi.inner_type_name + "*");
        case PT::WeakObjectProperty:
            return pi.inner_type_name.empty() ? "TWeakObjectPtr<UObject>" : ("TWeakObjectPtr<" + pi.inner_type_name + ">");
        case PT::LazyObjectProperty:
            return pi.inner_type_name.empty() ? "TLazyObjectPtr<UObject>" : ("TLazyObjectPtr<" + pi.inner_type_name + ">");
        case PT::SoftObjectProperty:
            return pi.inner_type_name.empty() ? "TSoftObjectPtr<UObject>" : ("TSoftObjectPtr<" + pi.inner_type_name + ">");
        case PT::ClassProperty:
            return pi.inner_type_name.empty() ? "UClass*" : ("TSubclassOf<" + pi.inner_type_name + ">");
        case PT::SoftClassProperty:
            return pi.inner_type_name.empty() ? "TSoftClassPtr<UObject>" : ("TSoftClassPtr<" + pi.inner_type_name + ">");
        case PT::InterfaceProperty:
            return pi.inner_type_name.empty() ? "FScriptInterface" : ("TScriptInterface<" + pi.inner_type_name + ">");
        case PT::StructProperty:
            return pi.inner_type_name.empty() ? "FUnknownStruct" : pi.inner_type_name;
        case PT::ArrayProperty:
            return pi.inner_type_name.empty() ? "TArray<uint8>" : ("TArray<" + pi.inner_type_name + ">");
        case PT::MapProperty:
        {
            std::string k = pi.inner_type_name.empty() ? "FName" : pi.inner_type_name;
            std::string v = pi.inner_type_name2.empty() ? "uint8" : pi.inner_type_name2;
            return "TMap<" + k + ", " + v + ">";
        }
        case PT::SetProperty:
            return pi.inner_type_name.empty() ? "TSet<uint8>" : ("TSet<" + pi.inner_type_name + ">");
        case PT::EnumProperty:
            return pi.inner_type_name.empty() ? "uint8" : pi.inner_type_name;
        case PT::DelegateProperty:
        case PT::MulticastDelegateProperty:
        case PT::MulticastInlineDelegateProperty:
        case PT::MulticastSparseDelegateProperty:
            return pi.inner_type_name.empty() ? "FMulticastDelegate" : pi.inner_type_name;
        case PT::FieldPathProperty:
            return "FFieldPath";
        default:
            return "uint8";
        }
    }

    // CXX return type for functions
    static std::string cxx_return_type(const reflection::FunctionInfo &fi)
    {
        if (fi.return_prop)
            return cxx_type_name(*fi.return_prop);
        return "void";
    }

    // CXX function param list string
    static std::string cxx_param_list(const reflection::FunctionInfo &fi)
    {
        std::string result;
        bool first = true;
        for (const auto &p : fi.params)
        {
            if (p.flags & ue::CPF_ReturnParm)
                continue;
            if (!first)
                result += ", ";
            first = false;

            // const param
            if (p.flags & ue::CPF_ConstParm)
                result += "const ";
            result += cxx_type_name(p);
            // out/ref param
            if (p.flags & ue::CPF_OutParm)
                result += "&";
            result += " ";
            result += p.name;
        }
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Lua type name conversion (PropType → Lua annotation type)
    // ═══════════════════════════════════════════════════════════════════════

    static std::string lua_type_name(const reflection::PropertyInfo &pi)
    {
        using PT = reflection::PropType;
        switch (pi.type)
        {
        case PT::BoolProperty:
            return "boolean";
        case PT::ByteProperty:
            return pi.inner_type_name.empty() ? "uint8" : pi.inner_type_name;
        case PT::Int8Property:
            return "int8";
        case PT::Int16Property:
            return "int16";
        case PT::IntProperty:
            return "int32";
        case PT::Int64Property:
            return "int64";
        case PT::UInt16Property:
            return "uint16";
        case PT::UInt32Property:
            return "uint32";
        case PT::UInt64Property:
            return "uint64";
        case PT::FloatProperty:
            return "float";
        case PT::DoubleProperty:
            return "double";
        case PT::NameProperty:
            return "FName";
        case PT::StrProperty:
            return "FString";
        case PT::TextProperty:
            return "FText";
        case PT::ObjectProperty:
            return pi.inner_type_name.empty() ? "UObject" : pi.inner_type_name;
        case PT::WeakObjectProperty:
            return pi.inner_type_name.empty() ? "TWeakObjectPtr<UObject>" : ("TWeakObjectPtr<" + pi.inner_type_name + ">");
        case PT::LazyObjectProperty:
            return pi.inner_type_name.empty() ? "TLazyObjectPtr<UObject>" : ("TLazyObjectPtr<" + pi.inner_type_name + ">");
        case PT::SoftObjectProperty:
            return pi.inner_type_name.empty() ? "TSoftObjectPtr<UObject>" : ("TSoftObjectPtr<" + pi.inner_type_name + ">");
        case PT::ClassProperty:
            return pi.inner_type_name.empty() ? "UClass" : ("TSubclassOf<" + pi.inner_type_name + ">");
        case PT::SoftClassProperty:
            return pi.inner_type_name.empty() ? "TSoftClassPtr<UObject>" : ("TSoftClassPtr<" + pi.inner_type_name + ">");
        case PT::InterfaceProperty:
            return pi.inner_type_name.empty() ? "FScriptInterface" : ("TScriptInterface<" + pi.inner_type_name + ">");
        case PT::StructProperty:
            return pi.inner_type_name.empty() ? "table" : pi.inner_type_name;
        case PT::ArrayProperty:
            return pi.inner_type_name.empty() ? "TArray" : ("TArray<" + pi.inner_type_name + ">");
        case PT::MapProperty:
        {
            std::string k = pi.inner_type_name.empty() ? "FName" : pi.inner_type_name;
            std::string v = pi.inner_type_name2.empty() ? "uint8" : pi.inner_type_name2;
            return "TMap<" + k + ", " + v + ">";
        }
        case PT::SetProperty:
            return pi.inner_type_name.empty() ? "TSet" : ("TSet<" + pi.inner_type_name + ">");
        case PT::EnumProperty:
            return pi.inner_type_name.empty() ? "uint8" : pi.inner_type_name;
        case PT::DelegateProperty:
        case PT::MulticastDelegateProperty:
        case PT::MulticastInlineDelegateProperty:
        case PT::MulticastSparseDelegateProperty:
            return pi.inner_type_name.empty() ? "FDelegate" : pi.inner_type_name;
        case PT::FieldPathProperty:
            return "FFieldPath";
        default:
            return "any";
        }
    }

    // Check if a string is a valid Lua identifier
    static bool is_valid_lua_ident(const std::string &s)
    {
        if (s.empty())
            return false;
        // Check Lua keywords
        static const char *keywords[] = {
            "and", "break", "do", "else", "elseif", "end", "false", "for",
            "function", "goto", "if", "in", "local", "nil", "not", "or",
            "repeat", "return", "then", "true", "until", "while", nullptr};
        for (int i = 0; keywords[i]; i++)
        {
            if (s == keywords[i])
                return false;
        }
        // Must start with letter or underscore
        if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_'))
            return false;
        // Rest: alphanumeric or underscore
        for (size_t i = 1; i < s.size(); i++)
        {
            char c = s[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        }
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 1. CXX Header Generator — UE4SS CXXHeaderDump format
    // ═══════════════════════════════════════════════════════════════════════

    static void cxx_write_enum(FILE *f, const reflection::EnumInfo &ei)
    {
        // Detect if this is a namespaced enum (values without :: prefix same as enum name)
        // For simplicity: use enum class for all (UE4SS default for most UE4.27 enums)
        fprintf(f, "enum class %s {\n", ei.name.c_str());
        for (size_t i = 0; i < ei.values.size(); i++)
        {
            const auto &v = ei.values[i];
            fprintf(f, "    %s = %lld,\n", v.first.c_str(), (long long)v.second);
        }
        fprintf(f, "};\n\n\n");
    }

    static void cxx_write_struct(FILE *f, const reflection::StructInfo &si)
    {
        if (si.parent_name.empty())
        {
            fprintf(f, "struct %s\n", si.name.c_str());
        }
        else
        {
            fprintf(f, "struct %s : public %s\n", si.name.c_str(), si.parent_name.c_str());
        }
        fprintf(f, "{\n");

        // Properties with padding
        int32_t expected_offset = 0;
        int padding_idx = 0;
        for (const auto &prop : si.properties)
        {
            // Insert padding if there's a gap
            if (prop.offset > expected_offset)
            {
                int32_t gap = prop.offset - expected_offset;
                char line_buf[256];
                snprintf(line_buf, sizeof(line_buf), "    char padding_%d[0x%X];", padding_idx++, gap);
                std::string line(line_buf);
                std::string padded = pad_right(line, 85);
                fprintf(f, "%s // 0x%04X (size: 0x%X)\n", padded.c_str(), expected_offset, gap);
            }
            // Property line
            std::string type_str = cxx_type_name(prop);
            char line_buf[512];
            snprintf(line_buf, sizeof(line_buf), "    %s %s;", type_str.c_str(), prop.name.c_str());
            std::string line(line_buf);
            std::string padded = pad_right(line, 85);
            fprintf(f, "%s // 0x%04X (size: 0x%X)\n", padded.c_str(), prop.offset, prop.element_size);

            expected_offset = prop.offset + prop.element_size;
        }

        fprintf(f, "}; // Size: 0x%X\n\n\n", si.properties_size);
    }

    static void cxx_write_class(FILE *f, const reflection::ClassInfo &ci)
    {
        // Determine keyword: struct for F-prefix names, class otherwise
        const char *kw = is_struct_prefix(ci.name) ? "struct" : "class";

        if (ci.parent_name.empty())
        {
            fprintf(f, "%s %s\n", kw, ci.name.c_str());
        }
        else
        {
            fprintf(f, "%s %s : public %s\n", kw, ci.name.c_str(), ci.parent_name.c_str());
        }
        fprintf(f, "{\n");

        // Properties with offset/size comments
        int32_t expected_offset = 0;
        int padding_idx = 0;
        for (const auto &prop : ci.properties)
        {
            // Insert padding if there's a gap
            if (prop.offset > expected_offset)
            {
                int32_t gap = prop.offset - expected_offset;
                char line_buf[256];
                snprintf(line_buf, sizeof(line_buf), "    char padding_%d[0x%X];", padding_idx++, gap);
                std::string line(line_buf);
                std::string padded = pad_right(line, 85);
                fprintf(f, "%s // 0x%04X (size: 0x%X)\n", padded.c_str(), expected_offset, gap);
            }

            std::string type_str = cxx_type_name(prop);
            char line_buf[512];
            snprintf(line_buf, sizeof(line_buf), "    %s %s;", type_str.c_str(), prop.name.c_str());
            std::string line(line_buf);
            std::string padded = pad_right(line, 85);
            fprintf(f, "%s // 0x%04X (size: 0x%X)\n", padded.c_str(), prop.offset, prop.element_size);

            // For delegate properties, emit the delegate function signature right after
            if (prop.type == reflection::PropType::DelegateProperty ||
                prop.type == reflection::PropType::MulticastDelegateProperty ||
                prop.type == reflection::PropType::MulticastInlineDelegateProperty ||
                prop.type == reflection::PropType::MulticastSparseDelegateProperty)
            {
                // Find matching function with delegate signature name
                std::string delegate_func_name = prop.name;
                // Strip __DelegateSignature suffix if present
                size_t ds_pos = delegate_func_name.find("__DelegateSignature");
                if (ds_pos != std::string::npos)
                    delegate_func_name = delegate_func_name.substr(0, ds_pos);

                for (const auto &func : ci.functions)
                {
                    std::string fn = func.name;
                    size_t ds_pos2 = fn.find("__DelegateSignature");
                    if (ds_pos2 != std::string::npos)
                        fn = fn.substr(0, ds_pos2);
                    if (fn == delegate_func_name)
                    {
                        fprintf(f, "    %s %s(%s);\n", cxx_return_type(func).c_str(),
                                delegate_func_name.c_str(), cxx_param_list(func).c_str());
                        break;
                    }
                }
            }

            expected_offset = prop.offset + prop.element_size;
        }

        // Functions (non-delegate)
        if (!ci.functions.empty())
        {
            fprintf(f, "\n");
            for (const auto &func : ci.functions)
            {
                // Skip delegate signatures (already emitted inline)
                if (func.name.find("__DelegateSignature") != std::string::npos)
                    continue;
                fprintf(f, "    %s %s(%s);\n", cxx_return_type(func).c_str(),
                        func.name.c_str(), cxx_param_list(func).c_str());
            }
        }

        fprintf(f, "}; // Size: 0x%X\n\n\n", ci.properties_size);
    }

    int generate_cxx_headers()
    {
        const auto &classes = reflection::get_classes();
        const auto &structs = reflection::get_structs();
        const auto &enums = reflection::get_enums();

        auto pkgs = build_package_map(classes, structs, enums);
        int files = 0;

        for (const auto &[pkg_name, pkg_data] : pkgs)
        {
            // ── Primary file: {Package}.hpp ──
            bool has_primary = !pkg_data.class_indices.empty() || !pkg_data.struct_indices.empty();
            bool has_enums = !pkg_data.enum_indices.empty();

            // Sanitize package name — UE5 names like "/Script/Engine" contain slashes
            std::string safe_pkg = sanitize_pkg_name(pkg_name);

            if (has_primary)
            {
                std::string filepath = paths::cxx_header_dir() + safe_pkg + ".hpp";
                ensure_parent_dirs(filepath);
                FILE *f = fopen(filepath.c_str(), "w");
                if (!f)
                {
                    logger::log_error("SDK_GEN", "CXXHeaderDump: fopen failed for %s: %s",
                                      filepath.c_str(), strerror(errno));
                    continue;
                }

                // Include guard
                fprintf(f, "#ifndef UE4SS_SDK_%s_HPP\n", safe_pkg.c_str());
                fprintf(f, "#define UE4SS_SDK_%s_HPP\n\n", safe_pkg.c_str());
                if (has_enums)
                {
                    fprintf(f, "#include \"%s_enums.hpp\"\n\n", safe_pkg.c_str());
                }

                // Sort: structs first (by name), then classes (by name)
                // Collect struct indices sorted by name
                std::vector<size_t> sorted_structs = pkg_data.struct_indices;
                std::sort(sorted_structs.begin(), sorted_structs.end(),
                          [&](size_t a, size_t b)
                          { return structs[a].name < structs[b].name; });

                std::vector<size_t> sorted_classes = pkg_data.class_indices;
                std::sort(sorted_classes.begin(), sorted_classes.end(),
                          [&](size_t a, size_t b)
                          { return classes[a].name < classes[b].name; });

                for (size_t idx : sorted_structs)
                {
                    cxx_write_struct(f, structs[idx]);
                }
                for (size_t idx : sorted_classes)
                {
                    cxx_write_class(f, classes[idx]);
                }

                fprintf(f, "#endif\n");
                fclose(f);
                files++;
            }

            // ── Enums file: {Package}_enums.hpp ──
            if (has_enums)
            {
                std::string filepath = paths::cxx_header_dir() + safe_pkg + "_enums.hpp";
                ensure_parent_dirs(filepath);
                FILE *f = fopen(filepath.c_str(), "w");
                if (!f)
                {
                    logger::log_error("SDK_GEN", "CXXHeaderDump: fopen failed for %s: %s",
                                      filepath.c_str(), strerror(errno));
                    continue;
                }

                // No include guard for enums file (matches UE4SS)
                std::vector<size_t> sorted_enums = pkg_data.enum_indices;
                std::sort(sorted_enums.begin(), sorted_enums.end(),
                          [&](size_t a, size_t b)
                          { return enums[a].name < enums[b].name; });

                for (size_t idx : sorted_enums)
                {
                    cxx_write_enum(f, enums[idx]);
                }

                fclose(f);
                files++;
            }
        }

        logger::log_info("SDK", "CXXHeaderDump: %d files for %zu packages", files, pkgs.size());
        return files;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 2. Lua Types Generator — UE4SS Lua/ format
    // ═══════════════════════════════════════════════════════════════════════

    static void lua_write_enum(FILE *f, const reflection::EnumInfo &ei)
    {
        fprintf(f, "---@enum %s\n", ei.name.c_str());
        fprintf(f, "local %s = {\n", ei.name.c_str());
        for (size_t i = 0; i < ei.values.size(); i++)
        {
            const auto &v = ei.values[i];
            fprintf(f, "    %s = %lld,\n", v.first.c_str(), (long long)v.second);
        }
        fprintf(f, "}\n\n");
    }

    static void lua_write_struct(FILE *f, const reflection::StructInfo &si)
    {
        if (si.parent_name.empty())
        {
            fprintf(f, "---@class %s\n", si.name.c_str());
        }
        else
        {
            fprintf(f, "---@class %s : %s\n", si.name.c_str(), si.parent_name.c_str());
        }

        for (const auto &prop : si.properties)
        {
            std::string type_str = lua_type_name(prop);
            if (is_valid_lua_ident(prop.name))
            {
                fprintf(f, "---@field %s %s\n", prop.name.c_str(), type_str.c_str());
            }
            else
            {
                fprintf(f, "---@field ['%s'] %s\n", prop.name.c_str(), type_str.c_str());
            }
        }
        fprintf(f, "local %s = {}\n\n", si.name.c_str());
    }

    static void lua_write_class(FILE *f, const reflection::ClassInfo &ci)
    {
        if (ci.parent_name.empty())
        {
            fprintf(f, "---@class %s\n", ci.name.c_str());
        }
        else
        {
            fprintf(f, "---@class %s : %s\n", ci.name.c_str(), ci.parent_name.c_str());
        }

        // Fields
        for (const auto &prop : ci.properties)
        {
            std::string type_str = lua_type_name(prop);
            if (is_valid_lua_ident(prop.name))
            {
                fprintf(f, "---@field %s %s\n", prop.name.c_str(), type_str.c_str());
            }
            else
            {
                fprintf(f, "---@field ['%s'] %s\n", prop.name.c_str(), type_str.c_str());
            }
        }
        fprintf(f, "local %s = {}\n\n", ci.name.c_str());

        // Functions
        for (const auto &func : ci.functions)
        {
            // Skip delegate signatures
            if (func.name.find("__DelegateSignature") != std::string::npos)
                continue;

            // @param annotations (with struct field hints)
            for (const auto &p : func.params)
            {
                if (p.flags & ue::CPF_ReturnParm)
                    continue;
                std::string type_str = lua_type_name(p);
                // Add struct field hints for StructProperty params
                if (p.type == reflection::PropType::StructProperty && !p.inner_type_name.empty())
                {
                    reflection::StructInfo *si = reflection::find_struct(p.inner_type_name);
                    if (si && !si->properties.empty())
                    {
                        std::string fields_hint;
                        int shown = 0;
                        for (const auto &sp : si->properties)
                        {
                            if (shown > 0)
                                fields_hint += ", ";
                            fields_hint += sp.name + "=" + lua_type_name(sp);
                            shown++;
                            if (shown >= 6)
                            {
                                fields_hint += ", ...";
                                break;
                            }
                        }
                        fprintf(f, "---@param %s %s  -- struct {%s}\n", p.name.c_str(), type_str.c_str(), fields_hint.c_str());
                    }
                    else
                    {
                        fprintf(f, "---@param %s %s\n", p.name.c_str(), type_str.c_str());
                    }
                }
                else
                {
                    fprintf(f, "---@param %s %s\n", p.name.c_str(), type_str.c_str());
                }
            }

            // @return annotation
            if (func.return_prop)
            {
                std::string ret_type = lua_type_name(*func.return_prop);
                fprintf(f, "---@return %s\n", ret_type.c_str());
            }

            // Function stub
            if (is_valid_lua_ident(func.name))
            {
                fprintf(f, "function %s:%s(", ci.name.c_str(), func.name.c_str());
                bool first = true;
                for (const auto &p : func.params)
                {
                    if (p.flags & ue::CPF_ReturnParm)
                        continue;
                    if (!first)
                        fprintf(f, ", ");
                    first = false;
                    fprintf(f, "%s", p.name.c_str());
                }
                fprintf(f, ") end\n\n");
            }
            else
            {
                // Invalid Lua identifier → bracket syntax
                fprintf(f, "%s['%s'] = function(self", ci.name.c_str(), func.name.c_str());
                for (const auto &p : func.params)
                {
                    if (p.flags & ue::CPF_ReturnParm)
                        continue;
                    fprintf(f, ", %s", p.name.c_str());
                }
                fprintf(f, ") end\n\n");
            }
        }
    }

    int generate_lua_types()
    {
        const auto &classes = reflection::get_classes();
        const auto &structs = reflection::get_structs();
        const auto &enums = reflection::get_enums();

        auto pkgs = build_package_map(classes, structs, enums);
        int files = 0;

        for (const auto &[pkg_name, pkg_data] : pkgs)
        {
            bool has_primary = !pkg_data.class_indices.empty() || !pkg_data.struct_indices.empty();
            bool has_enums = !pkg_data.enum_indices.empty();
            std::string safe_pkg = sanitize_pkg_name(pkg_name);

            // ── Primary file: {Package}.lua ──
            if (has_primary)
            {
                std::string filepath = paths::lua_types_dir() + safe_pkg + ".lua";
                ensure_parent_dirs(filepath);
                FILE *f = fopen(filepath.c_str(), "w");
                if (!f)
                {
                    logger::log_error("SDK_GEN", "LuaTypes: fopen failed for %s: %s",
                                      filepath.c_str(), strerror(errno));
                    continue;
                }

                fprintf(f, "---@meta\n\n");

                // Sort: structs first, then classes
                std::vector<size_t> sorted_structs = pkg_data.struct_indices;
                std::sort(sorted_structs.begin(), sorted_structs.end(),
                          [&](size_t a, size_t b)
                          { return structs[a].name < structs[b].name; });

                std::vector<size_t> sorted_classes = pkg_data.class_indices;
                std::sort(sorted_classes.begin(), sorted_classes.end(),
                          [&](size_t a, size_t b)
                          { return classes[a].name < classes[b].name; });

                for (size_t idx : sorted_structs)
                {
                    lua_write_struct(f, structs[idx]);
                }
                for (size_t idx : sorted_classes)
                {
                    lua_write_class(f, classes[idx]);
                }

                fclose(f);
                files++;
            }

            // ── Enums file: {Package}_enums.lua ──
            if (has_enums)
            {
                std::string filepath = paths::lua_types_dir() + safe_pkg + "_enums.lua";
                ensure_parent_dirs(filepath);
                FILE *f = fopen(filepath.c_str(), "w");
                if (!f)
                {
                    logger::log_error("SDK_GEN", "LuaTypes: fopen failed for %s: %s",
                                      filepath.c_str(), strerror(errno));
                    continue;
                }

                fprintf(f, "---@meta\n\n");

                std::vector<size_t> sorted_enums = pkg_data.enum_indices;
                std::sort(sorted_enums.begin(), sorted_enums.end(),
                          [&](size_t a, size_t b)
                          { return enums[a].name < enums[b].name; });

                for (size_t idx : sorted_enums)
                {
                    lua_write_enum(f, enums[idx]);
                }

                fclose(f);
                files++;
            }
        }

        logger::log_info("SDK", "LuaTypes: %d files for %zu packages", files, pkgs.size());
        return files;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 3. Legacy SDK Generator (per-class/struct/enum files)
    // ═══════════════════════════════════════════════════════════════════════

    static std::string legacy_prop_type_to_lua(const reflection::PropertyInfo &pi)
    {
        return lua_type_name(pi); // Reuse the Lua type mapper
    }

    static std::string legacy_prop_type_tag(const reflection::PropertyInfo &pi)
    {
        using PT = reflection::PropType;
        switch (pi.type)
        {
        case PT::BoolProperty:
            return "BoolProperty";
        case PT::ByteProperty:
            return "ByteProperty";
        case PT::IntProperty:
            return "IntProperty";
        case PT::Int64Property:
            return "Int64Property";
        case PT::FloatProperty:
            return "FloatProperty";
        case PT::DoubleProperty:
            return "DoubleProperty";
        case PT::NameProperty:
            return "NameProperty";
        case PT::StrProperty:
            return "StrProperty";
        case PT::TextProperty:
            return "TextProperty";
        case PT::ObjectProperty:
            return "ObjectProperty";
        case PT::StructProperty:
            return "StructProperty";
        case PT::ArrayProperty:
            return "ArrayProperty";
        case PT::MapProperty:
            return "MapProperty";
        case PT::EnumProperty:
            return "EnumProperty";
        default:
            return "";
        }
    }

    static std::string legacy_func_flags_str(uint32_t flags)
    {
        std::string result;
        if (flags & ue::FUNC_Native)
            result += "[Native]";
        if (flags & ue::FUNC_BlueprintCallable)
            result += "[BlueprintCallable]";
        if (flags & ue::FUNC_BlueprintEvent)
            result += "[BlueprintEvent]";
        if (flags & ue::FUNC_BlueprintPure)
            result += "[BlueprintPure]";
        if (flags & ue::FUNC_Event)
            result += "[Event]";
        if (flags & ue::FUNC_Static)
            result += "[Static]";
        if (flags & ue::FUNC_Exec)
            result += "[Exec]";
        if (flags & ue::FUNC_Net)
            result += "[Net]";
        if (flags & ue::FUNC_NetServer)
            result += "[NetServer]";
        if (flags & ue::FUNC_NetClient)
            result += "[NetClient]";
        if (flags & ue::FUNC_Const)
            result += "[Const]";
        return result;
    }

    static void legacy_write_class(const reflection::ClassInfo &ci)
    {
        std::string filepath = paths::sdk_classes_dir() + ci.name + ".lua";
        FILE *f = fopen(filepath.c_str(), "w");
        if (!f)
            return;

        if (ci.parent_name.empty())
            fprintf(f, "---@class %s\n", ci.name.c_str());
        else
            fprintf(f, "---@class %s : %s\n", ci.name.c_str(), ci.parent_name.c_str());
        fprintf(f, "local %s = {}\n\n", ci.name.c_str());

        if (!ci.properties.empty())
        {
            fprintf(f, "-- Properties — offset and type from live UProperty walk\n");
            for (const auto &prop : ci.properties)
            {
                std::string lt = legacy_prop_type_to_lua(prop);
                std::string tag = legacy_prop_type_tag(prop);
                if (prop.type == reflection::PropType::BoolProperty)
                    fprintf(f, "---@field %s %s                  @ 0x%04X  (%s mask 0x%02X)\n",
                            prop.name.c_str(), lt.c_str(), prop.offset, tag.c_str(), prop.bool_byte_mask);
                else if (!tag.empty())
                    fprintf(f, "---@field %s %s                  @ 0x%04X  (%s)\n",
                            prop.name.c_str(), lt.c_str(), prop.offset, tag.c_str());
                else
                    fprintf(f, "---@field %s %s                  @ 0x%04X\n",
                            prop.name.c_str(), lt.c_str(), prop.offset);
            }
            fprintf(f, "\n");
        }

        if (!ci.functions.empty())
        {
            fprintf(f, "-- Functions — ALL go through ProcessEvent, native AND Blueprint\n");
            fprintf(f, "-- Struct params: pass as table {Field1=val, Field2=val} or LuaUStruct from :Get()\n");
            for (const auto &func : ci.functions)
            {
                for (const auto &param : func.params)
                {
                    if (param.flags & ue::CPF_ReturnParm)
                        continue;
                    std::string lt = legacy_prop_type_to_lua(param);
                    // Add struct field hints for StructProperty params
                    if (param.type == reflection::PropType::StructProperty && !param.inner_type_name.empty())
                    {
                        reflection::StructInfo *si = reflection::find_struct(param.inner_type_name);
                        if (si && !si->properties.empty())
                        {
                            std::string fields_hint;
                            int shown = 0;
                            for (const auto &sp : si->properties)
                            {
                                if (shown > 0)
                                    fields_hint += ", ";
                                fields_hint += sp.name + "=" + legacy_prop_type_to_lua(sp);
                                shown++;
                                if (shown >= 6)
                                {
                                    fields_hint += ", ...";
                                    break;
                                }
                            }
                            fprintf(f, "---@param %s %s  -- struct {%s}\n", param.name.c_str(), lt.c_str(), fields_hint.c_str());
                        }
                        else
                        {
                            fprintf(f, "---@param %s %s\n", param.name.c_str(), lt.c_str());
                        }
                    }
                    else
                    {
                        fprintf(f, "---@param %s %s\n", param.name.c_str(), lt.c_str());
                    }
                }
                if (func.return_prop)
                    fprintf(f, "---@return %s\n", legacy_prop_type_to_lua(*func.return_prop).c_str());
                else
                    fprintf(f, "---@return nil\n");

                std::string fflags = legacy_func_flags_str(func.flags);
                if (!fflags.empty())
                    fprintf(f, "---%s\n", fflags.c_str());

                fprintf(f, "function %s:%s(", ci.name.c_str(), func.name.c_str());
                bool first = true;
                for (const auto &param : func.params)
                {
                    if (param.flags & ue::CPF_ReturnParm)
                        continue;
                    if (!first)
                        fprintf(f, ", ");
                    fprintf(f, "%s", param.name.c_str());
                    first = false;
                }
                fprintf(f, ") end\n\n");
            }
        }

        fprintf(f, "return %s\n", ci.name.c_str());
        fclose(f);
    }

    static void legacy_write_struct(const reflection::StructInfo &si)
    {
        std::string filepath = paths::sdk_structs_dir() + si.name + ".lua";
        FILE *f = fopen(filepath.c_str(), "w");
        if (!f)
            return;

        if (si.parent_name.empty())
            fprintf(f, "---@class %s\n", si.name.c_str());
        else
            fprintf(f, "---@class %s : %s\n", si.name.c_str(), si.parent_name.c_str());
        fprintf(f, "local %s = {}\n", si.name.c_str());

        for (const auto &prop : si.properties)
        {
            std::string lt = legacy_prop_type_to_lua(prop);
            std::string tag = legacy_prop_type_tag(prop);
            if (prop.type == reflection::PropType::BoolProperty)
                fprintf(f, "---@field %s %s      @ 0x%04X  mask 0x%02X\n",
                        prop.name.c_str(), lt.c_str(), prop.offset, prop.bool_byte_mask);
            else
                fprintf(f, "---@field %s %s      @ 0x%04X\n",
                        prop.name.c_str(), lt.c_str(), prop.offset);
        }

        fprintf(f, "return %s\n", si.name.c_str());
        fclose(f);
    }

    static void legacy_write_enum(const reflection::EnumInfo &ei)
    {
        std::string filepath = paths::sdk_enums_dir() + ei.name + ".lua";
        FILE *f = fopen(filepath.c_str(), "w");
        if (!f)
            return;

        fprintf(f, "---@enum %s\n", ei.name.c_str());
        fprintf(f, "return {\n");
        for (size_t i = 0; i < ei.values.size(); i++)
        {
            const auto &v = ei.values[i];
            fprintf(f, "    %s = %lld", v.first.c_str(), (long long)v.second);
            if (i + 1 < ei.values.size())
                fprintf(f, ",");
            fprintf(f, "\n");
        }
        fprintf(f, "}\n");
        fclose(f);
    }

    int generate_legacy_sdk()
    {
        const auto &classes = reflection::get_classes();
        const auto &structs = reflection::get_structs();
        const auto &enums = reflection::get_enums();

        int files = 0;
        for (const auto &ci : classes)
        {
            legacy_write_class(ci);
            files++;
        }
        for (const auto &si : structs)
        {
            legacy_write_struct(si);
            files++;
        }
        for (const auto &ei : enums)
        {
            legacy_write_enum(ei);
            files++;
        }

        // Write _index.lua
        {
            std::string filepath = paths::sdk_index_path();
            FILE *f = fopen(filepath.c_str(), "w");
            if (f)
            {
                fprintf(f, "-- Auto-generated by UEModLoader — do not edit manually\n");
                fprintf(f, "local SDK = { Classes = {}, Structs = {}, Enums = {} }\n\n");
                for (const auto &ci : classes)
                    fprintf(f, "SDK.Classes.%s = require(\"SDK/Classes/%s\")\n", ci.name.c_str(), ci.name.c_str());
                fprintf(f, "\n");
                for (const auto &si : structs)
                    fprintf(f, "SDK.Structs.%s = require(\"SDK/Structs/%s\")\n", si.name.c_str(), si.name.c_str());
                fprintf(f, "\n");
                for (const auto &ei : enums)
                    fprintf(f, "SDK.Enums.%s = require(\"SDK/Enums/%s\")\n", ei.name.c_str(), ei.name.c_str());
                fprintf(f, "\nreturn SDK\n");
                fclose(f);
                files++;
            }
        }

        // Write _sdk_manifest.json
        {
            std::string filepath = paths::sdk_manifest_path();
            FILE *f = fopen(filepath.c_str(), "w");
            if (f)
            {
                time_t now = time(nullptr);
                struct tm tm_info;
                localtime_r(&now, &tm_info);
                char timebuf[64];
                strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

                fprintf(f, "{\n");
                fprintf(f, "  \"generated\": \"%s\",\n", timebuf);
                fprintf(f, "  \"ue4_base\": \"0x%lX\",\n", symbols::lib_base());
                fprintf(f, "  \"counts\": { \"classes\": %zu, \"structs\": %zu, \"enums\": %zu },\n",
                        classes.size(), structs.size(), enums.size());

                fprintf(f, "  \"classes\": [");
                for (size_t i = 0; i < classes.size(); i++)
                {
                    if (i > 0)
                        fprintf(f, ", ");
                    fprintf(f, "\"%s\"", classes[i].name.c_str());
                }
                fprintf(f, "],\n");

                fprintf(f, "  \"structs\": [");
                for (size_t i = 0; i < structs.size(); i++)
                {
                    if (i > 0)
                        fprintf(f, ", ");
                    fprintf(f, "\"%s\"", structs[i].name.c_str());
                }
                fprintf(f, "],\n");

                fprintf(f, "  \"enums\": [");
                for (size_t i = 0; i < enums.size(); i++)
                {
                    if (i > 0)
                        fprintf(f, ", ");
                    fprintf(f, "\"%s\"", enums[i].name.c_str());
                }
                fprintf(f, "]\n");

                fprintf(f, "}\n");
                fclose(f);
                files++;
            }
        }

        logger::log_info("SDK", "LegacySDK: %d files", files);
        return files;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Top-level API
    // ═══════════════════════════════════════════════════════════════════════

    int generate()
    {
        auto start = std::chrono::steady_clock::now();

        const auto &classes = reflection::get_classes();
        const auto &structs = reflection::get_structs();
        const auto &enums = reflection::get_enums();

        s_class_count = static_cast<int>(classes.size());
        s_struct_count = static_cast<int>(structs.size());
        s_enum_count = static_cast<int>(enums.size());

        logger::log_info("SDK", "Generating SDK: %d classes, %d structs, %d enums...",
                         s_class_count, s_struct_count, s_enum_count);

        int total_files = 0;

        // 1. UE4SS CXX Header Dump
        total_files += generate_cxx_headers();

        // 2. UE4SS Lua Type Annotations
        total_files += generate_lua_types();

        // 3. Legacy per-class SDK
        total_files += generate_legacy_sdk();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        logger::log_info("SDK", "SDK complete: %d classes, %d structs, %d enums (%d files in %lldms)",
                         s_class_count, s_struct_count, s_enum_count, total_files, (long long)elapsed.count());

        return total_files;
    }

    int regenerate()
    {
        logger::log_info("SDK", "Regenerating SDK on demand...");
        reflection::walk_all();
        return generate();
    }

    int class_count() { return s_class_count; }
    int struct_count() { return s_struct_count; }
    int enum_count() { return s_enum_count; }

} // namespace sdk_gen
