#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <cstddef>
#include "slice.hpp"
#include "status.hpp"

namespace lsm {
namespace crypto {

static constexpr size_t kKeySize = 32;       // 256-bit key
static constexpr size_t kNonceSize = 12;     // 96-bit nonce / IV
static constexpr size_t kTagSize = 16;       // 128-bit authentication tag
static constexpr size_t kCryptoHeaderSize = kNonceSize + kTagSize; // 28 bytes envelope

class ChaCha20Poly1305 {
public:
    static Status Encrypt(
        const uint8_t key[kKeySize],
        const uint8_t nonce[kNonceSize],
        const Slice& plaintext,
        const Slice& aad,
        std::string* ciphertext,
        uint8_t tag_out[kTagSize]
    );

    static Status Decrypt(
        const uint8_t key[kKeySize],
        const uint8_t nonce[kNonceSize],
        const Slice& ciphertext,
        const Slice& aad,
        const uint8_t tag_in[kTagSize],
        std::string* plaintext
    );

    static void GenerateRandomNonce(uint8_t nonce_out[kNonceSize]);
    static void GenerateKeyFromSecret(const std::string& secret, uint8_t key_out[kKeySize]);

    // Envelope helpers for WAL records: [12B Nonce] [16B Tag] [Ciphertext]
    static Status EncryptPayload(
        const uint8_t key[kKeySize],
        const Slice& plaintext,
        std::string* encrypted_envelope
    );

    static Status DecryptPayload(
        const uint8_t key[kKeySize],
        const Slice& encrypted_envelope,
        std::string* decrypted_plaintext
    );
};

// Cryptographic Key Storage and Lifecycle
class KeyStore {
public:
    KeyStore() = default;
    explicit KeyStore(const std::string& raw_key_secret);
    explicit KeyStore(const uint8_t key[kKeySize]);

    void SetKey(const uint8_t key[kKeySize]);
    void SetKeyFromSecret(const std::string& secret);
    const uint8_t* GetKey() const { return key_.data(); }
    bool HasKey() const { return initialized_; }

private:
    std::array<uint8_t, kKeySize> key_{};
    bool initialized_{false};
};

// WALCipher provides block-level and payload-level authenticated encryption (AEAD)
class WALCipher {
public:
    explicit WALCipher(const KeyStore& key_store, uint64_t segment_id = 0);
    ~WALCipher() = default;

    // Encrypts block / payload with deterministic or randomized IV
    Status EncryptBlock(
        const uint8_t* in,
        size_t len,
        uint64_t block_index,
        std::string* out_ciphertext_envelope
    );

    // Decrypts and authenticates block / payload
    Status DecryptBlock(
        const uint8_t* in_envelope,
        size_t envelope_len,
        uint64_t block_index,
        std::string* out_plaintext
    );

    // Derives a 96-bit nonce deterministically from segment_id and block_index
    static void DeriveNonce(uint64_t segment_id, uint64_t block_index, uint8_t nonce_out[kNonceSize]);

private:
    KeyStore key_store_;
    uint64_t segment_id_{0};
};

} // namespace crypto
} // namespace lsm
