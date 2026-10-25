#include "crypto_stream.hpp"

namespace lsm {

Status BlockCipher::GenerateRandomKey(std::string* out_key) {
    if (!out_key) {
        return Status::InvalidArgument("Null output pointer in GenerateRandomKey");
    }
    out_key->resize(kKeySize);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out_key->data()), static_cast<int>(kKeySize)) != 1) {
        return Status::IOError("Failed to generate secure random key via OpenSSL RAND_bytes");
    }
    return Status::OK();
}

Status BlockCipher::GenerateIV(uint64_t file_number, uint32_t block_offset, std::string* out_iv) {
    if (!out_iv) {
        return Status::InvalidArgument("Null output pointer in GenerateIV");
    }
    out_iv->resize(kIVSize);
    char* dst = out_iv->data();

    // 8 bytes file_number (little-endian)
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<char>((file_number >> (i * 8)) & 0xFF);
    }
    // 4 bytes block_offset (little-endian)
    for (int i = 0; i < 4; ++i) {
        dst[8 + i] = static_cast<char>((block_offset >> (i * 8)) & 0xFF);
    }
    return Status::OK();
}

Status BlockCipher::EncryptBlock(
    const Slice& plaintext,
    const Slice& key,
    const Slice& iv,
    std::string* out_ciphertext,
    char out_tag[kTagSize]
) {
    if (key.size() != kKeySize) {
        return Status::InvalidArgument("Invalid AES-256 key size: expected 32 bytes, got " + std::to_string(key.size()));
    }
    if (iv.size() != kIVSize) {
        return Status::InvalidArgument("Invalid GCM IV size: expected 12 bytes, got " + std::to_string(iv.size()));
    }
    if (!out_ciphertext || !out_tag) {
        return Status::InvalidArgument("Null output buffer in EncryptBlock");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Status::IOError("Failed to allocate EVP_CIPHER_CTX");
    }

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("Failed to initialize AES-256-GCM cipher");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIVSize), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("Failed to set GCM IV length");
    }

    if (EVP_EncryptInit_ex(
            ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.data()),
            reinterpret_cast<const unsigned char*>(iv.data())
        ) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("Failed to set key and IV for encryption");
    }

    out_ciphertext->resize(plaintext.size());
    int out_len = 0;

    if (plaintext.size() > 0) {
        if (EVP_EncryptUpdate(
                ctx,
                reinterpret_cast<unsigned char*>(out_ciphertext->data()),
                &out_len,
                reinterpret_cast<const unsigned char*>(plaintext.data()),
                static_cast<int>(plaintext.size())
            ) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Status::IOError("EVP_EncryptUpdate failed");
        }
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(
            ctx,
            reinterpret_cast<unsigned char*>(out_ciphertext->data()) + out_len,
            &final_len
        ) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("EVP_EncryptFinal_ex failed");
    }
    out_ciphertext->resize(out_len + final_len);

    // Get the 16-byte GCM authentication tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize), out_tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("Failed to retrieve GCM authentication tag");
    }

    EVP_CIPHER_CTX_free(ctx);
    return Status::OK();
}

Status BlockCipher::DecryptBlock(
    const Slice& ciphertext,
    const Slice& key,
    const Slice& iv,
    const char tag[kTagSize],
    std::string* out_plaintext
) {
    if (key.size() != kKeySize) {
        return Status::InvalidArgument("Invalid AES-256 key size: expected 32 bytes, got " + std::to_string(key.size()));
    }
    if (iv.size() != kIVSize) {
        return Status::InvalidArgument("Invalid GCM IV size: expected 12 bytes, got " + std::to_string(iv.size()));
    }
    if (!out_plaintext || !tag) {
        return Status::InvalidArgument("Null output buffer in DecryptBlock");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Status::IOError("Failed to allocate EVP_CIPHER_CTX");
    }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("Failed to initialize AES-256-GCM cipher for decryption");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIVSize), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("Failed to set GCM IV length for decryption");
    }

    if (EVP_DecryptInit_ex(
            ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.data()),
            reinterpret_cast<const unsigned char*>(iv.data())
        ) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::IOError("Failed to set key and IV for decryption");
    }

    out_plaintext->resize(ciphertext.size());
    int out_len = 0;

    if (ciphertext.size() > 0) {
        if (EVP_DecryptUpdate(
                ctx,
                reinterpret_cast<unsigned char*>(out_plaintext->data()),
                &out_len,
                reinterpret_cast<const unsigned char*>(ciphertext.data()),
                static_cast<int>(ciphertext.size())
            ) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Status::Corruption("EVP_DecryptUpdate failed");
        }
    }

    // Set expected GCM tag for verification
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize), const_cast<char*>(tag)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Status::Corruption("Failed to set expected GCM tag");
    }

    int final_len = 0;
    int ret = EVP_DecryptFinal_ex(
        ctx,
        reinterpret_cast<unsigned char*>(out_plaintext->data()) + out_len,
        &final_len
    );
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) {
        out_plaintext->clear();
        return Status::Corruption("Authentication tag mismatch: ciphertext altered or corrupted");
    }

    out_plaintext->resize(out_len + final_len);
    return Status::OK();
}

} // namespace lsm
