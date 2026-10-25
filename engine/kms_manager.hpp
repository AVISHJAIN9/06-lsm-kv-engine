#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "status.hpp"
#include "crypto_stream.hpp"

namespace lsm {

// KMSClient: Interface for pluggable Key Management Systems (AWS KMS, GCP Cloud KMS, HashiCorp Vault)
class KMSClient {
public:
    virtual ~KMSClient() = default;

    virtual Status GenerateMasterKey(const std::string& kek_id) = 0;
    virtual Status EncryptDEK(
        const std::string& raw_dek,
        const std::string& kek_id,
        std::string* out_encrypted_dek
    ) = 0;
    virtual Status DecryptDEK(
        const std::string& encrypted_dek,
        const std::string& kek_id,
        std::string* out_raw_dek
    ) = 0;
};

// LocalKMSClient: High-speed local hardware-wrapped Key Encryption Key (KEK) provider
class LocalKMSClient : public KMSClient {
public:
    LocalKMSClient();
    ~LocalKMSClient() override = default;

    Status GenerateMasterKey(const std::string& kek_id) override;
    Status EncryptDEK(
        const std::string& raw_dek,
        const std::string& kek_id,
        std::string* out_encrypted_dek
    ) override;
    Status DecryptDEK(
        const std::string& encrypted_dek,
        const std::string& kek_id,
        std::string* out_raw_dek
    ) override;

    bool HasKEK(const std::string& kek_id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> master_keks_;
};

// KMSManager: Envelope encryption manager supporting online master key rotation (Feature 140)
class KMSManager {
public:
    explicit KMSManager(std::shared_ptr<KMSClient> kms_client);
    ~KMSManager() = default;

    // Generates a new Data Encryption Key (DEK), returning both the raw DEK and its KEK-wrapped envelope
    Status GenerateNewDEK(
        const std::string& kek_id,
        std::string* out_raw_dek,
        std::string* out_encrypted_dek
    );

    // Unwraps an encrypted DEK using the specified KEK
    Status DecryptDEK(
        const std::string& encrypted_dek,
        const std::string& kek_id,
        std::string* out_raw_dek
    );

    // Online Envelope Re-Keying: Rotates master KEK without re-encrypting multi-gigabyte SSTable data blocks
    Status RotateMasterKey(
        const std::string& old_kek_id,
        const std::string& new_kek_id,
        const std::string& encrypted_dek,
        std::string* out_reencrypted_dek
    );

    std::shared_ptr<KMSClient> GetClient() const { return kms_client_; }

private:
    std::shared_ptr<KMSClient> kms_client_;
};

} // namespace lsm
