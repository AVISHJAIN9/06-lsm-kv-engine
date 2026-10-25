#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include "status.hpp"

namespace lsm {

// ObjectStoreClient: Abstract cloud object storage client interface (S3/GCS/Azure Blob)
class ObjectStoreClient {
public:
    virtual ~ObjectStoreClient() = default;

    virtual Status PutObject(const std::string& key, const std::string& data) = 0;
    virtual Status InitiateMultiPart(const std::string& key, std::string* out_upload_id) = 0;
    virtual Status UploadPart(
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::string& part_data,
        std::string* out_etag
    ) = 0;
    virtual Status CompleteMultiPart(
        const std::string& key,
        const std::string& upload_id,
        const std::vector<std::pair<int, std::string>>& parts
    ) = 0;
    virtual Status AbortMultiPart(const std::string& key, const std::string& upload_id) = 0;
    virtual Status GetObject(const std::string& key, std::string* out_data) = 0;
};

// MockCloudStorageClient: High-fidelity simulated cloud object store with fault injection and retry testing
class MockCloudStorageClient : public ObjectStoreClient {
public:
    MockCloudStorageClient() = default;
    ~MockCloudStorageClient() override = default;

    Status PutObject(const std::string& key, const std::string& data) override;
    Status InitiateMultiPart(const std::string& key, std::string* out_upload_id) override;
    Status UploadPart(
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::string& part_data,
        std::string* out_etag
    ) override;
    Status CompleteMultiPart(
        const std::string& key,
        const std::string& upload_id,
        const std::vector<std::pair<int, std::string>>& parts
    ) override;
    Status AbortMultiPart(const std::string& key, const std::string& upload_id) override;
    Status GetObject(const std::string& key, std::string* out_data) override;

    void SetSimulatedFailures(int count) {
        simulated_failures_remaining_.store(count, std::memory_order_relaxed);
    }

    size_t GetObjectCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return objects_.size();
    }

    bool HasObject(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return objects_.find(key) != objects_.end();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> objects_;
    std::unordered_map<std::string, std::map<int, std::string>> in_flight_parts_;
    std::atomic<int> simulated_failures_remaining_{0};
    std::atomic<uint64_t> next_upload_id_{1};
};

// ObjectStoreOffloader: Multi-part cloud object storage streaming uploader with retry & chunking (Feature 138)
class ObjectStoreOffloader {
public:
    explicit ObjectStoreOffloader(
        std::shared_ptr<ObjectStoreClient> client,
        int max_retries = 3
    );
    ~ObjectStoreOffloader() = default;

    // Uploads a local file using parallel / multi-part chunking with automatic retries
    Status UploadFile(
        const std::string& local_file_path,
        const std::string& remote_key,
        size_t chunk_size = 1024 * 1024
    );

private:
    std::shared_ptr<ObjectStoreClient> client_;
    int max_retries_{3};
};

} // namespace lsm
