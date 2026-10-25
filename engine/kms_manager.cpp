#include "kms_manager.hpp"

namespace lsm {

LocalKMSClient::LocalKMSClient() {
    GenerateMasterKey("default_kek");
}

Status LocalKMSClient::GenerateMasterKey(const std::string& kek_id) {
    std::string key;
    Status s = BlockCipher::GenerateRandomKey(&key);
    if (!s.ok()) return s;

    std::lock_guard<std::mutex> lock(mutex_);
    master_keks_[kek_id] = key;
    return Status::OK();
}

bool LocalKMSClient::HasKEK(const std::string& kek_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return master_keks_.find(kek_id) != master_keks_.end();
}

Status LocalKMSClient::EncryptDEK(
    const std::string& raw_dek,
    const std::string& kek_id,
    std::string* out_encrypted_dek
) {
    std::string kek;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = master_keks_.find(kek_id);
        if (it == master_keks_.end()) {
            return Status::NotFound("Master KEK ID not found: " + kek_id);
        }
        kek = it->second;
    }

    std::string iv;
    Status s = BlockCipher::GenerateIV(0xDEADBEEF, 0x1, &iv);
    if (!s.ok()) return s;

    std::string cipher;
    char tag[BlockCipher::kTagSize];
    s = BlockCipher::EncryptBlock(Slice(raw_dek), Slice(kek), Slice(iv), &cipher, tag);
    if (!s.ok()) return s;

    // Packed format: [IV 12B] [TAG 16B] [CIPHERTEXT]
    if (out_encrypted_dek) {
        *out_encrypted_dek = iv + std::string(tag, BlockCipher::kTagSize) + cipher;
    }
    return Status::OK();
}

Status LocalKMSClient::DecryptDEK(
    const std::string& encrypted_dek,
    const std::string& kek_id,
    std::string* out_raw_dek
) {
    if (encrypted_dek.size() < BlockCipher::kIVSize + BlockCipher::kTagSize) {
        return Status::Corruption("Encrypted DEK envelope is truncated");
    }

    std::string kek;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = master_keks_.find(kek_id);
        if (it == master_keks_.end()) {
            return Status::NotFound("Master KEK ID not found: " + kek_id);
        }
        kek = it->second;
    }

    std::string iv = encrypted_dek.substr(0, BlockCipher::kIVSize);
    std::string tag_str = encrypted_dek.substr(BlockCipher::kIVSize, BlockCipher::kTagSize);
    std::string cipher = encrypted_dek.substr(BlockCipher::kIVSize + BlockCipher::kTagSize);

    return BlockCipher::DecryptBlock(
        Slice(cipher),
        Slice(kek),
        Slice(iv),
        tag_str.data(),
        out_raw_dek
    );
}

KMSManager::KMSManager(std::shared_ptr<KMSClient> kms_client)
    : kms_client_(std::move(kms_client)) {
    if (!kms_client_) {
        kms_client_ = std::make_shared<LocalKMSClient>();
    }
}

Status KMSManager::GenerateNewDEK(
    const std::string& kek_id,
    std::string* out_raw_dek,
    std::string* out_encrypted_dek
) {
    std::string raw_dek;
    Status s = BlockCipher::GenerateRandomKey(&raw_dek);
    if (!s.ok()) return s;

    s = kms_client_->EncryptDEK(raw_dek, kek_id, out_encrypted_dek);
    if (!s.ok()) return s;

    if (out_raw_dek) *out_raw_dek = raw_dek;
    return Status::OK();
}

Status KMSManager::DecryptDEK(
    const std::string& encrypted_dek,
    const std::string& kek_id,
    std::string* out_raw_dek
) {
    return kms_client_->DecryptDEK(encrypted_dek, kek_id, out_raw_dek);
}

Status KMSManager::RotateMasterKey(
    const std::string& old_kek_id,
    const std::string& new_kek_id,
    const std::string& encrypted_dek,
    std::string* out_reencrypted_dek
) {
    // 1. Unwrap DEK using old KEK
    std::string raw_dek;
    Status s = kms_client_->DecryptDEK(encrypted_dek, old_kek_id, &raw_dek);
    if (!s.ok()) return s;

    // 2. Re-wrap DEK using new KEK (SSTable file data on disk is untouched!)
    return kms_client_->EncryptDEK(raw_dek, new_kek_id, out_reencrypted_dek);
}

} // namespace lsm
