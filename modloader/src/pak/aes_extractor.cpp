// modloader/src/pak/aes_extractor.cpp
// ═══════════════════════════════════════════════════════════════════════════
// AES-256 Encryption Key Extractor for UE PAK files
// Hooks FAES::DecryptData to capture encryption keys at runtime
// ═══════════════════════════════════════════════════════════════════════════

#include "modloader/aes_extractor.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/ue_types.h"
#include "modloader/pattern_scanner.h"
#include "modloader/game_profile.h"

#include <dobby.h>
#include <dlfcn.h>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <android/log.h>

namespace aes_extractor
{

    // ═══ State ══════════════════════════════════════════════════════════════
    static std::mutex s_mutex;
    static std::vector<AESKey> s_keys;
    static std::atomic<bool> s_initialized{false};

    // Dobby trampoline for FAES::DecryptData
    static ue::faes::DecryptDataFn s_original_decrypt = nullptr;

    // ═══ Helpers ════════════════════════════════════════════════════════════

    static uint64_t now_ms()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    static bool is_key_duplicate(const uint8_t *key_bytes)
    {
        // Check if we already have this exact key
        for (const auto &k : s_keys)
        {
            if (memcmp(k.bytes, key_bytes, 32) == 0)
            {
                return true;
            }
        }
        return false;
    }

    static bool is_key_all_zeros(const uint8_t *key_bytes)
    {
        for (int i = 0; i < 32; i++)
        {
            if (key_bytes[i] != 0)
                return false;
        }
        return true;
    }

    // ═══ Hex / Base64 conversion ════════════════════════════════════════════

    std::string key_to_hex(const AESKey &key)
    {
        static const char hex[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(64);
        for (int i = 0; i < 32; i++)
        {
            result += hex[(key.bytes[i] >> 4) & 0x0F];
            result += hex[key.bytes[i] & 0x0F];
        }
        return result;
    }

    std::string key_to_base64(const AESKey &key)
    {
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(45); // ceil(32/3)*4 = 44 + null

        for (int i = 0; i < 32; i += 3)
        {
            uint32_t n = (static_cast<uint32_t>(key.bytes[i]) << 16);
            if (i + 1 < 32)
                n |= (static_cast<uint32_t>(key.bytes[i + 1]) << 8);
            if (i + 2 < 32)
                n |= static_cast<uint32_t>(key.bytes[i + 2]);

            result += b64[(n >> 18) & 0x3F];
            result += b64[(n >> 12) & 0x3F];
            result += (i + 1 < 32) ? b64[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < 32) ? b64[n & 0x3F] : '=';
        }
        return result;
    }

    // ═══ Dobby hook: FAES::DecryptData ══════════════════════════════════════
    // Signature: void DecryptData(uint8* Contents, uint64 NumBytes, const FAESKey& Key)
    // ARM64 ABI: X0=Contents, X1=NumBytes, X2=&Key
    static void hooked_decrypt_data(uint8_t *contents, uint64_t num_bytes,
                                    const ue::faes::FAESKey *key)
    {
        // Capture the key before calling original
        if (key && !is_key_all_zeros(key->Key))
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!is_key_duplicate(key->Key))
            {
                AESKey ak;
                memcpy(ak.bytes, key->Key, 32);
                ak.source = "hook:DecryptData";
                ak.timestamp = now_ms();

                // Try to guess PAK name from call context (limited without stack walking)
                ak.pak_name = "unknown";

                s_keys.push_back(ak);

                std::string hex = key_to_hex(ak);
                logger::log_info("AES", "*** KEY CAPTURED via DecryptData hook ***");
                logger::log_info("AES", "  Key (hex): %s", hex.c_str());
                logger::log_info("AES", "  Data size: %llu bytes", (unsigned long long)num_bytes);
                logger::log_info("AES", "  Total keys captured: %zu", s_keys.size());
            }
        }

