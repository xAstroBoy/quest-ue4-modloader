// modloader/src/core/pattern_scanner.cpp
// AOB/pattern scanner for engine library mapped memory.
// Supports any engine library (libUE4.so, libUnreal.so, etc.) via game profile.
// Reads /proc/self/maps to find the executable and data segments
// Pattern format: "48 8B 05 ?? ?? ?? ?? 48 85 C0" where ?? = wildcard

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // for memmem on Android/Bionic
#endif
#include "modloader/pattern_scanner.h"
#include "modloader/game_profile.h"
#include "modloader/logger.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string.h> // for memmem
#include <vector>
#include <sstream>
#include <string>

namespace pattern
{

    struct MemRegion
    {
        uintptr_t start;
        uintptr_t end;
        bool executable;
        bool readable;
        bool writable;
    };

    static std::vector<MemRegion> s_regions;
    static uintptr_t s_text_start = 0;
    static uintptr_t s_text_end = 0;
    static uintptr_t s_data_start = 0;
    static uintptr_t s_data_end = 0;

    static void init_for_lib(const std::string &lib_name)
    {
        s_regions.clear();
        s_text_start = s_text_end = 0;
        s_data_start = s_data_end = 0;

        FILE *maps = fopen("/proc/self/maps", "r");
        if (!maps)
        {
            logger::log_error("PATTERN", "Failed to open /proc/self/maps");
            return;
        }

        char line[512];
        bool found_lib = false;
        while (fgets(line, sizeof(line), maps))
        {
            // Match the engine library name from game profile
            if (!strstr(line, lib_name.c_str()))
                continue;
            found_lib = true;

            uintptr_t start, end;
            char perms[5];
            if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
                continue;

            MemRegion region;
            region.start = start;
            region.end = end;
            region.readable = (perms[0] == 'r');
            region.writable = (perms[1] == 'w');
            region.executable = (perms[2] == 'x');
            s_regions.push_back(region);

            if (region.executable)
            {
                if (s_text_start == 0 || start < s_text_start)
                    s_text_start = start;
                if (end > s_text_end)
                    s_text_end = end;
            }
            else if (region.readable && region.writable)
            {
                if (s_data_start == 0 || start < s_data_start)
                    s_data_start = start;
                if (end > s_data_end)
                    s_data_end = end;
            }
        }
        fclose(maps);

        if (found_lib)
        {
            logger::log_info("PATTERN", "%s mapped: .text 0x%lX-0x%lX, .data 0x%lX-0x%lX (%zu regions)",
                             lib_name.c_str(), s_text_start, s_text_end, s_data_start, s_data_end, s_regions.size());
        }
        else
        {
            logger::log_error("PATTERN", "%s not found in /proc/self/maps", lib_name.c_str());
        }
    }

    void init()
    {
        // Use game profile to determine which library to scan
        init_for_lib(game_profile::engine_lib_name());
    }

    void init(const std::string &lib_name)
    {
        init_for_lib(lib_name);
    }

    struct PatternByte
    {
        uint8_t value;
        bool wildcard;
    };

    static std::vector<PatternByte> parse_pattern(const std::string &pattern_str)
    {
        std::vector<PatternByte> result;
        std::istringstream iss(pattern_str);
        std::string token;
        while (iss >> token)
        {
            PatternByte pb;
            if (token == "??" || token == "?")
            {
                pb.value = 0;
                pb.wildcard = true;
            }
            else
            {
                pb.value = static_cast<uint8_t>(strtoul(token.c_str(), nullptr, 16));
                pb.wildcard = false;
            }
            result.push_back(pb);
        }
        return result;
    }

    static void *scan_region(uintptr_t start, uintptr_t end, const std::vector<PatternByte> &bytes)
    {
        if (bytes.empty())
            return nullptr;
        size_t pat_len = bytes.size();
        if (end - start < pat_len)
            return nullptr;

        const uint8_t *mem = reinterpret_cast<const uint8_t *>(start);
        size_t scan_len = end - start - pat_len;

        for (size_t i = 0; i <= scan_len; i++)
        {
            bool match = true;
            for (size_t j = 0; j < pat_len; j++)
            {
                if (!bytes[j].wildcard && mem[i + j] != bytes[j].value)
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return reinterpret_cast<void *>(start + i);
            }
        }
        return nullptr;
    }

    void *scan(const std::string &pattern_str)
    {
        auto bytes = parse_pattern(pattern_str);
        if (bytes.empty())
            return nullptr;

        // Scan all readable regions of the engine library
        for (const auto &region : s_regions)
        {
            if (!region.readable)
                continue;
            void *result = scan_region(region.start, region.end, bytes);
            if (result)
            {
                logger::log_info("PATTERN", "Pattern match at 0x%lX in region 0x%lX-0x%lX",
                                 reinterpret_cast<uintptr_t>(result), region.start, region.end);
                return result;
            }
        }

        logger::log_warn("PATTERN", "Pattern not found: %s", pattern_str.c_str());
        return nullptr;
    }

