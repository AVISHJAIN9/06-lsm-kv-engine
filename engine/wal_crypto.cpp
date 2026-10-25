#include "wal_crypto.hpp"
#include <cstring>
#include <random>
#include <chrono>
#include <algorithm>

namespace lsm {
namespace crypto {

namespace {

inline uint32_t Rotl32(uint32_t v, int c) {
    return (v << c) | (v >> (32 - c));
}

inline uint32_t Load32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

inline void Store32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

inline void Store64LE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xff);
    }
}

// ChaCha20 Quarter Round
inline void QuarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = Rotl32(d, 16);
    c += d; b ^= c; b = Rotl32(b, 12);
    a += b; d ^= a; d = Rotl32(d, 8);
    c += d; b ^= c; b = Rotl32(b, 7);
}

void ChaCha20Block(uint32_t out[16], const uint32_t in[16]) {
    for (int i = 0; i < 16; ++i) out[i] = in[i];

    for (int i = 0; i < 10; ++i) {
        // Column rounds
        QuarterRound(out[0], out[4], out[8],  out[12]);
        QuarterRound(out[1], out[5], out[9],  out[13]);
        QuarterRound(out[2], out[6], out[10], out[14]);
        QuarterRound(out[3], out[7], out[11], out[15]);
        // Diagonal rounds
        QuarterRound(out[0], out[5], out[10], out[15]);
        QuarterRound(out[1], out[6], out[11], out[12]);
        QuarterRound(out[2], out[7], out[8],  out[13]);
        QuarterRound(out[3], out[4], out[9],  out[14]);
    }

    for (int i = 0; i < 16; ++i) out[i] += in[i];
}

void ChaCha20InitState(
    uint32_t state[16],
    const uint8_t key[kKeySize],
    uint32_t counter,
    const uint8_t nonce[kNonceSize]
) {
    // "expand 32-byte k"
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;

    for (int i = 0; i < 8; ++i) {
        state[4 + i] = Load32LE(key + (i * 4));
    }

    state[12] = counter;
    state[13] = Load32LE(nonce + 0);
    state[14] = Load32LE(nonce + 4);
    state[15] = Load32LE(nonce + 8);
}

void ChaCha20XOR(
    const uint8_t key[kKeySize],
    uint32_t counter,
    const uint8_t nonce[kNonceSize],
    const uint8_t* in,
    uint8_t* out,
    size_t length
) {
    uint32_t state[16];
    ChaCha20InitState(state, key, counter, nonce);

    uint32_t block[16];
    uint8_t stream[64];

    while (length > 0) {
        ChaCha20Block(block, state);
        state[12]++; // Increment counter

        for (int i = 0; i < 16; ++i) {
            Store32LE(stream + (i * 4), block[i]);
        }

        size_t chunk = std::min(length, size_t(64));
        for (size_t i = 0; i < chunk; ++i) {
            out[i] = in[i] ^ stream[i];
        }

        in += chunk;
        out += chunk;
        length -= chunk;
    }
}

// Poly1305 MAC Implementation (RFC 8439)
class Poly1305 {
public:
    void Init(const uint8_t key[32]) {
        // r &= 0x0ffffffc0fffffff0ffffffc0fffffff
        r0 = Load32LE(key + 0) & 0x3ffffff;
        r1 = (Load32LE(key + 3) >> 2) & 0x3ffff03;
        r2 = (Load32LE(key + 6) >> 4) & 0x3ffc0ff;
        r3 = (Load32LE(key + 9) >> 6) & 0x3f03fff;
        r4 = (Load32LE(key + 12) >> 8) & 0x00fffff;

        s1 = r1 * 5;
        s2 = r2 * 5;
        s3 = r3 * 5;
        s4 = r4 * 5;

        pad0 = Load32LE(key + 16);
        pad1 = Load32LE(key + 20);
        pad2 = Load32LE(key + 24);
        pad3 = Load32LE(key + 28);

        h0 = h1 = h2 = h3 = h4 = 0;
    }

