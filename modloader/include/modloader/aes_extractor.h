#pragma once
// modloader/include/modloader/aes_extractor.h
// ═══════════════════════════════════════════════════════════════════════════
// AES-256 Encryption Key Extractor for UE PAK files
//
// Strategy:
// 1. Hook FAES::DecryptData() — intercepts ALL PAK decryption calls.
//    When the engine decrypts PAK indexes, we capture the AES key used.
// 2. Hook FCoreDelegates::GetPakEncryptionKeyDelegate — intercepts key
//    retrieval before decryption starts.
// 3. Memory scan — scan for likely AES key patterns in .data/.rodata.
//
// Extracted keys are stored and exposed to Lua for inspection and logging.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace aes_extractor
{

    // AES-256 key (32 bytes)
    struct AESKey
    {
        uint8_t bytes[32];
        std::string source;   // how the key was found ("hook:DecryptData", "hook:GetKey", "scan")
        std::string pak_name; // which PAK file was being processed (if known)
        uint64_t timestamp;   // when the key was captured
    };

    // Initialize the AES extractor subsystem.
    // Installs hooks on FAES::DecryptData and key delegate functions.
    // Must be called after symbols::init() has resolved core symbols.
    void init();

    // Get all extracted keys
    std::vector<AESKey> get_keys();

    // Get the most recently extracted key (or empty key if none found)
    AESKey get_latest_key();

    // Get the single best PAK encryption key (prioritizes FAES::DecryptData source).
    // Returns empty key if none found.
    AESKey get_pak_key();

    // Check if any keys have been extracted
    bool has_keys();

    // Get key count
    size_t key_count();

    // Format a key as hex string (64 hex chars for 32 bytes)
    std::string key_to_hex(const AESKey &key);

    // Format a key as base64 string
    std::string key_to_base64(const AESKey &key);

    // Manually add a known key (e.g., from config file or user input)
    void add_key(const AESKey &key);

    // Enable/disable AES key capture (disabled by default to avoid log spam).
    // When disabled, keys are not logged but PAK decryption still works.
    void set_capture_enabled(bool enabled);
    void add_key(const uint8_t *key_bytes, const std::string &source);

    // Try to scan memory for potential AES keys.
    // This is a heuristic scan — looks for 32-byte aligned blobs near
    // string references to "PakEncryptionKey" or "EncryptionKey".
    // Returns number of candidate keys found.
    // NOTE: For PFXVR, these strings don't exist — scan returns 0.
    // The primary extraction method is hooking AES_set_decrypt_key.
    int scan_for_keys();

    // Write all extracted keys to a log file
    void dump_keys_to_file(const std::string &path);

} // namespace aes_extractor
