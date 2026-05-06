// modloader/src/core/symbols.cpp
// Dynamic symbol resolution with multi-priority fallback:
//   1. dlsym by exact symbol name
//   2. dl_iterate_phdr scan across all loaded libraries
//   3. Pattern/AOB scan in libUE4.so
//   4. Registered fallback offset (from UE4Dumper data)
// Every attempt is logged at every priority level.

#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/pattern_scanner.h"
#include "modloader/auto_offsets.h"
#include "modloader/game_profile.h"
#include <dlfcn.h>
#include <link.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <mutex>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <vector>
#include <algorithm>
#include <cxxabi.h>
#include <chrono>
#include <utility>

namespace symbols
{

    static bool disallow_hardcoded_fallbacks()
    {
        // Pinball FX VR updates frequently reshuffle stripped symbols.
        // Hardcoded static offsets are intentionally disabled there.
        return game_profile::is_pinball_fx();
    }

    // Resolved symbol cache
    static std::unordered_map<std::string, void *> s_cache;
    static std::unordered_map<std::string, uintptr_t> s_fallbacks;
    struct PatternSpec
    {
        std::string pattern;
        int rip_offset = -1;
        int instr_size = 0;
    };

    static std::unordered_map<std::string, PatternSpec> s_patterns;
    static std::recursive_mutex s_mutex;

    // Relative offset cache — target_name -> (anchor_name, delta)
    static std::unordered_map<std::string, std::pair<std::string, int64_t>> s_relative_offsets;

    static void *s_lib_handle = nullptr;
    static uintptr_t s_lib_base = 0;

    // ═══ Global symbol storage ══════════════════════════════════════════════
    ue::ProcessEventFn ProcessEvent = nullptr;
    ue::StaticFindObjectFn StaticFindObject = nullptr;
    ue::StaticLoadObjectFn StaticLoadObject = nullptr;
    ue::StaticLoadClassFn StaticLoadClass = nullptr;
    ue::StaticConstructObjectFn StaticConstructObject = nullptr;
    ue::PakMountFn PakMount = nullptr;
    GetTransientPackageFn GetTransientPackage = nullptr;
    void *GUObjectArray = nullptr;
    void *GNames = nullptr;
    void *GEngine = nullptr;
    void *GWorld = nullptr;
    void *GetEtcModelClass = nullptr;
    FName_InitFn FName_Init = nullptr;
    FOutputDevice_LogFn FOutputDevice_Log = nullptr;
    FText_ToStringFn FText_ToString = nullptr;
    FText_FromStringFn FText_FromString = nullptr;
    FText_DtorFn FText_Dtor = nullptr;
    FText_CtorFn FText_Ctor = nullptr;

    // ═══ Cached ELF symbol scanner ══════════════════════════════════════════
    // Opens libUE4.so once, caches the mmap, and scans both .dynsym and .symtab
    // with substring and demangled matching. Much faster than per-call file I/O.

    static void *s_elf_mmap = nullptr;
    static size_t s_elf_mmap_size = 0;
    static bool s_elf_init_done = false;

    struct CachedSymSection
    {
        const Elf64_Sym *syms;
        size_t num_syms;
        const char *strtab;
        size_t strtab_size;
        uint32_t type; // SHT_SYMTAB or SHT_DYNSYM
    };
    static std::vector<CachedSymSection> s_elf_sections;

    // Lightweight symbol reference for the method name index
    // Points back into s_elf_sections — no string copies, no demangling
    struct SymRef
    {
        uint16_t sec_idx; // index into s_elf_sections
        uint32_t sym_idx; // symbol index within that section
    };
    static std::unordered_map<std::string, std::vector<SymRef>> s_method_idx;

    // Find the on-disk path of the engine library from /proc/self/maps
    static std::string find_lib_path_for_scan()
    {
        const std::string &lib_name = game_profile::engine_lib_name();
        FILE *maps = fopen("/proc/self/maps", "r");
        if (!maps)
            return "";
        char line[1024];
        std::string result;
        while (fgets(line, sizeof(line), maps))
        {
            if (strstr(line, lib_name.c_str()))
            {
                char *path_start = strchr(line, '/');
                if (path_start)
                {
                    size_t len = strlen(path_start);
                    while (len > 0 && (path_start[len - 1] == '\n' || path_start[len - 1] == '\r'))
                        path_start[--len] = '\0';
                    result = path_start;
                    break;
                }
            }
        }
        fclose(maps);
        return result;
    }

    // Forward declaration — used in init_elf_cache pre-demangling
    static bool is_ue4_metadata_symbol(const char *sym_name);

    static void init_elf_cache()
    {
        if (s_elf_init_done)
            return; // already initialized
        s_elf_init_done = true;

        std::string lib_path = find_lib_path_for_scan();
        if (lib_path.empty())
        {
            logger::log_warn("RESOLVE", "Cannot find %s path for ELF cache",
                             game_profile::engine_lib_name().c_str());
            return;
        }

        int fd = open(lib_path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            logger::log_warn("RESOLVE", "Cannot open %s for ELF cache: %s", lib_path.c_str(), strerror(errno));
            return;
        }

        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            close(fd);
            return;
        }
        s_elf_mmap_size = static_cast<size_t>(st.st_size);

