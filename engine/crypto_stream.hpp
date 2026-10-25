#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "slice.hpp"
#include "status.hpp"

namespace lsm {

// BlockCipher: Hardware-accelerated AES-256-GCM encryption and decryption with 128-bit authentication tags (Feature 139)
class BlockCipher {
public:
    static constexpr size_t kKeySize = 32;  // 256-bit AES key
    static constexpr size_t kIVSize = 12;   // 96-bit GCM initialization vector
    static constexpr size_t kTagSize = 16;  // 128-bit authentication tag

    // Generates a cryptographically secure random 256-bit encryption key
    static Status GenerateRandomKey(std::string* out_key);

    // Generates a deterministic unique 96-bit IV from file number and block offset
    static Status GenerateIV(uint64_t file_number, uint32_t block_offset, std::string* out_iv);

    // Encrypts plaintext using AES-256-GCM, producing ciphertext and 16-byte authentication tag
    static Status EncryptBlock(
        const Slice& plaintext,
        const Slice& key,
        const Slice& iv,
        std::string* out_ciphertext,
        char out_tag[kTagSize]
    );

    // Decrypts ciphertext and validates the 16-byte authentication tag
    static Status DecryptBlock(
        const Slice& ciphertext,
        const Slice& key,
        const Slice& iv,
        const char tag[kTagSize],
        std::string* out_plaintext
    );
};

} // namespace lsm
