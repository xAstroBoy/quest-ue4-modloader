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
#include "modloader/paths.h"

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
    static std::atomic<bool> s_capture_enabled{false}; // Disabled by default — too noisy

    // ── Primary hook: T-table FAES::DecryptData (the REAL PAK decrypt) ────────
    // sub_13FBEE4: void FAES_DecryptData(void* data, int64_t size, uint8_t* key32)
    // This is UE's standalone T-table AES-256 — NOT OpenSSL.
    // arg3 (X2) points to the raw 32-byte AES key.
    typedef void (*FAESDecryptDataFn)(void *data, int64_t size, const uint8_t *key32);
    static FAESDecryptDataFn s_original_faes_decrypt = nullptr;

    // ── Secondary hooks: OpenSSL AES key-schedule setup ─────────────────────
    // These catch TLS/OpenSSL AES usage but NOT PAK decryption.
    typedef int (*AESSetKeyFn)(const uint8_t *, int, void *);
    static AESSetKeyFn s_original_aes_set_decrypt_key = nullptr;
    static AESSetKeyFn s_original_aes_set_encrypt_key = nullptr;

    // Legacy: FAES::DecryptData with FAESKey struct (UE4-style, dlsym path)
    static ue::faes::DecryptDataFn s_original_decrypt = nullptr;

    // Hit count per key — PAK keys are seen repeatedly (once per .pak), TLS keys are ephemeral
    struct KeyHitEntry
    {
        uint8_t key[32];
        int hits;
    };
    static std::vector<KeyHitEntry> s_hit_counts;
    static uint64_t s_init_time = 0; // timestamp when init() was called

    // Increment hit count for a key, return new count
    static int increment_hit_count(const uint8_t *key_bytes)
    {
        for (auto &entry : s_hit_counts)
        {
            if (memcmp(entry.key, key_bytes, 32) == 0)
            {
                return ++entry.hits;
            }
        }
        // New key
        KeyHitEntry e;
        memcpy(e.key, key_bytes, 32);
        e.hits = 1;
        s_hit_counts.push_back(e);
        return 1;
    }

    // Get hit count for a key
    static int get_hit_count(const uint8_t *key_bytes)
    {
        for (const auto &entry : s_hit_counts)
        {
            if (memcmp(entry.key, key_bytes, 32) == 0)
                return entry.hits;
        }
        return 0;
    }

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

    // ═══ Core: capture and log a key (shared by all hooks) ═════════════════
    static void capture_key(const uint8_t *key_bytes, const char *source, int bits)
    {
        // AES capture disabled by default — too noisy in logs
        if (!s_capture_enabled)
            return;

        if (!key_bytes || is_key_all_zeros(key_bytes))
            return;

        std::lock_guard<std::mutex> lock(s_mutex);

        // Always track hit count (even for duplicates)
        int hits = increment_hit_count(key_bytes);

        if (is_key_duplicate(key_bytes))
        {
            // Log repeated sighting — helps identify the PAK key
            if (hits == 2 || hits == 5 || hits == 10)
            {
                AESKey tmp;
                memcpy(tmp.bytes, key_bytes, 32);
                logger::log_info("AES_KEY", "Key seen %d times (source: %s) — likely PAK key: %s",
                                 hits, source, key_to_hex(tmp).c_str());
            }
            return;
        }

        AESKey ak;
        memcpy(ak.bytes, key_bytes, 32);
        ak.source = source;
        ak.timestamp = now_ms();
        ak.pak_name = "intercepted";
        s_keys.push_back(ak);

        std::string hex = key_to_hex(ak);
        std::string b64 = key_to_base64(ak);

        // Loud, impossible-to-miss banner in the log
        logger::log_info("AES_KEY", "");
        logger::log_info("AES_KEY", "╔══════════════════════════════════════════════════════════════════════╗");
        logger::log_info("AES_KEY", "║          *** AES-%d KEY INTERCEPTED AT RUNTIME ***              ║", bits);
        logger::log_info("AES_KEY", "╠══════════════════════════════════════════════════════════════════════╣");
        logger::log_info("AES_KEY", "║  Source : %-58s  ║", source);
        logger::log_info("AES_KEY", "║  HEX    : %s  ║", hex.c_str());
        logger::log_info("AES_KEY", "║  Base64 : %-58s  ║", b64.c_str());
        logger::log_info("AES_KEY", "╚══════════════════════════════════════════════════════════════════════╝");
        logger::log_info("AES_KEY", "  Total unique AES keys captured so far: %zu", s_keys.size());
        logger::log_info("AES_KEY", "");
    }

    // ═══ Dobby hook: AES_set_decrypt_key ════════════════════════════════════
    // int AES_set_decrypt_key(const uint8_t* userKey, int bits, AES_KEY* key)
    // X0 = raw key bytes, W1 = key bits (256 for AES-256), X2 = key schedule
    static int hooked_aes_set_decrypt_key(const uint8_t *userKey, int bits, void *keySchedule)
    {
        // Only capture AES-256 (PAK encryption is always 256-bit)
        // TLS mostly uses AES-128-GCM; AES-256 TLS keys are rare and ephemeral
        if (userKey && bits == 256)
            capture_key(userKey, "hook:AES_set_decrypt_key", bits);

        if (s_original_aes_set_decrypt_key)
            return s_original_aes_set_decrypt_key(userKey, bits, keySchedule);
        return 0;
    }

    // ═══ Dobby hook: AES_set_encrypt_key ════════════════════════════════════
    // int AES_set_encrypt_key(const uint8_t* userKey, int bits, AES_KEY* key)
    // UE5 sometimes uses the "encrypt" direction for ECB PAK decryption
    static int hooked_aes_set_encrypt_key(const uint8_t *userKey, int bits, void *keySchedule)
    {
        // Only capture AES-256 (PAK encryption is always 256-bit)
        if (userKey && bits == 256)
            capture_key(userKey, "hook:AES_set_encrypt_key", bits);

        if (s_original_aes_set_encrypt_key)
            return s_original_aes_set_encrypt_key(userKey, bits, keySchedule);
        return 0;
    }

    // ── Hook: GetPakEncryptionKey (sub_228E614) ────────────────────────────
    // void GetPakEncryptionKey(uint8_t out[32], const FGuid* guid)
    // Called by FPakFile::DecryptData to resolve GUID → raw key.
    // We hook this to capture the OUTPUT key after the real function runs.
    typedef void (*GetPakEncryptionKeyFn)(uint8_t *out_key, const void *guid);
    static GetPakEncryptionKeyFn s_original_get_pak_key = nullptr;

    static void hooked_get_pak_encryption_key(uint8_t *out_key, const void *guid)
    {
        // Call the original first
        if (s_original_get_pak_key)
            s_original_get_pak_key(out_key, guid);

        // Now capture the output
        if (out_key && !is_key_all_zeros(out_key))
        {
            capture_key(out_key, "hook:GetPakEncryptionKey", 256);

            // Write to file on first capture
            static bool written = false;
            if (!written)
            {
                written = true;
                AESKey tmp;
                memcpy(tmp.bytes, out_key, 32);
                std::string hex = key_to_hex(tmp);
                std::string b64 = key_to_base64(tmp);

                std::string key_file = paths::data_dir() + "/pak_key.txt";
                FILE *kf = fopen(key_file.c_str(), "w");
                if (kf)
                {
                    fprintf(kf, "HEX=%s\n", hex.c_str());
                    fprintf(kf, "BASE64=%s\n", b64.c_str());
                    fprintf(kf, "SOURCE=hook:GetPakEncryptionKey\n");
                    fclose(kf);
                    logger::log_info("AES", "★★★ PAK key from GetPakEncryptionKey hook: %s", hex.c_str());
                    logger::log_info("AES", "★★★ Written to %s", key_file.c_str());
                }
            }
        }
    }

    // ═══ PRIMARY: T-table FAES::DecryptData (real PAK decrypt) ══════════════
    // sub_13FBEE4: void(void* data, int64_t size, uint8_t* key32)
    // X0=data, X1=size, X2=raw 32-byte key pointer
    // Called by FPakFile::DecryptData (sub_228F334) for ALL PAK decryption.
    static void hooked_faes_decrypt_data(void *data, int64_t size, const uint8_t *key32)
    {
        if (key32 && size > 0 && !is_key_all_zeros(key32))
            capture_key(key32, "hook:FAES_DecryptData", 256);

        if (s_original_faes_decrypt)
            s_original_faes_decrypt(data, size, key32);
    }

    // ═══ LEGACY: FAES::DecryptData with FAESKey struct (UE4-style) ══════════
    // Only used if found via dlsym (unlikely on stripped binary)
    static void hooked_decrypt_data(uint8_t *contents, uint64_t num_bytes,
                                    const ue::faes::FAESKey *key)
    {
        if (key && !is_key_all_zeros(key->Key))
            capture_key(key->Key, "hook:FAES::DecryptData", 256);

        if (s_original_decrypt)
            s_original_decrypt(contents, num_bytes, key);
    }

    // ═══ Direct key probe: call GetPakEncryptionKey post-init ══════════════
    // PAK decryption happens DURING dlopen(libUnreal.so) — before hooks install.
    // FAES_DecryptData is never called again after PAK indices are cached.
    // So we directly call GetPakEncryptionKey(output, guid) to extract the key.
    //
    // GetPakEncryptionKey (sub_228E614): void(uint8_t out[32], const FGuid* guid)
    // Uses FEncryptionKeyManager to look up key by GUID. Null GUID = default key.
    typedef void (*GetPakEncryptionKeyFn)(uint8_t *out_key, const void *guid);

    static void probe_encryption_key_direct()
    {
        uintptr_t base = symbols::lib_base();
        if (base == 0)
            return;

        // Find GetPakEncryptionKey offset from profile
        uintptr_t fn_addr = 0;
        for (const auto &fb : game_profile::profile().fallback_offsets)
        {
            if (fb.symbol_name == "GetPakEncryptionKey" && fb.offset != 0)
            {
                fn_addr = base + fb.offset;
                break;
            }
        }

        if (fn_addr == 0)
        {
            logger::log_info("AES", "GetPakEncryptionKey offset not in profile — skipping direct probe");
            return;
        }

        logger::log_info("AES", "Probing GetPakEncryptionKey directly @ 0x%lX...",
                         (unsigned long)fn_addr);

        auto fn = reinterpret_cast<GetPakEncryptionKeyFn>(fn_addr);

        // Try with null GUID (all zeros = default encryption key)
        uint8_t null_guid[16] = {};
        uint8_t key_buf[32] = {};

        // Wrap in signal-safe try — the function may crash if PAKs aren't encrypted
        // or if the key manager isn't initialized
        bool success = false;
        try
        {
            fn(key_buf, null_guid);
            success = true;
        }
        catch (...)
        {
            logger::log_warn("AES", "GetPakEncryptionKey threw exception — PAKs may not be encrypted");
        }

        if (success && !is_key_all_zeros(key_buf))
        {
            capture_key(key_buf, "direct:GetPakEncryptionKey", 256);
            AESKey tmp;
            memcpy(tmp.bytes, key_buf, 32);
            std::string hex = key_to_hex(tmp);
            std::string b64 = key_to_base64(tmp);
            logger::log_info("AES", "★★★ PAK encryption key extracted directly: %s", hex.c_str());
            logger::log_info("AES", "★★★ PAK key Base64: %s", b64.c_str());

            // Write to file immediately so it survives logcat rollover
            std::string key_file = paths::data_dir() + "/pak_key.txt";
            FILE *kf = fopen(key_file.c_str(), "w");
            if (kf)
            {
                fprintf(kf, "HEX=%s\n", hex.c_str());
                fprintf(kf, "BASE64=%s\n", b64.c_str());
                fprintf(kf, "SOURCE=direct:GetPakEncryptionKey\n");
                fclose(kf);
                logger::log_info("AES", "PAK key written to %s", key_file.c_str());
            }
        }
        else if (success)
        {
            logger::log_info("AES", "GetPakEncryptionKey returned all-zeros — PAKs likely not encrypted");
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

        // ── If PAK key already extracted, skip ALL hooks ────────────────────
        // The AES hooks (especially FAES_DecryptData with 11 callers) are
        // dangerous on hot paths — they intercept ALL AES operations and take
        // a mutex lock. Once the key is known, there's no reason to keep them.
        {
            std::string key_file = paths::data_dir() + "/pak_key.txt";
            FILE *kf = fopen(key_file.c_str(), "r");
            if (kf)
            {
                char buf[256] = {};
                size_t n = fread(buf, 1, sizeof(buf) - 1, kf);
                fclose(kf);
                if (n > 10 && strstr(buf, "HEX="))
                {
                    logger::log_info("AES", "PAK key already extracted (%s) — skipping AES hooks",
                                     key_file.c_str());
                    s_initialized.store(true);
                    return;
                }
            }
        }

        uintptr_t base = symbols::lib_base();
        int hooks_installed = 0;

        s_init_time = now_ms();

        // ── PRIMARY: Hook GetPakEncryptionKey (most reliable for PAK key) ────
        // This is called ONLY for PAK decryption — captures the output key.
        for (const auto &fb : game_profile::profile().fallback_offsets)
        {
            if (fb.offset == 0)
                continue;
            if (fb.symbol_name == "GetPakEncryptionKey")
            {
                void *addr = reinterpret_cast<void *>(base + fb.offset);
                int status = DobbyHook(
                    addr,
                    reinterpret_cast<dobby_dummy_func_t>(hooked_get_pak_encryption_key),
                    reinterpret_cast<dobby_dummy_func_t *>(&s_original_get_pak_key));
                if (status == 0)
                {
                    logger::log_info("AES", "★ GetPakEncryptionKey hooked @ 0x%lX [PRIMARY/PAK-ONLY]",
                                     (unsigned long)reinterpret_cast<uintptr_t>(addr));
                    hooks_installed++;
                }
                else
                {
                    logger::log_error("AES", "Dobby failed to hook GetPakEncryptionKey (status=%d)", status);
                }
            }
        }

        // ── Hook T-table FAES_DecryptData (captures all AES decrypt calls) ──
        // This is the actual AES-256 T-table implementation at sub_13FBEE4.
        // Called by FPakFile::DecryptData for ALL encrypted PAK index/content.
        // X2 = raw 32-byte key — we capture it directly.
        for (const auto &fb : game_profile::profile().fallback_offsets)
        {
            if (fb.offset == 0)
                continue;
            void *addr = reinterpret_cast<void *>(base + fb.offset);

            if (fb.symbol_name == "FAES_DecryptData")
            {
                int status = DobbyHook(
                    addr,
                    reinterpret_cast<dobby_dummy_func_t>(hooked_faes_decrypt_data),
                    reinterpret_cast<dobby_dummy_func_t *>(&s_original_faes_decrypt));
                if (status == 0)
                {
                    logger::log_info("AES", "★ FAES_DecryptData (T-table AES) hooked @ 0x%lX [PRIMARY]",
                                     (unsigned long)reinterpret_cast<uintptr_t>(addr));
                    hooks_installed++;
                }
                else
                {
                    logger::log_error("AES", "Dobby failed to hook FAES_DecryptData (status=%d)", status);
                }
            }
        }

        // ── SECONDARY: Hook OpenSSL AES_set_decrypt/encrypt_key ──────────────
        // These catch TLS and other OpenSSL-based AES usage.
        // They do NOT catch PAK decryption (PAK uses T-table AES above).
        for (const auto &fb : game_profile::profile().fallback_offsets)
        {
            if (fb.offset == 0)
                continue;
            void *addr = reinterpret_cast<void *>(base + fb.offset);

            if (fb.symbol_name == "AES_set_decrypt_key")
            {
                int status = DobbyHook(
                    addr,
                    reinterpret_cast<dobby_dummy_func_t>(hooked_aes_set_decrypt_key),
                    reinterpret_cast<dobby_dummy_func_t *>(&s_original_aes_set_decrypt_key));
                if (status == 0)
                {
                    logger::log_info("AES", "AES_set_decrypt_key hooked @ 0x%lX [secondary]",
                                     (unsigned long)reinterpret_cast<uintptr_t>(addr));
                    hooks_installed++;
                }
                else
                {
                    logger::log_error("AES", "Dobby failed to hook AES_set_decrypt_key (status=%d)", status);
                }
            }
            else if (fb.symbol_name == "AES_set_encrypt_key")
            {
                int status = DobbyHook(
                    addr,
                    reinterpret_cast<dobby_dummy_func_t>(hooked_aes_set_encrypt_key),
                    reinterpret_cast<dobby_dummy_func_t *>(&s_original_aes_set_encrypt_key));
                if (status == 0)
                {
                    logger::log_info("AES", "AES_set_encrypt_key hooked @ 0x%lX [secondary]",
                                     (unsigned long)reinterpret_cast<uintptr_t>(addr));
                    hooks_installed++;
                }
                else
                {
                    logger::log_error("AES", "Dobby failed to hook AES_set_encrypt_key (status=%d)", status);
                }
            }
        }

        // ── TERTIARY: try legacy FAES::DecryptData via dlsym ─────────────────
        void *decrypt_addr = find_decrypt_data();
        if (decrypt_addr)
        {
            int status = DobbyHook(
                decrypt_addr,
                reinterpret_cast<dobby_dummy_func_t>(hooked_decrypt_data),
                reinterpret_cast<dobby_dummy_func_t *>(&s_original_decrypt));
            if (status == 0)
            {
                logger::log_info("AES", "FAES::DecryptData hooked @ 0x%lX [tertiary/dlsym]",
                                 (unsigned long)reinterpret_cast<uintptr_t>(decrypt_addr));
                hooks_installed++;
            }
            else
            {
                logger::log_warn("AES", "Dobby failed to hook FAES::DecryptData (status=%d)", status);
            }
        }

        s_initialized.store(true);

        // ── DIRECT PROBE: Call GetPakEncryptionKey to extract key post-init ──
        // Hooks fire too late (PAK decrypt happens during dlopen before we hook).
        // So we call the key resolver directly to grab the cached key.
        probe_encryption_key_direct();

        logger::log_info("AES", "AES extractor initialized: %d hook(s) active", hooks_installed);
        if (hooks_installed == 0)
        {
            logger::log_warn("AES", "No AES hooks installed — key capture inactive until memory scan");
        }
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
        // NOTE: Only PAK-specific terms — "AES" alone is too generic and matches noise
        const char *search_terms[] = {
            "PakEncryptionKey",
            "CryptoKeys",
            "EncryptionKeyGuid",
        };

        for (const char *term : search_terms)
        {
            auto refs = pattern::find_string_all(term);
            for (auto ref_ptr : refs)
            {
                uintptr_t ref_addr = reinterpret_cast<uintptr_t>(ref_ptr);
                // Check ±128 bytes around the string reference for 32-byte aligned blobs
                // that look like AES keys (high entropy, non-zero)
                for (int delta = -128; delta <= 128; delta += 8)
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

                    // Skip if too low entropy (less than 20 unique bytes) or all zeros
                    // Skip if all printable ASCII (likely a string, not a key)
                    // A real AES-256 key should have ~28+ unique byte values
                    if (unique_bytes < 20 || is_key_all_zeros(candidate) || all_printable)
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

    // ═══ Get the single PAK encryption key ════════════════════════════════
    // Strategy:
    //   1. FAES::DecryptData hook = definitive (only used for PAK decryption)
    //   2. Otherwise, pick the AES-256 key with the HIGHEST hit count.
    //      PAK key is reused for every .pak file → many hits.
    //      TLS session keys are ephemeral → 1-2 hits max.
    AESKey get_pak_key()
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        // Priority 1: GetPakEncryptionKey or FAES_DecryptData = definitive PAK key
        for (const auto &k : s_keys)
        {
            if (k.source == "hook:GetPakEncryptionKey" ||
                k.source == "direct:GetPakEncryptionKey" ||
                k.source == "hook:FAES_DecryptData")
                return k;
        }

        // Priority 1b: Legacy FAES::DecryptData (dlsym path)
        for (const auto &k : s_keys)
        {
            if (k.source == "hook:FAES::DecryptData")
                return k;
        }

        // Priority 2: highest hit-count key (PAK key seen many times > TLS)
        AESKey *best = nullptr;
        int best_hits = 0;
        for (auto &k : s_keys)
        {
            int hits = get_hit_count(k.bytes);
            if (hits > best_hits)
            {
                best_hits = hits;
                best = &k;
            }
        }

        if (best && best_hits >= 2)
        {
            logger::log_info("AES", "get_pak_key: selected key with %d hits (source: %s)",
                             best_hits, best->source.c_str());
            return *best;
        }

        // Priority 3: earliest captured key (PAK decryption happens at startup before TLS)
        if (!s_keys.empty())
            return s_keys.front();

        AESKey empty;
        memset(empty.bytes, 0, 32);
        empty.source = "none";
        empty.timestamp = 0;
        return empty;
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

    void set_capture_enabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_capture_enabled = enabled;
        if (enabled)
            logger::log_info("AES", "AES key capture ENABLED");
        else
            logger::log_info("AES", "AES key capture DISABLED (no log spam)");
    }

} // namespace aes_extractor