        // Call original — let the engine decrypt normally
        if (s_original_decrypt)
        {
            s_original_decrypt(contents, num_bytes, key);
        }
    }

    // ═══ Find FAES::DecryptData address ═════════════════════════════════════
    static void *find_decrypt_data()
    {
        // Method 1: Try dlsym (may be exported)
        void *lib = dlopen(game_profile::engine_lib_name().c_str(), RTLD_NOLOAD);
        if (lib)
        {
            // UE4 mangled name: _ZN4FAES11DecryptDataEPhyRKNS_7FAESKeyE
            void *addr = dlsym(lib, "_ZN4FAES11DecryptDataEPhyRKNS_7FAESKeyE");
            if (!addr)
            {
                // Alternative mangling
                addr = dlsym(lib, "_ZN4FAES11DecryptDataEPhmRKNS_7FAESKeyE");
            }
            if (!addr)
            {
                // UE5 may use different mangling
                addr = dlsym(lib, "_ZN4FAES11DecryptDataEPhyRK7FAESKey");
            }
            dlclose(lib);
            if (addr)
            {
                logger::log_info("AES", "FAES::DecryptData found via dlsym @ 0x%lX",
                                 (unsigned long)reinterpret_cast<uintptr_t>(addr));
                return addr;
            }
        }

        // Method 2: Try string-based search for "DecryptData" or related
        // The function often references error strings about encryption failures
        uintptr_t base = symbols::lib_base();
        if (base != 0)
        {
            // Try pattern scanner for known function signatures
            // FAES::DecryptData typically starts with: check key != null, check size % 16 == 0
            // Then calls into AES implementation (openssl or custom)

            // Try searching for the string "FAES::DecryptData" which may appear in error messages
            auto results = pattern::find_string_all("AES");
            if (!results.empty())
            {
                logger::log_info("AES", "Found %zu 'AES' string references — may help locate DecryptData",
                                 results.size());
            }
        }

        // Method 3: Check fallback offsets from game profile
        for (const auto &fb : game_profile::profile().fallback_offsets)
        {
            if (fb.symbol_name == "FAES::DecryptData" || fb.symbol_name == "DecryptData")
            {
                void *addr = reinterpret_cast<void *>(symbols::lib_base() + fb.offset);
                logger::log_info("AES", "FAES::DecryptData from fallback offset @ 0x%lX",
                                 (unsigned long)reinterpret_cast<uintptr_t>(addr));
                return addr;
            }
        }

        logger::log_warn("AES", "FAES::DecryptData not found — AES key extraction won't work via hook");
        return nullptr;
    }

    // ═══ Init ═══════════════════════════════════════════════════════════════
    void init()
    {
        if (s_initialized.load())
            return;

        logger::log_info("AES", "Initializing AES key extractor...");

        // Try to hook FAES::DecryptData
        void *decrypt_addr = find_decrypt_data();
        if (decrypt_addr)
        {
            int status = DobbyHook(
                decrypt_addr,
                reinterpret_cast<dobby_dummy_func_t>(hooked_decrypt_data),
                reinterpret_cast<dobby_dummy_func_t *>(&s_original_decrypt));
            if (status == 0)
            {
                logger::log_info("AES", "FAES::DecryptData hooked successfully @ 0x%lX",
                                 (unsigned long)reinterpret_cast<uintptr_t>(decrypt_addr));
            }
            else
            {
                logger::log_error("AES", "Dobby failed to hook DecryptData (status=%d)", status);
            }
        }

        s_initialized.store(true);
        logger::log_info("AES", "AES extractor initialized (hook=%s)",
                         decrypt_addr ? "active" : "inactive");
    }

    // ═══ Query API ══════════════════════════════════════════════════════════

    std::vector<AESKey> get_keys()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_keys;
    }

    AESKey get_latest_key()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_keys.empty())
        {
            AESKey empty;
            memset(empty.bytes, 0, 32);
            empty.source = "none";
            empty.timestamp = 0;
            return empty;
        }
        return s_keys.back();
    }

    bool has_keys()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return !s_keys.empty();
    }

    size_t key_count()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_keys.size();
    }

    void add_key(const uint8_t *key_bytes, const std::string &source)
    {
        if (!key_bytes)
            return;
        if (is_key_all_zeros(key_bytes))
            return;

        std::lock_guard<std::mutex> lock(s_mutex);
        if (is_key_duplicate(key_bytes))
        {
            logger::log_info("AES", "Key already known (source: %s)", source.c_str());
            return;
        }

        AESKey ak;
        memcpy(ak.bytes, key_bytes, 32);
        ak.source = source;
        ak.timestamp = now_ms();
        ak.pak_name = "manual";
        s_keys.push_back(ak);

        logger::log_info("AES", "Key added manually (source: %s, total: %zu)",
                         source.c_str(), s_keys.size());
    }

    // ═══ Memory scan for AES keys ═══════════════════════════════════════════
    // Heuristic: Look for 32-byte blobs near known encryption-related strings.
    // This is a best-effort approach — may produce false positives.
    int scan_for_keys()
    {
        logger::log_info("AES", "Scanning memory for potential AES keys...");

        uintptr_t base = symbols::lib_base();
        if (base == 0)
        {
            logger::log_error("AES", "Engine lib base not available — cannot scan");
            return 0;
        }

        int found = 0;

        // Scan for string references to encryption-related terms
        const char *search_terms[] = {
            "EncryptionKey",
            "PakEncryptionKey",
            "CryptoKeys",
            "AES",
        };

        for (const char *term : search_terms)
        {
            auto refs = pattern::find_string_all(term);
            for (auto ref_ptr : refs)
            {
                uintptr_t ref_addr = reinterpret_cast<uintptr_t>(ref_ptr);
                // Check ±256 bytes around the string reference for 32-byte aligned blobs
                // that look like AES keys (high entropy, non-zero)
                for (int delta = -256; delta <= 256; delta += 8)
                {
                    uintptr_t candidate_addr = ref_addr + delta;
                    const uint8_t *candidate = reinterpret_cast<const uint8_t *>(candidate_addr);

                    // Safety check
                    if (!ue::is_mapped_ptr(candidate) || !ue::is_mapped_ptr(candidate + 31))
                    {
                        continue;
                    }

                    // Check if this looks like a key (high byte entropy)
                    int unique_bytes = 0;
                    bool seen[256] = {};
                    bool all_printable = true;
                    for (int i = 0; i < 32; i++)
                    {
                        if (!seen[candidate[i]])
                        {
                            seen[candidate[i]] = true;
                            unique_bytes++;
                        }
                        if (candidate[i] < 0x20 || candidate[i] > 0x7E)
                        {
                            all_printable = false;
                        }
                    }

                    // Skip if too low entropy (less than 16 unique bytes) or all zeros
                    // Skip if all printable ASCII (likely a string, not a key)
                    if (unique_bytes < 16 || is_key_all_zeros(candidate) || all_printable)
                    {
                        continue;
                    }

                    // This looks like a potential key
                    std::lock_guard<std::mutex> lock(s_mutex);
                    if (!is_key_duplicate(candidate))
                    {
                        AESKey ak;
                        memcpy(ak.bytes, candidate, 32);
                        ak.source = std::string("scan:") + term;
                        ak.timestamp = now_ms();
                        ak.pak_name = "scan";
                        s_keys.push_back(ak);
                        found++;

                        std::string hex = key_to_hex(ak);
                        logger::log_info("AES", "Candidate key found near '%s' @ 0x%lX: %s",
                                         term, (unsigned long)candidate_addr, hex.c_str());
                    }
                }
            }
        }

        logger::log_info("AES", "Memory scan complete — %d candidate key(s) found", found);
        return found;
    }

    // ═══ Dump keys to file ══════════════════════════════════════════════════
    void dump_keys_to_file(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        FILE *f = fopen(path.c_str(), "w");
        if (!f)
        {
            logger::log_error("AES", "Failed to open %s for writing", path.c_str());
            return;
        }

        fprintf(f, "=== AES Key Dump ===\n");
        fprintf(f, "Total keys: %zu\n\n", s_keys.size());

        for (size_t i = 0; i < s_keys.size(); i++)
        {
            const auto &k = s_keys[i];
            std::string hex = key_to_hex(k);
            std::string b64 = key_to_base64(k);

            fprintf(f, "Key #%zu:\n", i + 1);
            fprintf(f, "  Hex:       %s\n", hex.c_str());
            fprintf(f, "  Base64:    %s\n", b64.c_str());
            fprintf(f, "  Source:    %s\n", k.source.c_str());
            fprintf(f, "  PAK:       %s\n", k.pak_name.c_str());
            fprintf(f, "  Timestamp: %llu ms\n\n", (unsigned long long)k.timestamp);
        }

        fclose(f);
        logger::log_info("AES", "Keys dumped to %s (%zu keys)", path.c_str(), s_keys.size());
    }

} // namespace aes_extractor