    void Update(const uint8_t* data, size_t length) {
        while (length > 0) {
            size_t chunk = std::min(length, size_t(16));
            uint8_t block[16] = {0};
            std::memcpy(block, data, chunk);

            uint32_t hibit = 0;
            if (chunk < 16) {
                block[chunk] = 0x01;
                hibit = 0;
            } else {
                hibit = 1;
            }

            uint32_t d0 = Load32LE(block + 0);
            uint32_t d1 = Load32LE(block + 4);
            uint32_t d2 = Load32LE(block + 8);
            uint32_t d3 = Load32LE(block + 12);

            h0 += d0 & 0x3ffffff;
            h1 += ((d0 >> 26) | (d1 << 6)) & 0x3ffffff;
            h2 += ((d1 >> 20) | (d2 << 12)) & 0x3ffffff;
            h3 += ((d2 >> 14) | (d3 << 18)) & 0x3ffffff;
            h4 += (d3 >> 8) | (hibit << 24);

            uint64_t f0 = static_cast<uint64_t>(h0) * r0 + static_cast<uint64_t>(h1) * s4 + static_cast<uint64_t>(h2) * s3 + static_cast<uint64_t>(h3) * s2 + static_cast<uint64_t>(h4) * s1;
            uint64_t f1 = static_cast<uint64_t>(h0) * r1 + static_cast<uint64_t>(h1) * r0 + static_cast<uint64_t>(h2) * s4 + static_cast<uint64_t>(h3) * s3 + static_cast<uint64_t>(h4) * s2;
            uint64_t f2 = static_cast<uint64_t>(h0) * r2 + static_cast<uint64_t>(h1) * r1 + static_cast<uint64_t>(h2) * r0 + static_cast<uint64_t>(h3) * s4 + static_cast<uint64_t>(h4) * s3;
            uint64_t f3 = static_cast<uint64_t>(h0) * r3 + static_cast<uint64_t>(h1) * r2 + static_cast<uint64_t>(h2) * r1 + static_cast<uint64_t>(h3) * r0 + static_cast<uint64_t>(h4) * s4;
            uint64_t f4 = static_cast<uint64_t>(h0) * r4 + static_cast<uint64_t>(h1) * r3 + static_cast<uint64_t>(h2) * r2 + static_cast<uint64_t>(h3) * r1 + static_cast<uint64_t>(h4) * r0;

            h0 = static_cast<uint32_t>(f0) & 0x3ffffff; f1 += (f0 >> 26);
            h1 = static_cast<uint32_t>(f1) & 0x3ffffff; f2 += (f1 >> 26);
            h2 = static_cast<uint32_t>(f2) & 0x3ffffff; f3 += (f2 >> 26);
            h3 = static_cast<uint32_t>(f3) & 0x3ffffff; f4 += (f3 >> 26);
            h4 = static_cast<uint32_t>(f4) & 0x3ffffff;
            h0 += static_cast<uint32_t>(f4 >> 26) * 5;

            data += chunk;
            length -= chunk;
        }
    }