        s_elf_mmap = mmap(nullptr, s_elf_mmap_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (s_elf_mmap == MAP_FAILED)
        {
            s_elf_mmap = nullptr;
            return;
        }

        const uint8_t *elf_base = static_cast<const uint8_t *>(s_elf_mmap);
        const Elf64_Ehdr *ehdr = reinterpret_cast<const Elf64_Ehdr *>(elf_base);

        if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        {
            munmap(s_elf_mmap, s_elf_mmap_size);
            s_elf_mmap = nullptr;
            return;
        }

        const Elf64_Shdr *shdrs = reinterpret_cast<const Elf64_Shdr *>(elf_base + ehdr->e_shoff);

        for (int i = 0; i < ehdr->e_shnum; i++)
        {
            if (shdrs[i].sh_type != SHT_SYMTAB && shdrs[i].sh_type != SHT_DYNSYM)
                continue;

            if (shdrs[i].sh_link >= static_cast<unsigned>(ehdr->e_shnum))
                continue;
            const Elf64_Shdr *str_shdr = &shdrs[shdrs[i].sh_link];

            // Bounds check
            if (shdrs[i].sh_offset + shdrs[i].sh_size > s_elf_mmap_size)
                continue;
            if (str_shdr->sh_offset + str_shdr->sh_size > s_elf_mmap_size)
                continue;

            CachedSymSection sec;
            sec.syms = reinterpret_cast<const Elf64_Sym *>(elf_base + shdrs[i].sh_offset);
            sec.num_syms = shdrs[i].sh_size / sizeof(Elf64_Sym);
            sec.strtab = reinterpret_cast<const char *>(elf_base + str_shdr->sh_offset);
            sec.strtab_size = str_shdr->sh_size;
            sec.type = shdrs[i].sh_type;
            s_elf_sections.push_back(sec);
        }

        // Copy symbol tables and string tables into heap memory, then munmap the file.
        // The kernel evicts mmap'd pages under memory pressure (VR game + modloader),
        // causing Phase 1 scans to page-fault on every access and stall for minutes.
        // Heap memory stays resident. Total: ~20MB for 415K symbols.
        for (auto &sec : s_elf_sections)
        {
            size_t syms_bytes = sec.num_syms * sizeof(Elf64_Sym);
            auto *syms_copy = static_cast<Elf64_Sym *>(malloc(syms_bytes));
            memcpy(syms_copy, sec.syms, syms_bytes);
            sec.syms = syms_copy;

            auto *str_copy = static_cast<char *>(malloc(sec.strtab_size));
            memcpy(str_copy, sec.strtab, sec.strtab_size);
            sec.strtab = str_copy;
        }

        // Release the mmap — all data is now in heap
        if (s_elf_mmap)
        {
            munmap(s_elf_mmap, s_elf_mmap_size);
            s_elf_mmap = nullptr;
        }

        int total_syms = 0;
        for (auto &sec : s_elf_sections)
            total_syms += static_cast<int>(sec.num_syms);
        logger::log_info("RESOLVE", "ELF cache initialized: %d sections, %d total symbols from %s",
                         static_cast<int>(s_elf_sections.size()), total_syms, lib_path.c_str());

        // ── Build method name index by parsing Itanium ABI mangled names ──
        // No demangling needed. Extracts class/method identifiers directly from _ZN...E patterns.
        // Uses ~5MB vs ~40MB for full pre-demangling. Zero string allocation except method keys.
        {
            using namespace std::chrono;
            auto t0 = steady_clock::now();
            size_t indexed = 0;

            for (uint16_t si = 0; si < static_cast<uint16_t>(s_elf_sections.size()); si++)
            {
                auto &sec = s_elf_sections[si];
                for (uint32_t j = 0; j < static_cast<uint32_t>(sec.num_syms); j++)
                {
                    const Elf64_Sym &sym = sec.syms[j];
                    if (sym.st_name == 0 || sym.st_name >= sec.strtab_size)
                        continue;
                    if (sym.st_shndx == SHN_UNDEF)
                        continue;
                    if (sym.st_value == 0)
                        continue;
                    if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC)
                        continue;

                    const char *name = &sec.strtab[sym.st_name];
                    if (name[0] != '_' || name[1] != 'Z')
                        continue;
                    if (is_ue4_metadata_symbol(name))
                        continue;

                    std::string method_key;

                    if (name[2] == 'N')
                    {
                        // Nested name: _ZN[qualifiers]<id1><id2>...<idN>E
                        const char *p = name + 3;
                        while (*p == 'K' || *p == 'V' || *p == 'r')
                            p++;

                        const char *ids[16];
                        int lens[16];
                        int count = 0;
                        bool is_ctor = false, is_dtor = false;

                        while (*p && *p != 'E' && count < 16)
                        {
                            if (*p == 'C' && p[1] >= '1' && p[1] <= '3')
                            {
                                is_ctor = true;
                                break;
                            }
                            if (*p == 'D' && p[1] >= '0' && p[1] <= '2')
                            {
                                is_dtor = true;
                                break;
                            }
                            if (*p == 'S')
                            {
                                p++;
                                if (*p == 't')
                                {
                                    p++;
                                    continue;
                                }
                                while (*p && *p != '_')
                                    p++;
                                if (*p == '_')
                                    p++;
                                continue;
                            }
                            if (*p == 'I')
                            {
                                int d = 1;
                                p++;
                                while (*p && d > 0)
                                {
                                    if (*p == 'I')
                                        d++;
                                    else if (*p == 'E')
                                        d--;
                                    p++;
                                }
                                continue;
                            }
                            if (!isdigit((unsigned char)*p))
                                break;
                            int len = 0;
                            while (isdigit((unsigned char)*p))
                            {
                                len = len * 10 + (*p - '0');
                                p++;
                            }
                            if (len <= 0 || len > 500)
                                break;
                            ids[count] = p;
                            lens[count] = len;
                            count++;
                            p += len;
                        }

                        if (is_dtor && count > 0)
                        {
                            method_key = "~" + std::string(ids[count - 1], lens[count - 1]);
                        }
                        else if (is_ctor && count > 0)
                        {
                            method_key = std::string(ids[count - 1], lens[count - 1]);
                        }
                        else if (count >= 2)
                        {
                            method_key = std::string(ids[count - 1], lens[count - 1]);
                        }
                    }
                    else if (isdigit((unsigned char)name[2]))
                    {
                        // Top-level function: _Z<len><name><params>
                        // E.g. _Z9Et3f_initmP8ETS_DATAPPv → Et3f_init
                        //      _Z16GetEtcModelClassmPP8AVR4Model → GetEtcModelClass
                        const char *p = name + 2;
                        int len = 0;
                        while (isdigit((unsigned char)*p))
                        {
                            len = len * 10 + (*p - '0');
                            p++;
                        }
                        if (len > 0 && len < 500)
                        {
                            // Verify the identifier chars are within strtab bounds
                            size_t name_offset = static_cast<size_t>(p - sec.strtab);
                            if (name_offset + len <= sec.strtab_size)
                            {
                                method_key = std::string(p, len);
                            }
                        }
                    }

                    if (method_key.empty())
                        continue;

                    SymRef ref;
                    ref.sec_idx = si;
                    ref.sym_idx = j;
                    s_method_idx[method_key].push_back(ref);
                    indexed++;
                }
            }

            auto ms = duration_cast<std::chrono::milliseconds>(steady_clock::now() - t0).count();
            logger::log_info("RESOLVE", "Method index built: %zu refs, %zu unique methods (%lldms)",
                             indexed, s_method_idx.size(), (long long)ms);

            // Debug: verify top-level function indexing worked
            // Use game-appropriate test names — RE4-exclusive symbols shouldn't be checked on PFX
            std::vector<const char *> test_names = {"StaticFindObject"};
            if (game_profile::is_re4_vr())
            {
                test_names.push_back("Et3f_init");
                test_names.push_back("GetEtcModelClass");
                test_names.push_back("VR4ModelInit");
                test_names.push_back("getEmListNum");
            }
            for (auto &tn : test_names)
            {
                auto it = s_method_idx.find(tn);
                logger::log_info("RESOLVE", "INDEX CHECK: '%s' -> %s (%zu entries)",
                                 tn, it != s_method_idx.end() ? "FOUND" : "NOT FOUND",
                                 it != s_method_idx.end() ? it->second.size() : (size_t)0);
            }
        }
    }

    // Split a string by a delimiter character
    static std::vector<std::string> split_string(const std::string &s, char delim)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); i++)
        {
            if (i == s.size() || s[i] == delim)
            {
                if (i > start)
                    parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return parts;
    }

    // Check if a mangled symbol name is a UE4 type registration metadata symbol
    // These are NEVER actual game functions — they are static data used by UE4's
    // type registration system. Hooking them would crash or do nothing.
    static bool is_ue4_metadata_symbol(const char *sym_name)
    {
        // Z_Construct_UFunction_ — UE4 function registration stubs
        if (strstr(sym_name, "Z_Construct_U"))
            return true;
        // _Statics — static data containers inside registration functions
        if (strstr(sym_name, "_Statics"))
            return true;
        // NewProp_ — property metadata
        if (strstr(sym_name, "NewProp_"))
            return true;
        // PropPointers — property pointer arrays
        if (strstr(sym_name, "PropPointers"))
            return true;
        // FuncParams — function parameter metadata
        if (strstr(sym_name, "FuncParams"))
            return true;
        return false;
    }

    // Scan the cached ELF symbol tables for a symbol by name
    // Phase 0a: indexed lookup for bare search name (O(1))
    // Phase 0b: underscore/:: split → method index + class filter (O(1))
    // Phase 1:  linear scan — only for names without separators (rare fallback)
    static void *elf_scan(const std::string &name)
    {
        init_elf_cache();
        if (s_elf_sections.empty() || s_lib_base == 0)
            return nullptr;

        const char *search = name.c_str();

        // ── Phase 0a: direct index lookup for the bare search name ──
        // Handles top-level functions (_Z<len><name>) and class methods (_ZN...E)
        // indexed at init time. O(1) lookup, no linear scan.
        {
            auto idx_it = s_method_idx.find(name);
            if (idx_it != s_method_idx.end() && !idx_it->second.empty())
            {
                const SymRef *best_ref = nullptr;
                size_t best_name_len = SIZE_MAX;
                for (auto &ref : idx_it->second)
                {
                    auto &sec = s_elf_sections[ref.sec_idx];
                    const Elf64_Sym &sym = sec.syms[ref.sym_idx];
                    const char *sym_name = &sec.strtab[sym.st_name];
                    size_t slen = strlen(sym_name);
                    if (slen < best_name_len)
                    {
                        best_name_len = slen;
                        best_ref = &ref;
                    }
                }
                if (best_ref)
                {
                    auto &sec = s_elf_sections[best_ref->sec_idx];
                    const Elf64_Sym &sym = sec.syms[best_ref->sym_idx];
                    const char *sym_name = &sec.strtab[sym.st_name];
                    void *result = reinterpret_cast<void *>(s_lib_base + sym.st_value);
                    int status;
                    char *dm = abi::__cxa_demangle(sym_name, nullptr, nullptr, &status);
                    logger::log_info("RESOLVE", "%s: ELF index match -> %s [%s] @ 0x%lX (%zu overloads)",
                                     search, sym_name, dm ? dm : sym_name,
                                     (unsigned long)(s_lib_base + sym.st_value),
                                     idx_it->second.size());
                    if (dm)
                        free(dm);
                    return result;
                }
            }
        }

        // ── Phase 0c: full mangled _ZN name → parse class+method → index ──
        // When a full mangled name like _ZN22AVR4CutscenePlayerPawn12UpdateCameraE...
        // is passed, Phase 0a won't find it (index is keyed by method name).
        // This phase parses the Itanium mangled name to extract identifiers,
        // then uses the method index with class filtering. Handles both correct
        // and slightly incorrect mangled names (wrong lengths, etc.).
        if (name.size() > 4 && name[0] == '_' && name[1] == 'Z' && name[2] == 'N')
        {
            // Try demangling first (handles correct mangled names)
            int dm_status;
            char *dm = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &dm_status);
            std::string class_hint, method_hint;

            if (dm)
            {
                // Demangled form: "ClassName::MethodName(params...)"
                std::string demangled(dm);
                free(dm);
                size_t paren = demangled.find('(');
                std::string prefix = (paren != std::string::npos) ? demangled.substr(0, paren) : demangled;
                // Strip qualifiers like "const" after the name
                size_t space = prefix.rfind(' ');
                if (space != std::string::npos)
                    prefix = prefix.substr(space + 1);
                size_t last_sep = prefix.rfind("::");
                if (last_sep != std::string::npos)
                {
                    method_hint = prefix.substr(last_sep + 2);
                    std::string cls_full = prefix.substr(0, last_sep);
                    size_t cls_sep = cls_full.rfind("::");
                    class_hint = (cls_sep != std::string::npos) ? cls_full.substr(cls_sep + 2) : cls_full;
                }
                else
                {
                    method_hint = prefix;
                }
            }
            else
            {
                // Demangling failed (wrong lengths, etc.) — extract identifiers heuristically.
                // Parse _ZN[qualifiers]<len><id><len><id>... pattern, being tolerant of errors.
                const char *p = name.c_str() + 3;
                while (*p == 'K' || *p == 'V' || *p == 'r')
                    p++;
                std::vector<std::string> ids;
                while (*p && *p != 'E')
                {
                    if (*p == 'C' || *p == 'D')
                        break; // ctor/dtor
                    if (*p == 'S' || *p == 'I')
                        break; // substitution/template — stop
                    if (!isdigit((unsigned char)*p))
                        break;
                    int len = 0;
                    while (isdigit((unsigned char)*p))
                    {
                        len = len * 10 + (*p - '0');
                        p++;
                    }
                    if (len <= 0 || len > 200)
                        break;
                    // Even if len overshoots the actual identifier, grab what we can
                    std::string id;
                    for (int k = 0; k < len && *p && *p != 'E'; k++, p++)
                    {
                        if (isdigit((unsigned char)*p) && k > 2)
                        {
                            // Might be the start of the next length prefix
                            // Check if this looks like a new identifier (digit followed by alpha)
                            const char *q = p;
                            int maybe_len = 0;
                            while (isdigit((unsigned char)*q))
                            {
                                maybe_len = maybe_len * 10 + (*q - '0');
                                q++;
                            }
                            if (maybe_len > 0 && maybe_len < 200 && isalpha((unsigned char)*q))
                            {
                                // This IS the next identifier — stop the current one here
                                break;
                            }
                        }
                        id += *p;
                    }
                    if (!id.empty() && isalpha((unsigned char)id[0]))
                    {
                        ids.push_back(id);
                    }
                }
                if (ids.size() >= 2)
                {
                    class_hint = ids[ids.size() - 2];
                    method_hint = ids[ids.size() - 1];
                }
                else if (ids.size() == 1)
                {
                    method_hint = ids[0];
                }
            }

            // Now search the method index with the extracted hints
            if (!method_hint.empty())
            {
                logger::log_info("RESOLVE", "%s: Phase 0c parsed → class='%s' method='%s'",
                                 search, class_hint.c_str(), method_hint.c_str());
                auto idx_it = s_method_idx.find(method_hint);
                if (idx_it != s_method_idx.end())
                {
                    logger::log_info("RESOLVE", "%s: Phase 0c index has %zu entries for '%s'",
                                     search, idx_it->second.size(), method_hint.c_str());
                    struct Match
                    {
                        void *addr;
                        std::string mangled;
                        std::string demangled;
                    };
                    std::vector<Match> matches;
                    for (auto &ref : idx_it->second)
                    {
                        auto &sec = s_elf_sections[ref.sec_idx];
                        const Elf64_Sym &sym = sec.syms[ref.sym_idx];
                        const char *sym_name = &sec.strtab[sym.st_name];
                        // If we have a class hint, verify the symbol's class matches
                        if (!class_hint.empty())
                        {
                            if (strstr(sym_name, class_hint.c_str()) == nullptr)
                                continue;
                        }
                        int st;
                        char *d = abi::__cxa_demangle(sym_name, nullptr, nullptr, &st);
                        Match m;
                        m.addr = reinterpret_cast<void *>(s_lib_base + sym.st_value);
                        m.mangled = sym_name;
                        m.demangled = d ? d : sym_name;
                        if (d)
                            free(d);
                        matches.push_back(m);
                    }
                    if (!matches.empty())
                    {
                        auto &best = *std::min_element(matches.begin(), matches.end(),
                                                       [](const Match &a, const Match &b)
                                                       { return a.demangled.size() < b.demangled.size(); });
                        logger::log_info("RESOLVE", "%s: ELF mangled-name parse -> %s [%s] @ 0x%lX (%zu candidates)",
                                         search, best.mangled.c_str(), best.demangled.c_str(),
                                         (unsigned long)reinterpret_cast<uintptr_t>(best.addr), matches.size());
                        return best.addr;
                    }
                }
            }
            // _ZN name could not be resolved via index — fall through to Phase 1
        }

        // ── Phase 0b: underscore/:: split → method index + class filter ──
        // Converts "cObjWep_reloadable" → class="cObjWep", method="reloadable"
        // and resolves via method index instantly instead of 175ms linear scan.
        // Also handles "cItemMgr_bulletNum_ITEM_ID" with parameter type filter.
        {
            std::string search_str(name);
            std::vector<std::string> words;
            bool has_separator = false;

            if (search_str.find("::") != std::string::npos)
            {
                has_separator = true;
                size_t pos = 0;
                while (pos < search_str.size())
                {
                    size_t next = search_str.find("::", pos);
                    if (next == std::string::npos)
                    {
                        if (pos < search_str.size())
                            words.push_back(search_str.substr(pos));
                        break;
                    }
                    if (next > pos)
                        words.push_back(search_str.substr(pos, next - pos));
                    pos = next + 2;
                }
            }
            else if (search_str.find('_') != std::string::npos)
            {
                has_separator = true;
                words = split_string(search_str, '_');
            }

            if (has_separator && words.size() >= 2)
            {
                bool is_dtor = false, is_ctor = false;
                if (words.back() == "dtor" || words.back() == "destructor")
                {
                    is_dtor = true;
                    words.pop_back();
                }
                else if (words.back() == "ctor" || words.back() == "constructor")
                {
                    is_ctor = true;
                    words.pop_back();
                }

                if (!words.empty())
                {
                    std::string class_part = words[0];
                    std::string lookup_method;
                    if (is_dtor)
                    {
                        lookup_method = "~" + class_part;
                    }
                    else if (is_ctor)
                    {
                        lookup_method = class_part;
                    }
                    else if (words.size() >= 2)
                    {
                        lookup_method = words[1];
                    }

                    if (!lookup_method.empty())
                    {
                        auto idx_it = s_method_idx.find(lookup_method);
                        if (idx_it != s_method_idx.end())
                        {
                            struct DemangledMatch
                            {
                                void *addr;
                                std::string mangled;
                                std::string demangled;
                            };
                            std::vector<DemangledMatch> matches;

                            for (auto &ref : idx_it->second)
                            {
                                auto &sec = s_elf_sections[ref.sec_idx];
                                const Elf64_Sym &sym = sec.syms[ref.sym_idx];
                                const char *sym_name = &sec.strtab[sym.st_name];

                                // Parse class identifier from mangled name
                                // _ZN[qualifiers]<class_len><class_name><method_len><method_name>E...
                                if (sym_name[0] != '_' || sym_name[1] != 'Z' || sym_name[2] != 'N')
                                    continue;
                                const char *p = sym_name + 3;
                                while (*p == 'K' || *p == 'V' || *p == 'r')
                                    p++;
                                if (!isdigit((unsigned char)*p))
                                    continue;
                                int clen = 0;
                                while (isdigit((unsigned char)*p))
                                {
                                    clen = clen * 10 + (*p - '0');
                                    p++;
                                }
                                if (clen <= 0 || clen > 500)
                                    continue;
                                std::string cls_id(p, clen);

                                // Class must contain query class_part as substring
                                if (cls_id.find(class_part) == std::string::npos)
                                    continue;

                                // For parameter qualifiers (words[2+]), demangle and verify
                                if (!is_dtor && !is_ctor && words.size() > 2)
                                {
                                    int status;
                                    char *dm = abi::__cxa_demangle(sym_name, nullptr, nullptr, &status);
                                    if (!dm)
                                        continue;
                                    bool ok = true;
                                    for (size_t wi = 2; wi < words.size(); wi++)
                                    {
                                        if (!strstr(dm, words[wi].c_str()))
                                        {
                                            ok = false;
                                            break;
                                        }
                                    }
                                    free(dm);
                                    if (!ok)
                                        continue;
                                }

                                int status;
                                char *dm = abi::__cxa_demangle(sym_name, nullptr, nullptr, &status);
                                DemangledMatch m;
                                m.addr = reinterpret_cast<void *>(s_lib_base + sym.st_value);
                                m.mangled = sym_name;
                                m.demangled = dm ? dm : sym_name;
                                if (dm)
                                    free(dm);
                                matches.push_back(m);
                            }

                            if (!matches.empty())
                            {
                                // Prefer shortest demangled name (fewest params = most specific)
                                auto &best = *std::min_element(matches.begin(), matches.end(),
                                                               [](const DemangledMatch &a, const DemangledMatch &b)
                                                               {
                                                                   return a.demangled.size() < b.demangled.size();
                                                               });
                                logger::log_info("RESOLVE", "%s: ELF indexed class match -> %s [%s] @ 0x%lX (%zu candidates)",
                                                 search, best.mangled.c_str(), best.demangled.c_str(),
                                                 (unsigned long)reinterpret_cast<uintptr_t>(best.addr), matches.size());
                                return best.addr;
                            }
                        }
                    }
                }

                // Phase 0b tried method index + class filter and found nothing.
                // If the method name doesn't exist in the index at all, no linear
                // scan will find it either — the function isn't exported. Skip Phase 1.
                return nullptr;
            }
        }

        // ── Phase 1: linear scan — only for names WITHOUT separators ──
        // Catches edge cases like "AllowDebugViewmodes1" matching via substring.
        // Names with underscore/:: separators already handled by Phase 0b above.
        struct SubstringCandidate
        {
            void *addr;
            const char *sym_name;
            uint8_t sym_type;
            size_t name_len;
        };
        std::vector<SubstringCandidate> candidates;

        auto phase1_start = std::chrono::steady_clock::now();
        bool phase1_timeout = false;
        for (auto &sec : s_elf_sections)
        {
            for (size_t j = 0; j < sec.num_syms; j++)
            {
                if ((j & 0x7FFF) == 0 && j > 0)
                {
                    auto elapsed = std::chrono::steady_clock::now() - phase1_start;
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 2000)
                    {
                        logger::log_warn("RESOLVE", "%s: Phase 1 TIMEOUT after 2s at symbol %zu/%zu",
                                         search, j, sec.num_syms);
                        phase1_timeout = true;
                        break;
                    }
                }
                const Elf64_Sym &sym = sec.syms[j];
                if (sym.st_name == 0 || sym.st_name >= sec.strtab_size)
                    continue;
                if (sym.st_shndx == SHN_UNDEF)
                    continue;
                if (sym.st_value == 0)
                    continue;

                uint8_t type = ELF64_ST_TYPE(sym.st_info);
                if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)
                    continue;

                const char *sym_name = &sec.strtab[sym.st_name];

                size_t remaining = sec.strtab_size - sym.st_name;
                if (remaining == 0 || strnlen(sym_name, std::min(remaining, (size_t)4096)) >= std::min(remaining, (size_t)4096))
                    continue;

                if (strcmp(sym_name, search) == 0)
                {
                    void *result = reinterpret_cast<void *>(s_lib_base + sym.st_value);
                    logger::log_info("RESOLVE", "%s: ELF exact match -> %s @ 0x%lX",
                                     search, sym_name, (unsigned long)(s_lib_base + sym.st_value));
                    return result;
                }

                if (strstr(sym_name, search) != nullptr)
                {
                    if (is_ue4_metadata_symbol(sym_name))
                        continue;
                    SubstringCandidate c;
                    c.addr = reinterpret_cast<void *>(s_lib_base + sym.st_value);
                    c.sym_name = sym_name;
                    c.sym_type = type;
                    c.name_len = strlen(sym_name);
                    candidates.push_back(c);
                }
            }
            if (phase1_timeout)
                break;
        }

        if (!candidates.empty())
        {
            auto &best = *std::min_element(candidates.begin(), candidates.end(),
                                           [](const SubstringCandidate &a, const SubstringCandidate &b)
                                           {
                                               bool a_func = (a.sym_type == STT_FUNC);
                                               bool b_func = (b.sym_type == STT_FUNC);
                                               if (a_func != b_func)
                                                   return a_func;
                                               return a.name_len < b.name_len;
                                           });
            logger::log_info("RESOLVE", "%s: ELF substring match -> %s @ 0x%lX (%zu candidates, %zu filtered)",
                             search, best.sym_name, (unsigned long)reinterpret_cast<uintptr_t>(best.addr),
                             candidates.size(), candidates.size());
            return best.addr;
        }

        return nullptr;
    }

    // ═══ Find libUE4.so base address ════════════════════════════════════════
    struct BaseCtx
    {
        uintptr_t base;
        bool found;
    };

    static int base_callback(struct dl_phdr_info *info, size_t, void *data)
    {
        auto *ctx = static_cast<BaseCtx *>(data);
        if (info->dlpi_name)
        {
            const std::string &lib_name = game_profile::engine_lib_name();
            if (strstr(info->dlpi_name, lib_name.c_str()))
            {
                ctx->base = info->dlpi_addr;
                ctx->found = true;
                return 1;
            }
            // Also check alternate names for unknown games
            if (game_profile::detected_game() == game_profile::GameID::UNKNOWN)
            {
                if (strstr(info->dlpi_name, "libUE4.so") || strstr(info->dlpi_name, "libUnreal.so"))
                {
                    ctx->base = info->dlpi_addr;
                    ctx->found = true;
                    return 1;
                }
            }
        }
        return 0;
    }

    void init()
    {
        const std::string &lib_name = game_profile::engine_lib_name();

        // Open engine library (game-specific name)
        s_lib_handle = dlopen(lib_name.c_str(), RTLD_NOLOAD | RTLD_NOW);
        if (!s_lib_handle)
        {
            s_lib_handle = dlopen(lib_name.c_str(), RTLD_NOW);
        }
        if (!s_lib_handle)
        {
            // Try alternate names for unknown games
            if (game_profile::detected_game() == game_profile::GameID::UNKNOWN)
            {
                s_lib_handle = dlopen("libUnreal.so", RTLD_NOLOAD | RTLD_NOW);
                if (!s_lib_handle)
                    s_lib_handle = dlopen("libUE4.so", RTLD_NOLOAD | RTLD_NOW);
            }
        }
        if (!s_lib_handle)
        {
            logger::log_error("SYMBOL", "Failed to open %s: %s", lib_name.c_str(), dlerror());
            return;
        }

        // Find base address via dl_iterate_phdr
        BaseCtx base_ctx;
        base_ctx.base = 0;
        base_ctx.found = false;
        dl_iterate_phdr(base_callback, &base_ctx);

        if (base_ctx.found)
        {
            s_lib_base = base_ctx.base;
            logger::log_info("SYMBOL", "%s base: 0x%lX", lib_name.c_str(), s_lib_base);
        }
        else
        {
            logger::log_warn("SYMBOL", "Could not find %s base via dl_iterate_phdr", lib_name.c_str());
        }

        // Register per-game fallback offsets from game profile
        // (unless the game is configured for strict dynamic-only resolution).
        const auto &profile = game_profile::profile();
        size_t registered_fallbacks = 0;
        for (const auto &fb : profile.fallback_offsets)
        {
            if (fb.offset != 0 && !disallow_hardcoded_fallbacks())
            {
                register_fallback(fb.symbol_name, fb.offset);
                registered_fallbacks++;
            }
        }

        if (disallow_hardcoded_fallbacks() && !profile.fallback_offsets.empty())
        {
            logger::log_warn("SYMBOL", "Hardcoded profile fallbacks are DISABLED for %s (dynamic-only mode)",
                             profile.display_name.c_str());
        }

        // Register stable global offsets — ALWAYS applied regardless of dynamic-only mode.
        // These are data globals (GUObjectArray, GNames, GEngine) proved stable across updates.
        size_t registered_stable = 0;
        for (const auto &sg : profile.stable_global_offsets)
        {
            if (sg.offset != 0)
            {
                register_fallback(sg.symbol_name, sg.offset);
                registered_stable++;
                logger::log_info("SYMBOL", "Stable global registered: %s @ +0x%lX",
                                 sg.symbol_name.c_str(), sg.offset);
            }
        }

        // Register per-game pattern signatures
        for (const auto &sig : profile.pattern_signatures)
        {
            register_pattern(sig.symbol_name, sig.pattern, sig.rip_offset, sig.instr_size);
        }

        // Register per-game relative offsets (anchor + delta resolution)
        for (const auto &rel : profile.relative_offsets)
        {
            register_relative_offset(rel.target_name, rel.anchor_name, rel.delta);
        }

        logger::log_info("SYMBOL", "Registered %zu/%zu fallback offsets, %zu stable globals, %zu patterns, %zu relative offsets from %s profile",
                         registered_fallbacks, profile.fallback_offsets.size(),
                         registered_stable, profile.pattern_signatures.size(),
                         profile.relative_offsets.size(),
                         profile.display_name.c_str());

        // Initialize the ELF symbol cache for fast scanning
        init_elf_cache();
    }

    static void *try_runtime_discovery(const std::string &name)
    {
        uintptr_t discovered = 0;
        std::string canonical = name;

        if (name == "_ZN7UObject12ProcessEventEP9UFunctionPv" || name == "ProcessEvent")
        {
            discovered = auto_offsets::find_process_event();
            canonical = "ProcessEvent";
        }
        else if (name == "GUObjectArray")
        {
            discovered = auto_offsets::find_guobjectarray();
            canonical = "GUObjectArray";
        }
        else if (name == "GNames" || name == "_ZL6GNames" || name == "GNamePool" || name == "NamePoolData")
        {
            discovered = auto_offsets::find_gnames();
            canonical = "GNames";
        }
        else if (name == "GEngine")
        {
            discovered = auto_offsets::find_gengine();
            canonical = "GEngine";
        }
        else if (name == "GWorld")
        {
            discovered = auto_offsets::find_gworld();
            canonical = "GWorld";
        }
        else if (name == "StaticFindObject")
        {
            discovered = auto_offsets::find_static_find_object();
            canonical = "StaticFindObject";
        }
        else if (name == "StaticConstructObject_Internal")
        {
            discovered = auto_offsets::find_static_construct_object();
            canonical = "StaticConstructObject_Internal";
        }
        else if (name == "FName::Init")
        {
            discovered = auto_offsets::find_fname_init();
            canonical = "FName::Init";
        }

        if (discovered == 0)
            return nullptr;

        // Convert runtime absolute address to an offset fallback so aliases can reuse it.
        if (s_lib_base != 0 && discovered >= s_lib_base)
        {
            uintptr_t offset = discovered - s_lib_base;
            register_fallback(canonical, offset);
            register_fallback(name, offset);
            logger::log_info("RESOLVE", "%s: runtime-discovered %s @ 0x%lX (offset 0x%lX)",
                             name.c_str(), canonical.c_str(), discovered, offset);
        }
        else
        {
            logger::log_info("RESOLVE", "%s: runtime-discovered absolute @ 0x%lX",
                             name.c_str(), discovered);
        }

        return reinterpret_cast<void *>(discovered);
    }

    void *resolve(const std::string &name)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);

        // Check cache first
        auto it = s_cache.find(name);
        if (it != s_cache.end())
            return it->second;

        void *result = nullptr;

        // Priority 1: dlsym
        logger::log_info("RESOLVE", "%s: trying dlsym...", name.c_str());
        if (s_lib_handle)
        {
            result = dlsym(s_lib_handle, name.c_str());
        }
        if (result)
        {
            logger::log_info("RESOLVE", "%s: resolved via dlsym @ 0x%lX",
                             name.c_str(), reinterpret_cast<uintptr_t>(result));
            s_cache[name] = result;
            return result;
        }
        logger::log_warn("RESOLVE", "%s: dlsym failed", name.c_str());

        // Priority 2: ELF symbol table scan (.dynsym + .symtab with substring and demangled matching)
        logger::log_info("RESOLVE", "%s: trying ELF scan...", name.c_str());
        result = elf_scan(name);
        if (result)
        {
            s_cache[name] = result;
            return result;
        }
        logger::log_warn("RESOLVE", "%s: ELF scan failed", name.c_str());

        // Priority 3: Pattern scan
        auto pat_it = s_patterns.find(name);
        if (pat_it != s_patterns.end())
        {
            logger::log_info("RESOLVE", "%s: trying pattern scan...", name.c_str());
            const auto &spec = pat_it->second;
            if (spec.rip_offset >= 0)
            {
                result = pattern::scan_rip(spec.pattern, spec.rip_offset, spec.instr_size);
            }
            else
            {
                result = pattern::scan(spec.pattern);
            }
            if (result)
            {
                logger::log_info("RESOLVE", "%s: resolved via pattern scan @ 0x%lX",
                                 name.c_str(), reinterpret_cast<uintptr_t>(result));
                s_cache[name] = result;
                return result;
            }
            logger::log_warn("RESOLVE", "%s: pattern scan failed", name.c_str());
        }

        // Priority 3.25: Relative offset resolution — find via anchor + delta
        // For functions that lack unique AOB patterns, resolve via a nearby
        // confirmed function within the same compilation unit.
        auto rel_it = s_relative_offsets.find(name);
        if (rel_it != s_relative_offsets.end())
        {
            const auto &anchor_name = rel_it->second.first;
            int64_t delta = rel_it->second.second;
            logger::log_info("RESOLVE", "%s: trying relative offset from %s (delta=0x%lX)...",
                             name.c_str(), anchor_name.c_str(), static_cast<long>(delta));
            void *anchor = resolve(anchor_name);
            if (anchor)
            {
                result = reinterpret_cast<void *>(
                    reinterpret_cast<uintptr_t>(anchor) + delta);
                logger::log_info("RESOLVE", "%s: resolved via relative offset from %s + 0x%lX => 0x%lX",
                                 name.c_str(), anchor_name.c_str(),
                                 static_cast<long>(delta),
                                 reinterpret_cast<uintptr_t>(result));
                s_cache[name] = result;
                return result;
            }
            logger::log_warn("RESOLVE", "%s: relative offset failed (anchor %s not resolved)",
                             name.c_str(), anchor_name.c_str());
        }

        // Priority 3.5: On-demand runtime memory discovery (auto-offset scanner)
        // This rescans live mapped memory/string-xrefs and registers discovered
        // offsets immediately, so stripped builds can still resolve core symbols.
        logger::log_info("RESOLVE", "%s: trying runtime auto-discovery...", name.c_str());
        result = try_runtime_discovery(name);
        if (result)
        {
            s_cache[name] = result;
            return result;
        }
        logger::log_warn("RESOLVE", "%s: runtime auto-discovery failed", name.c_str());

        // Priority 4: Registered fallback offset (dynamic-discovered or profile-provided)
        auto fb_it = s_fallbacks.find(name);
        if (fb_it != s_fallbacks.end() && s_lib_base != 0 && fb_it->second != 0)
        {
            result = reinterpret_cast<void *>(s_lib_base + fb_it->second);
            logger::log_warn("RESOLVE", "%s: all dynamic methods failed — using registered fallback offset 0x%lX => 0x%lX",
                             name.c_str(), fb_it->second, reinterpret_cast<uintptr_t>(result));
            logger::log_warn("RESOLVE", "Fallback offsets are less stable than direct dynamic resolution");
            s_cache[name] = result;
            return result;
        }

        // Priority 5: Give up
        logger::log_error("RESOLVE", "%s: FAILED — symbol not found by any method", name.c_str());
        return nullptr;
    }

    void *resolve_with_fallback(const std::string &name, uintptr_t fallback_offset)
    {
        if (disallow_hardcoded_fallbacks())
        {
            logger::log_warn("RESOLVE", "%s: ignoring explicit hardcoded fallback in dynamic-only mode", name.c_str());
            return resolve(name);
        }

        if (fallback_offset != 0)
        {
            register_fallback(name, fallback_offset);
        }
        return resolve(name);
    }

    void register_fallback(const std::string &name, uintptr_t offset)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_fallbacks[name] = offset;
    }

    void register_pattern(const std::string &name,
                          const std::string &pat,
                          int rip_offset,
                          int instr_size)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        PatternSpec spec;
        spec.pattern = pat;
        spec.rip_offset = rip_offset;
        spec.instr_size = instr_size;
        s_patterns[name] = std::move(spec);
    }

    void register_relative_offset(const std::string &target_name,
                                  const std::string &anchor_name,
                                  int64_t delta)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_relative_offsets[target_name] = std::make_pair(anchor_name, delta);
    }

    uintptr_t lib_base() { return s_lib_base; }
    void *lib_handle() { return s_lib_handle; }
    void *get_lib_base() { return reinterpret_cast<void *>(s_lib_base); }

    bool is_resolved(const std::string &name)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        return s_cache.find(name) != s_cache.end();
    }

    void *get_cached(const std::string &name)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        auto it = s_cache.find(name);
        return (it != s_cache.end()) ? it->second : nullptr;
    }

    void resolve_core_symbols()
    {
        logger::log_info("SYMBOL", "Resolving core symbols...");

        // ProcessEvent — try simple name first (uses pattern scan which is more
        // reliable than string-xref auto-discovery for stripped binaries)
        void *pe = resolve("ProcessEvent");
        if (!pe)
            pe = resolve("_ZN7UObject12ProcessEventEP9UFunctionPv");
        ProcessEvent = reinterpret_cast<ue::ProcessEventFn>(pe);
        if (ProcessEvent)
        {
            logger::log_info("SYMBOL", "ProcessEvent resolved: 0x%lX", reinterpret_cast<uintptr_t>(pe));
        }
        else
        {
            logger::log_error("SYMBOL", "ProcessEvent NOT FOUND — modloader cannot function");
        }

        // StaticFindObject — fallback offset registered in init()
        void *sfo = resolve("StaticFindObject");
        StaticFindObject = reinterpret_cast<ue::StaticFindObjectFn>(sfo);
        logger::log_info("SYMBOL", "StaticFindObject: %s", sfo ? "OK" : "NOT FOUND");

        // StaticLoadObject — fallback offset registered in init()
        void *slo = resolve("StaticLoadObject");
        StaticLoadObject = reinterpret_cast<ue::StaticLoadObjectFn>(slo);
        logger::log_info("SYMBOL", "StaticLoadObject: %s", slo ? "OK" : "NOT FOUND");

        // StaticLoadClass — try native symbol first, fall back to C++ wrapper
        // In stripped UE5 binaries, StaticLoadClass is inlined/stripped.
        // It's just: return (UClass*)StaticLoadObject(BaseClass, Outer, Name, Filename, Flags|0x20000, Sandbox, false);
        void *slc = resolve("StaticLoadClass");
        if (!slc && StaticLoadObject)
        {
            // Implement as a trampoline — use a static lambda to match the signature
            // LOAD_DisableCompileOnLoad = 0x20000
            static ue::StaticLoadObjectFn s_slo_for_slc = nullptr;
            s_slo_for_slc = StaticLoadObject;
            static auto slc_wrapper = [](ue::UClass *base_cls, ue::UObject *outer,
                                         const char16_t *name, const char16_t *filename,
                                         uint32_t load_flags, void *sandbox) -> ue::UClass *
            {
                if (!s_slo_for_slc)
                    return nullptr;
                return reinterpret_cast<ue::UClass *>(
                    s_slo_for_slc(base_cls, outer, name, filename,
                                  load_flags | 0x20000, sandbox, false));
            };
            // Store as a function pointer via a converting lambda — since we can't
            // directly cast a capturing lambda, use the static storage trick above
            StaticLoadClass = reinterpret_cast<ue::StaticLoadClassFn>(&slc_wrapper);
            logger::log_info("SYMBOL", "StaticLoadClass: OK (C++ wrapper around StaticLoadObject)");
        }
        else
        {
            StaticLoadClass = reinterpret_cast<ue::StaticLoadClassFn>(slc);
            logger::log_info("SYMBOL", "StaticLoadClass: %s", slc ? "OK (native)" : "NOT FOUND");
        }

        // StaticConstructObject_Internal — fallback offset registered in init()
        void *sco = resolve("StaticConstructObject_Internal");
        StaticConstructObject = reinterpret_cast<ue::StaticConstructObjectFn>(sco);
        logger::log_info("SYMBOL", "StaticConstructObject: %s", sco ? "OK" : "NOT FOUND");

        // GetTransientPackage
        void *gtp = resolve("_Z19GetTransientPackagev");
        if (!gtp)
            gtp = resolve("GetTransientPackage");
        GetTransientPackage = reinterpret_cast<GetTransientPackageFn>(gtp);
        logger::log_info("SYMBOL", "GetTransientPackage: %s", gtp ? "OK" : "NOT FOUND");

        // PakMount — try multiple mangled forms
        void *pm = resolve("FPakPlatformFile::Mount");
        PakMount = reinterpret_cast<ue::PakMountFn>(pm);
        logger::log_info("SYMBOL", "FPakPlatformFile::Mount: %s", pm ? "OK" : "NOT FOUND");

        // GUObjectArray
        GUObjectArray = resolve("GUObjectArray");
        if (GUObjectArray)
        {
            logger::log_info("SYMBOL", "GUObjectArray resolved: 0x%lX", reinterpret_cast<uintptr_t>(GUObjectArray));
        }
        else
        {
            logger::log_error("SYMBOL", "GUObjectArray NOT FOUND — reflection walker will fail");
        }

        // GNames — try multiple names, it's often a static local
        GNames = resolve("GNames");
        if (!GNames)
            GNames = resolve("_ZL6GNames");
        if (!GNames)
            GNames = resolve("GNamePool");
        if (!GNames)
            GNames = resolve("NamePoolData");
        if (!GNames && GUObjectArray)
        {
            // Pinball FX VR fallback: GNames is at GUObjectArray - 0x47638
            // Verified on both old and new PFX VR binaries.
            // GNames is a global in .bss, not directly accessible via ADRP — it's
            // accessed through GOT indirection, making direct AOB scanning unreliable.
            // Instead, we compute it from the already-resolved GUObjectArray.
            constexpr uintptr_t PFX_GNAMES_GUOA_DELTA = 0x47638;
            GNames = reinterpret_cast<void *>(
                reinterpret_cast<uintptr_t>(GUObjectArray) - PFX_GNAMES_GUOA_DELTA);
            logger::log_info("SYMBOL", "GNames: resolved via GUObjectArray - 0x%lX => 0x%lX",
                             static_cast<unsigned long>(PFX_GNAMES_GUOA_DELTA),
                             reinterpret_cast<uintptr_t>(GNames));
        }
        if (GNames)
        {
            logger::log_info("SYMBOL", "GNames resolved: 0x%lX", reinterpret_cast<uintptr_t>(GNames));
        }
        else
        {
            logger::log_error("SYMBOL", "GNames NOT FOUND — name resolution will fail");
        }

        // GEngine
        GEngine = resolve("GEngine");
        logger::log_info("SYMBOL", "GEngine: %s", GEngine ? "OK" : "NOT FOUND (non-critical)");

        // GWorld
        GWorld = resolve("GWorld");
        logger::log_info("SYMBOL", "GWorld: %s", GWorld ? "OK" : "NOT FOUND (non-critical)");

        // ── RE4 VR specific symbols — skip on other games ──
        if (game_profile::is_re4_vr())
        {
            GetEtcModelClass = resolve("GetEtcModelClass");
            if (!GetEtcModelClass)
                GetEtcModelClass = resolve("UVR4DataSingleton::GetEtcModelClass");
            logger::log_info("SYMBOL", "GetEtcModelClass: %s", GetEtcModelClass ? "OK" : "NOT FOUND (RE4-specific)");
        }
        else
        {
            logger::log_info("SYMBOL", "GetEtcModelClass: SKIPPED (RE4 VR only)");
        }

        // FName::Init — used for constructing FNames from strings at runtime
        // Mangled: _ZN5FNameC1EPKcj or FName::FName(wchar_t const*, int)
        void *fni = resolve("FName::Init");
        if (!fni)
            fni = resolve("FName::FName");
        if (!fni)
            fni = resolve("_ZN5FNameC1EPKwi");
        if (!fni)
            fni = resolve("_ZN5FNameC2EPKwi");
        FName_Init = reinterpret_cast<FName_InitFn>(fni);
        logger::log_info("SYMBOL", "FName_Init: %s", fni ? "OK" : "NOT FOUND (FName construction from string unavailable)");

        // FOutputDevice::Log — used for outputting to UE log devices
        void *fol = resolve("FOutputDevice::Log");
        if (!fol)
            fol = resolve("_ZN13FOutputDevice3LogEPKw");
        FOutputDevice_Log = reinterpret_cast<FOutputDevice_LogFn>(fol);
        logger::log_info("SYMBOL", "FOutputDevice_Log: %s", fol ? "OK" : "NOT FOUND (non-critical)");

        // ── FText raw symbols — RE4 VR only ──
        // On Pinball FX VR (UE5 stripped), these are unavailable and unnecessary.
        // PFX uses Kismet Conv_TextToString / Conv_StringToText via ProcessEvent.
        if (game_profile::is_re4_vr())
        {
            void *ftt = resolve("_ZNK5FText8ToStringEv");
            FText_ToString = reinterpret_cast<FText_ToStringFn>(ftt);
            logger::log_info("SYMBOL", "FText_ToString: %s", ftt ? "OK" : "NOT FOUND");

            void *ftf = resolve("_ZN5FText10FromStringEO7FString");
            FText_FromString = reinterpret_cast<FText_FromStringFn>(ftf);
            logger::log_info("SYMBOL", "FText_FromString: %s", ftf ? "OK" : "NOT FOUND");

            void *ftd = resolve("_ZN5FTextD2Ev");
            FText_Dtor = reinterpret_cast<FText_DtorFn>(ftd);
            logger::log_info("SYMBOL", "FText_Dtor: %s", ftd ? "OK" : "NOT FOUND");

            void *ftc = resolve("_ZN5FTextC1Ev");
            FText_Ctor = reinterpret_cast<FText_CtorFn>(ftc);
            logger::log_info("SYMBOL", "FText_Ctor: %s", ftc ? "OK" : "NOT FOUND");
        }
        else
        {
            logger::log_info("SYMBOL", "FText raw symbols: SKIPPED (using Kismet path on %s)",
                             game_profile::display_name().c_str());
        }

        int resolved = 0;
        int total = 17;
        if (ProcessEvent)
            resolved++;
        if (StaticFindObject)
            resolved++;
        if (StaticLoadObject)
            resolved++;
        if (StaticLoadClass)
            resolved++;
        if (StaticConstructObject)
            resolved++;
        if (GetTransientPackage)
            resolved++;
        if (PakMount)
            resolved++;
        if (GUObjectArray)
            resolved++;
        if (GNames)
            resolved++;
        if (GEngine)
            resolved++;
        if (GWorld)
            resolved++;
        if (FName_Init)
            resolved++;
        if (FOutputDevice_Log)
            resolved++;
        if (FText_ToString)
            resolved++;
        if (FText_FromString)
            resolved++;
        if (FText_Dtor)
            resolved++;
        if (FText_Ctor)
            resolved++;

        logger::log_info("SYMBOL", "Core symbols resolved: %d/%d", resolved, total);
    }

    // ═══ ELF symbol table dumper ════════════════════════════════════════════
    // Reads the actual ELF .dynsym and .symtab sections from the on-disk file
    // to enumerate ALL symbols — not just the ones in the dynamic export table.
    // This gives us visibility into every named function and variable.

    // Find the on-disk path of engine library by reading /proc/self/maps
    static std::string find_lib_path()
    {
        const std::string &lib_name = game_profile::engine_lib_name();
        FILE *maps = fopen("/proc/self/maps", "r");
        if (!maps)
            return "";

        char line[1024];
        std::string result;
        while (fgets(line, sizeof(line), maps))
        {
            if (strstr(line, lib_name.c_str()))
            {
                // Extract path — it's the last field after the 5th column
                // Format: addr perms offset dev inode pathname
                char *p = strrchr(line, '/');
                if (p)
                {
                    // Go back to find the start of the path
                    char *path_start = strstr(line, "/");
                    if (path_start)
                    {
                        // Trim trailing newline
                        size_t len = strlen(path_start);
                        while (len > 0 && (path_start[len - 1] == '\n' || path_start[len - 1] == '\r'))
                            path_start[--len] = '\0';
                        result = path_start;
                        break;
                    }
                }
            }
        }
        fclose(maps);
        return result;
    }

    struct ElfSymInfo
    {
        std::string name;
        uintptr_t value; // st_value (offset from base for shared libs)
        uint64_t size;
        uint8_t type;   // STT_FUNC, STT_OBJECT, etc.
        uint8_t bind;   // STB_LOCAL, STB_GLOBAL, STB_WEAK
        uint16_t shndx; // section index (SHN_UNDEF = imported)
    };

    static const char *elf_sym_type_name(uint8_t type)
    {
        switch (type)
        {
        case STT_NOTYPE:
            return "NOTYPE";
        case STT_OBJECT:
            return "OBJECT";
        case STT_FUNC:
            return "FUNC";
        case STT_SECTION:
            return "SECTION";
        case STT_FILE:
            return "FILE";
        case STT_COMMON:
            return "COMMON";
        case STT_TLS:
            return "TLS";
        default:
            return "???";
        }
    }

    static const char *elf_sym_bind_name(uint8_t bind)
    {
        switch (bind)
        {
        case STB_LOCAL:
            return "LOCAL";
        case STB_GLOBAL:
            return "GLOBAL";
        case STB_WEAK:
            return "WEAK";
        default:
            return "???";
        }
    }

    // Read symbols from an ELF section (works for both .dynsym and .symtab)
    static int read_elf_symbols(const uint8_t *elf_base, size_t elf_size,
                                const Elf64_Shdr *sym_shdr, const Elf64_Shdr *str_shdr,
                                std::vector<ElfSymInfo> &out)
    {
        if (!sym_shdr || !str_shdr)
            return 0;
        if (sym_shdr->sh_offset + sym_shdr->sh_size > elf_size)
            return 0;
        if (str_shdr->sh_offset + str_shdr->sh_size > elf_size)
            return 0;

        const char *strtab = reinterpret_cast<const char *>(elf_base + str_shdr->sh_offset);
        size_t strtab_size = str_shdr->sh_size;

        const Elf64_Sym *syms = reinterpret_cast<const Elf64_Sym *>(elf_base + sym_shdr->sh_offset);
        size_t num_syms = sym_shdr->sh_size / sizeof(Elf64_Sym);

        int count = 0;
        for (size_t i = 0; i < num_syms; i++)
        {
            const Elf64_Sym &sym = syms[i];
            if (sym.st_name == 0)
                continue; // skip unnamed
            if (sym.st_name >= strtab_size)
                continue; // bounds check

            ElfSymInfo info;
            info.name = &strtab[sym.st_name];
            info.value = sym.st_value;
            info.size = sym.st_size;
            info.type = ELF64_ST_TYPE(sym.st_info);
            info.bind = ELF64_ST_BIND(sym.st_info);
            info.shndx = sym.st_shndx;

            if (info.name.empty())
                continue;

            out.push_back(info);
            count++;
        }
        return count;
    }

    int dump_symbols(const std::string &output_path)
    {
        std::string lib_path = find_lib_path();
        if (lib_path.empty())
        {
            logger::log_error("SYMDUMP", "Cannot find %s path in /proc/self/maps",
                              game_profile::engine_lib_name().c_str());
            return -1;
        }
        logger::log_info("SYMDUMP", "Reading ELF from: %s", lib_path.c_str());

        // Memory-map the file for reading
        int fd = open(lib_path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            logger::log_error("SYMDUMP", "Cannot open %s: %s", lib_path.c_str(), strerror(errno));
            return -1;
        }

        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            logger::log_error("SYMDUMP", "Cannot stat %s", lib_path.c_str());
            close(fd);
            return -1;
        }
        size_t file_size = static_cast<size_t>(st.st_size);

        void *map = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED)
        {
            logger::log_error("SYMDUMP", "Cannot mmap %s", lib_path.c_str());
            return -1;
        }

        const uint8_t *elf_base = static_cast<const uint8_t *>(map);

        // Validate ELF header
        const Elf64_Ehdr *ehdr = reinterpret_cast<const Elf64_Ehdr *>(elf_base);
        if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
        {
            logger::log_error("SYMDUMP", "Not a valid ELF file");
            munmap(map, file_size);
            return -1;
        }
        if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        {
            logger::log_error("SYMDUMP", "Not a 64-bit ELF");
            munmap(map, file_size);
            return -1;
        }

        logger::log_info("SYMDUMP", "ELF: %d section headers at offset 0x%lX",
                         ehdr->e_shnum, (unsigned long)ehdr->e_shoff);

        // Find section header string table
        if (ehdr->e_shstrndx >= ehdr->e_shnum)
        {
            logger::log_error("SYMDUMP", "Invalid shstrndx");
            munmap(map, file_size);
            return -1;
        }

        const Elf64_Shdr *shdrs = reinterpret_cast<const Elf64_Shdr *>(elf_base + ehdr->e_shoff);
        const Elf64_Shdr *shstr_shdr = &shdrs[ehdr->e_shstrndx];
        const char *shstrtab = reinterpret_cast<const char *>(elf_base + shstr_shdr->sh_offset);

        // Find .dynsym, .dynstr, .symtab, .strtab sections
        const Elf64_Shdr *dynsym_shdr = nullptr;
        const Elf64_Shdr *dynstr_shdr = nullptr;
        const Elf64_Shdr *symtab_shdr = nullptr;
        const Elf64_Shdr *strtab_shdr = nullptr;

        for (int i = 0; i < ehdr->e_shnum; i++)
        {
            const char *name = shstrtab + shdrs[i].sh_name;
            if (strcmp(name, ".dynsym") == 0)
                dynsym_shdr = &shdrs[i];
            if (strcmp(name, ".dynstr") == 0)
                dynstr_shdr = &shdrs[i];
            if (strcmp(name, ".symtab") == 0)
                symtab_shdr = &shdrs[i];
            if (strcmp(name, ".strtab") == 0)
                strtab_shdr = &shdrs[i];
        }

        std::vector<ElfSymInfo> all_symbols;

        // Read .dynsym (dynamic exports — always present)
        int dynsym_count = 0;
        if (dynsym_shdr && dynstr_shdr)
        {
            dynsym_count = read_elf_symbols(elf_base, file_size, dynsym_shdr, dynstr_shdr, all_symbols);
            logger::log_info("SYMDUMP", ".dynsym: %d symbols", dynsym_count);
        }
        else
        {
            logger::log_warn("SYMDUMP", ".dynsym section not found");
        }

        // Read .symtab (full symbol table — only present if unstripped)
        int symtab_count = 0;
        if (symtab_shdr && strtab_shdr)
        {
            symtab_count = read_elf_symbols(elf_base, file_size, symtab_shdr, strtab_shdr, all_symbols);
            logger::log_info("SYMDUMP", ".symtab: %d symbols", symtab_count);
        }
        else
        {
            logger::log_warn("SYMDUMP", ".symtab section not found (binary may be stripped)");
        }

        munmap(map, file_size);

        if (all_symbols.empty())
        {
            logger::log_error("SYMDUMP", "No symbols found");
            return 0;
        }

        // Sort by value (address offset)
        std::sort(all_symbols.begin(), all_symbols.end(),
                  [](const ElfSymInfo &a, const ElfSymInfo &b)
                  {
                      return a.value < b.value;
                  });

        // Write to output file
        FILE *out = fopen(output_path.c_str(), "w");
        if (!out)
        {
            logger::log_error("SYMDUMP", "Cannot open output file: %s (errno=%d: %s)",
                              output_path.c_str(), errno, strerror(errno));
            return -1;
        }

        fprintf(out, "# %s Symbol Dump\n", game_profile::engine_lib_name().c_str());
        fprintf(out, "# Game: %s\n", game_profile::display_name().c_str());
        fprintf(out, "# Source: %s\n", lib_path.c_str());
        fprintf(out, "# Base address: 0x%lX\n", (unsigned long)s_lib_base);
        fprintf(out, "# .dynsym: %d symbols\n", dynsym_count);
        fprintf(out, "# .symtab: %d symbols\n", symtab_count);
        fprintf(out, "# Total: %zu symbols\n", all_symbols.size());
        fprintf(out, "#\n");
        fprintf(out, "# Format: OFFSET | SIZE | TYPE | BIND | SECTION | NAME\n");
        fprintf(out, "#\n");

        int func_count = 0;
        int obj_count = 0;
        int other_count = 0;
        int defined_count = 0;

        for (const auto &sym : all_symbols)
        {
            const char *section;
            if (sym.shndx == SHN_UNDEF)
                section = "UNDEF";
            else if (sym.shndx == SHN_ABS)
                section = "ABS";
            else if (sym.shndx == SHN_COMMON)
                section = "COMMON";
            else
                section = "DEFINED";

            fprintf(out, "0x%012lX | %8lu | %-7s | %-6s | %-7s | %s\n",
                    (unsigned long)sym.value,
                    (unsigned long)sym.size,
                    elf_sym_type_name(sym.type),
                    elf_sym_bind_name(sym.bind),
                    section,
                    sym.name.c_str());

            if (sym.type == STT_FUNC)
                func_count++;
            else if (sym.type == STT_OBJECT)
                obj_count++;
            else
                other_count++;

            if (sym.shndx != SHN_UNDEF)
                defined_count++;
        }

        fprintf(out, "#\n");
        fprintf(out, "# Summary: %d FUNC, %d OBJECT, %d other\n", func_count, obj_count, other_count);
        fprintf(out, "# Defined: %d, Undefined (imports): %zu\n",
                defined_count, all_symbols.size() - defined_count);

        fflush(out);
        fclose(out);

        logger::log_info("SYMDUMP", "Dumped %zu symbols to %s (%d FUNC, %d OBJECT, %d defined)",
                         all_symbols.size(), output_path.c_str(), func_count, obj_count, defined_count);

        return static_cast<int>(all_symbols.size());
    }

} // namespace symbols