    void *scan_rip(const std::string &pattern_str, int rip_offset, int instr_size)
    {
        void *match = scan(pattern_str);
        if (!match)
            return nullptr;

        // For ARM64: handle ADRP + ADD pair or LDR literal
        // rip_offset is the offset into the matched pattern where the PC-relative encoding starts
        // instr_size is the total instruction group size
        uintptr_t instr_addr = reinterpret_cast<uintptr_t>(match) + rip_offset;
        uint32_t instr = *reinterpret_cast<uint32_t *>(instr_addr);

        // Check if ADRP (top 5 bits = 10x10 where x=op)
        if ((instr & 0x1F000000) == 0x10000000)
        {
            // ADRP: imm = immhi:immlo, shifted left 12
            int32_t immhi = static_cast<int32_t>((instr >> 5) & 0x7FFFF);
            int32_t immlo = static_cast<int32_t>((instr >> 29) & 0x3);
            int32_t imm = (immhi << 2) | immlo;
            // Sign extend from 21 bits
            if (imm & (1 << 20))
                imm |= ~((1 << 21) - 1);
            uintptr_t page_base = (instr_addr & ~0xFFF) + (static_cast<int64_t>(imm) << 12);

            // Check for a following ADD immediate
            if (rip_offset + 4 < instr_size)
            {
                uint32_t add_instr = *reinterpret_cast<uint32_t *>(instr_addr + 4);
                if ((add_instr & 0xFF000000) == 0x91000000)
                {
                    uint32_t add_imm = (add_instr >> 10) & 0xFFF;
                    uint32_t shift = (add_instr >> 22) & 0x3;
                    if (shift == 1)
                        add_imm <<= 12;
                    return reinterpret_cast<void *>(page_base + add_imm);
                }
            }
            return reinterpret_cast<void *>(page_base);
        }

        // LDR literal (PC-relative)
        if ((instr & 0x3B000000) == 0x18000000)
        {
            int32_t imm19 = static_cast<int32_t>((instr >> 5) & 0x7FFFF);
            if (imm19 & (1 << 18))
                imm19 |= ~((1 << 19) - 1);
            uintptr_t target = instr_addr + (static_cast<int64_t>(imm19) << 2);
            return reinterpret_cast<void *>(target);
        }

        // Fallback: treat as simple offset from match
        logger::log_warn("PATTERN", "scan_rip: unrecognized instruction at 0x%lX, returning raw match",
                         instr_addr);
        return match;
    }

    std::vector<void *> scan_all(const std::string &pattern_str)
    {
        auto bytes = parse_pattern(pattern_str);
        std::vector<void *> results;
        if (bytes.empty())
            return results;

        for (const auto &region : s_regions)
        {
            if (!region.readable)
                continue;
            size_t pat_len = bytes.size();
            if (region.end - region.start < pat_len)
                continue;

            const uint8_t *mem = reinterpret_cast<const uint8_t *>(region.start);
            size_t scan_len = region.end - region.start - pat_len;

            for (size_t i = 0; i <= scan_len; i++)
            {
                bool match = true;
                for (size_t j = 0; j < pat_len; j++)
                {
                    if (!bytes[j].wildcard && mem[i + j] != bytes[j].value)
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                {
                    results.push_back(reinterpret_cast<void *>(region.start + i));
                }
            }
        }
        return results;
    }

    uintptr_t text_start() { return s_text_start; }
    uintptr_t text_end() { return s_text_end; }
    uintptr_t data_start() { return s_data_start; }
    uintptr_t data_end() { return s_data_end; }

    // ═══ String scanning ════════════════════════════════════════════════════
    // Search for a null-terminated string in all readable regions.
    // Used for engine version detection (searching .rodata for version markers).

    void *find_string(const char *needle)
    {
        if (!needle || !needle[0])
            return nullptr;

        size_t needle_len = strlen(needle);

        for (const auto &region : s_regions)
        {
            if (!region.readable)
                continue;
            // Skip executable-only regions (we want .rodata / .data)
            // Actually, .rodata is typically in a readable non-writable region
            size_t region_size = region.end - region.start;
            if (region_size < needle_len)
                continue;

            const char *mem = reinterpret_cast<const char *>(region.start);
            const char *result = static_cast<const char *>(
                memmem(mem, region_size, needle, needle_len));
            if (result)
            {
                logger::log_info("PATTERN", "String '%s' found at 0x%lX in region 0x%lX-0x%lX",
                                 needle, reinterpret_cast<uintptr_t>(result),
                                 region.start, region.end);
                return const_cast<void *>(static_cast<const void *>(result));
            }
        }

        logger::log_warn("PATTERN", "String '%s' not found in any region", needle);
        return nullptr;
    }

    std::vector<void *> find_string_all(const char *needle)
    {
        std::vector<void *> results;
        if (!needle || !needle[0])
            return results;

        size_t needle_len = strlen(needle);

        for (const auto &region : s_regions)
        {
            if (!region.readable)
                continue;
            size_t region_size = region.end - region.start;
            if (region_size < needle_len)
                continue;

            const char *mem = reinterpret_cast<const char *>(region.start);
            size_t offset = 0;
            while (offset <= region_size - needle_len)
            {
                const char *result = static_cast<const char *>(
                    memmem(mem + offset, region_size - offset, needle, needle_len));
                if (!result)
                    break;
                results.push_back(const_cast<void *>(static_cast<const void *>(result)));
                offset = (result - mem) + 1;
            }
        }
        return results;
    }

} // namespace pattern