    void Final(uint8_t tag_out[16]) {
        // Carry propagation
        h1 += (h0 >> 26); h0 &= 0x3ffffff;
        h2 += (h1 >> 26); h1 &= 0x3ffffff;
        h3 += (h2 >> 26); h2 &= 0x3ffffff;
        h4 += (h3 >> 26); h3 &= 0x3ffffff;
        h0 += (h4 >> 26) * 5; h4 &= 0x3ffffff;
        h1 += (h0 >> 26); h0 &= 0x3ffffff;

        uint32_t g0 = h0 + 5;
        uint32_t g1 = h1 + (g0 >> 26); g0 &= 0x3ffffff;
        uint32_t g2 = h2 + (g1 >> 26); g1 &= 0x3ffffff;
        uint32_t g3 = h3 + (g2 >> 26); g2 &= 0x3ffffff;
        uint32_t g4 = h4 + (g3 >> 26) - (1 << 26); g3 &= 0x3ffffff;

        uint32_t mask = (g4 >> 31) - 1;
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0;
        h1 = (h1 & mask) | g1;
        h2 = (h2 & mask) | g2;
        h3 = (h3 & mask) | g3;
        h4 = (h4 & mask) | g4;

        uint64_t t0 = (static_cast<uint64_t>(h0)) | (static_cast<uint64_t>(h1) << 26);
        uint64_t t1 = (static_cast<uint64_t>(h1) >> 6) | (static_cast<uint64_t>(h2) << 20) | (static_cast<uint64_t>(h3) << 46);

        uint64_t p0 = static_cast<uint64_t>(pad0) | (static_cast<uint64_t>(pad1) << 32);
        uint64_t p1 = static_cast<uint64_t>(pad2) | (static_cast<uint64_t>(pad3) << 32);

        t0 += p0;
        t1 += p1 + (t0 < p0 ? 1 : 0);

        Store64LE(tag_out + 0, t0);
        Store64LE(tag_out + 8, t1);
    }

private:
    uint32_t r0{0}, r1{0}, r2{0}, r3{0}, r4{0};
    uint32_t s1{0}, s2{0}, s3{0}, s4{0};
    uint32_t h0{0}, h1{0}, h2{0}, h3{0}, h4{0};
    uint32_t pad0{0}, pad1{0}, pad2{0}, pad3{0};
};

} // namespace

Status ChaCha20Poly1305::Encrypt(
    const uint8_t key[kKeySize],
    const uint8_t nonce[kNonceSize],
    const Slice& plaintext,
    const Slice& aad,
    std::string* ciphertext,
    uint8_t tag_out[kTagSize]
) {
    if (!ciphertext) return Status::InvalidArgument("Null ciphertext output pointer");

    // 1. Generate Poly1305 one-time key with block counter = 0
    uint8_t poly_key[64] = {0};
    ChaCha20XOR(key, 0, nonce, poly_key, poly_key, 64);

    // 2. Encrypt plaintext starting with block counter = 1
    ciphertext->resize(plaintext.size());
    if (!plaintext.empty()) {
        ChaCha20XOR(
            key, 1, nonce,
            reinterpret_cast<const uint8_t*>(plaintext.data()),
            reinterpret_cast<uint8_t*>(ciphertext->data()),
            plaintext.size()
        );
    }

    // 3. Compute Poly1305 MAC over: AAD + pad + Ciphertext + pad + len(AAD) + len(Ciphertext)
    Poly1305 poly;
    poly.Init(poly_key);

    if (!aad.empty()) {
        poly.Update(reinterpret_cast<const uint8_t*>(aad.data()), aad.size());
        size_t pad_len = (16 - (aad.size() % 16)) % 16;
        if (pad_len > 0) {
            uint8_t zero_pad[16] = {0};
            poly.Update(zero_pad, pad_len);
        }
    }

    if (!ciphertext->empty()) {
        poly.Update(reinterpret_cast<const uint8_t*>(ciphertext->data()), ciphertext->size());
        size_t pad_len = (16 - (ciphertext->size() % 16)) % 16;
        if (pad_len > 0) {
            uint8_t zero_pad[16] = {0};
            poly.Update(zero_pad, pad_len);
        }
    }

    uint8_t lengths[16];
    Store64LE(lengths + 0, static_cast<uint64_t>(aad.size()));
    Store64LE(lengths + 8, static_cast<uint64_t>(ciphertext->size()));
    poly.Update(lengths, 16);

    poly.Final(tag_out);
    return Status::OK();
}

Status ChaCha20Poly1305::Decrypt(
    const uint8_t key[kKeySize],
    const uint8_t nonce[kNonceSize],
    const Slice& ciphertext,
    const Slice& aad,
    const uint8_t tag_in[kTagSize],
    std::string* plaintext
) {
    if (!plaintext) return Status::InvalidArgument("Null plaintext output pointer");

    // 1. Generate Poly1305 one-time key with block counter = 0
    uint8_t poly_key[64] = {0};
    ChaCha20XOR(key, 0, nonce, poly_key, poly_key, 64);

    // 2. Compute expected Poly1305 MAC
    Poly1305 poly;
    poly.Init(poly_key);

    if (!aad.empty()) {
        poly.Update(reinterpret_cast<const uint8_t*>(aad.data()), aad.size());
        size_t pad_len = (16 - (aad.size() % 16)) % 16;
        if (pad_len > 0) {
            uint8_t zero_pad[16] = {0};
            poly.Update(zero_pad, pad_len);
        }
    }

    if (!ciphertext.empty()) {
        poly.Update(reinterpret_cast<const uint8_t*>(ciphertext.data()), ciphertext.size());
        size_t pad_len = (16 - (ciphertext.size() % 16)) % 16;
        if (pad_len > 0) {
            uint8_t zero_pad[16] = {0};
            poly.Update(zero_pad, pad_len);
        }
    }

    uint8_t lengths[16];
    Store64LE(lengths + 0, static_cast<uint64_t>(aad.size()));
    Store64LE(lengths + 8, static_cast<uint64_t>(ciphertext.size()));
    poly.Update(lengths, 16);

    uint8_t expected_tag[16];
    poly.Final(expected_tag);

    // Constant-time tag comparison
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) {
        diff |= (tag_in[i] ^ expected_tag[i]);
    }
    if (diff != 0) {
        return Status::Corruption("AEAD authentication tag verification failed");
    }

    // 3. Decrypt ciphertext
    plaintext->resize(ciphertext.size());
    if (!ciphertext.empty()) {
        ChaCha20XOR(
            key, 1, nonce,
            reinterpret_cast<const uint8_t*>(ciphertext.data()),
            reinterpret_cast<uint8_t*>(plaintext->data()),
            ciphertext.size()
        );
    }

    return Status::OK();
}

void ChaCha20Poly1305::GenerateRandomNonce(uint8_t nonce_out[kNonceSize]) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    for (size_t i = 0; i < kNonceSize; ++i) {
        nonce_out[i] = static_cast<uint8_t>(rng() & 0xff);
    }
}

void ChaCha20Poly1305::GenerateKeyFromSecret(const std::string& secret, uint8_t key_out[kKeySize]) {
    std::memset(key_out, 0x5a, kKeySize);
    for (size_t i = 0; i < secret.size(); ++i) {
        key_out[i % kKeySize] ^= static_cast<uint8_t>(secret[i]);
        key_out[(i + 7) % kKeySize] += static_cast<uint8_t>(secret[i] * 31);
    }
}

Status ChaCha20Poly1305::EncryptPayload(
    const uint8_t key[kKeySize],
    const Slice& plaintext,
    std::string* encrypted_envelope
) {
    if (!encrypted_envelope) return Status::InvalidArgument("Null envelope output");

    uint8_t nonce[kNonceSize];
    GenerateRandomNonce(nonce);

    std::string ciphertext;
    uint8_t tag[kTagSize];

    Status s = Encrypt(key, nonce, plaintext, Slice(), &ciphertext, tag);
    if (!s.ok()) return s;

    // Envelope: [12B Nonce] [16B Tag] [Ciphertext]
    encrypted_envelope->clear();
    encrypted_envelope->reserve(kCryptoHeaderSize + ciphertext.size());
    encrypted_envelope->append(reinterpret_cast<const char*>(nonce), kNonceSize);
    encrypted_envelope->append(reinterpret_cast<const char*>(tag), kTagSize);
    encrypted_envelope->append(ciphertext);

    return Status::OK();
}

Status ChaCha20Poly1305::DecryptPayload(
    const uint8_t key[kKeySize],
    const Slice& encrypted_envelope,
    std::string* decrypted_plaintext
) {
    if (!decrypted_plaintext) return Status::InvalidArgument("Null plaintext output");
    if (encrypted_envelope.size() < kCryptoHeaderSize) {
        return Status::Corruption("Encrypted envelope is smaller than header size (28 bytes)");
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(encrypted_envelope.data());
    const uint8_t* nonce = p;
    const uint8_t* tag = p + kNonceSize;
    Slice ciphertext(encrypted_envelope.data() + kCryptoHeaderSize, encrypted_envelope.size() - kCryptoHeaderSize);

    return Decrypt(key, nonce, ciphertext, Slice(), tag, decrypted_plaintext);
}

KeyStore::KeyStore(const std::string& raw_key_secret) {
    SetKeyFromSecret(raw_key_secret);
}

KeyStore::KeyStore(const uint8_t key[kKeySize]) {
    SetKey(key);
}

void KeyStore::SetKey(const uint8_t key[kKeySize]) {
    std::memcpy(key_.data(), key, kKeySize);
    initialized_ = true;
}

void KeyStore::SetKeyFromSecret(const std::string& secret) {
    ChaCha20Poly1305::GenerateKeyFromSecret(secret, key_.data());
    initialized_ = true;
}

WALCipher::WALCipher(const KeyStore& key_store, uint64_t segment_id)
    : key_store_(key_store), segment_id_(segment_id) {}

void WALCipher::DeriveNonce(uint64_t segment_id, uint64_t block_index, uint8_t nonce_out[kNonceSize]) {
    // 96-bit nonce: 64-bit segment_id ^ block_index, 32-bit counter
    uint64_t combined = segment_id ^ (block_index * 0x9E3779B97F4A7C15ULL);
    for (int i = 0; i < 8; ++i) {
        nonce_out[i] = static_cast<uint8_t>((combined >> (i * 8)) & 0xff);
    }
    uint32_t block_low = static_cast<uint32_t>(block_index & 0xffffffff);
    for (int i = 0; i < 4; ++i) {
        nonce_out[8 + i] = static_cast<uint8_t>((block_low >> (i * 8)) & 0xff);
    }
}

Status WALCipher::EncryptBlock(
    const uint8_t* in,
    size_t len,
    uint64_t block_index,
    std::string* out_ciphertext_envelope
) {
    if (!key_store_.HasKey()) {
        return Status::InvalidArgument("KeyStore is uninitialized in WALCipher");
    }
    if (!out_ciphertext_envelope) {
        return Status::InvalidArgument("Null output envelope buffer");
    }

    uint8_t nonce[kNonceSize];
    DeriveNonce(segment_id_, block_index, nonce);

    std::string ciphertext;
    uint8_t tag[kTagSize];

    Status s = ChaCha20Poly1305::Encrypt(
        key_store_.GetKey(),
        nonce,
        Slice(reinterpret_cast<const char*>(in), len),
        Slice(),
        &ciphertext,
        tag
    );
    if (!s.ok()) return s;

    out_ciphertext_envelope->clear();
    out_ciphertext_envelope->reserve(kCryptoHeaderSize + ciphertext.size());
    out_ciphertext_envelope->append(reinterpret_cast<const char*>(nonce), kNonceSize);
    out_ciphertext_envelope->append(reinterpret_cast<const char*>(tag), kTagSize);
    out_ciphertext_envelope->append(ciphertext);

    return Status::OK();
}

Status WALCipher::DecryptBlock(
    const uint8_t* in_envelope,
    size_t envelope_len,
    uint64_t /*block_index*/,
    std::string* out_plaintext
) {
    if (!key_store_.HasKey()) {
        return Status::InvalidArgument("KeyStore is uninitialized in WALCipher");
    }
    if (!out_plaintext) {
        return Status::InvalidArgument("Null output plaintext buffer");
    }
    if (envelope_len < kCryptoHeaderSize) {
        return Status::Corruption("Encrypted block envelope smaller than header size (28 bytes)");
    }

    const uint8_t* nonce = in_envelope;
    const uint8_t* tag = in_envelope + kNonceSize;
    Slice ciphertext(reinterpret_cast<const char*>(in_envelope + kCryptoHeaderSize), envelope_len - kCryptoHeaderSize);

    return ChaCha20Poly1305::Decrypt(
        key_store_.GetKey(),
        nonce,
        ciphertext,
        Slice(),
        tag,
        out_plaintext
    );
}

} // namespace crypto
} // namespace lsm

