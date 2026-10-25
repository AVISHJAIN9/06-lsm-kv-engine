#include "object_offloader.hpp"
#include <fstream>
#include <map>
#include <thread>
#include <chrono>

namespace lsm {

Status MockCloudStorageClient::PutObject(const std::string& key, const std::string& data) {
    if (simulated_failures_remaining_.load(std::memory_order_relaxed) > 0) {
        simulated_failures_remaining_.fetch_sub(1, std::memory_order_relaxed);
        return Status::IOError("HTTP 503: Simulated temporary Cloud Storage service unavailable");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    objects_[key] = data;
    return Status::OK();
}

Status MockCloudStorageClient::InitiateMultiPart(const std::string& key, std::string* out_upload_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string upload_id = "upload_" + std::to_string(next_upload_id_++) + "_" + key;
    in_flight_parts_[upload_id] = std::map<int, std::string>();
    if (out_upload_id) *out_upload_id = upload_id;
    return Status::OK();
}

Status MockCloudStorageClient::UploadPart(
    const std::string& key,
    const std::string& upload_id,
    int part_number,
    const std::string& part_data,
    std::string* out_etag
) {
    (void)key;
    if (simulated_failures_remaining_.load(std::memory_order_relaxed) > 0) {
        simulated_failures_remaining_.fetch_sub(1, std::memory_order_relaxed);
        return Status::IOError("HTTP 500: Simulated chunk drop during multi-part upload");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = in_flight_parts_.find(upload_id);
    if (it == in_flight_parts_.end()) {
        return Status::NotFound("Upload ID not found: " + upload_id);
    }

    it->second[part_number] = part_data;
    if (out_etag) {
        *out_etag = "\"etag_part_" + std::to_string(part_number) + "\"";
    }
    return Status::OK();
}

Status MockCloudStorageClient::CompleteMultiPart(
    const std::string& key,
    const std::string& upload_id,
    const std::vector<std::pair<int, std::string>>& parts
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = in_flight_parts_.find(upload_id);
    if (it == in_flight_parts_.end()) {
        return Status::NotFound("Upload ID not found for completion: " + upload_id);
    }

    std::string assembled_data;
    for (const auto& p : parts) {
        int part_num = p.first;
        auto part_it = it->second.find(part_num);
        if (part_it != it->second.end()) {
            assembled_data += part_it->second;
        }
    }

    objects_[key] = assembled_data;
    in_flight_parts_.erase(it);
    return Status::OK();
}

Status MockCloudStorageClient::AbortMultiPart(const std::string& key, const std::string& upload_id) {
    (void)key;
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_parts_.erase(upload_id);
    return Status::OK();
}

Status MockCloudStorageClient::GetObject(const std::string& key, std::string* out_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = objects_.find(key);
    if (it == objects_.end()) {
        return Status::NotFound("Object key not found in storage: " + key);
    }
    if (out_data) *out_data = it->second;
    return Status::OK();
}

ObjectStoreOffloader::ObjectStoreOffloader(
    std::shared_ptr<ObjectStoreClient> client,
    int max_retries
) : client_(std::move(client)), max_retries_(max_retries) {}

Status ObjectStoreOffloader::UploadFile(
    const std::string& local_file_path,
    const std::string& remote_key,
    size_t chunk_size
) {
    if (!client_) {
        return Status::InvalidArgument("Null ObjectStoreClient in Offloader");
    }

    std::ifstream in(local_file_path, std::ios::binary);
    if (!in.is_open()) {
        return Status::IOError("Could not open local file for offload: " + local_file_path);
    }

    in.seekg(0, std::ios::end);
    size_t file_size = in.tellg();
    in.seekg(0, std::ios::beg);

    if (file_size <= chunk_size) {
        // Direct single-part upload with retries
        std::string data(file_size, '\0');
        in.read(data.data(), file_size);
        in.close();

        Status s;
        for (int retry = 0; retry <= max_retries_; ++retry) {
            s = client_->PutObject(remote_key, data);
            if (s.ok()) return Status::OK();
            std::this_thread::sleep_for(std::chrono::milliseconds(2 * (1 << retry)));
        }
        return s;
    }

    // Multi-part chunked upload
    std::string upload_id;
    Status s = client_->InitiateMultiPart(remote_key, &upload_id);
    if (!s.ok()) return s;

    std::vector<std::pair<int, std::string>> completed_parts;
    int part_num = 1;

    std::string buffer(chunk_size, '\0');
    while (in.read(buffer.data(), chunk_size) || in.gcount() > 0) {
        size_t bytes_read = in.gcount();
        std::string chunk = buffer.substr(0, bytes_read);

        std::string etag;
        bool part_uploaded = false;
        Status last_err;

        for (int retry = 0; retry <= max_retries_; ++retry) {
            last_err = client_->UploadPart(remote_key, upload_id, part_num, chunk, &etag);
            if (last_err.ok()) {
                part_uploaded = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2 * (1 << retry)));
        }

        if (!part_uploaded) {
            client_->AbortMultiPart(remote_key, upload_id);
            return Status::IOError("Failed to upload part " + std::to_string(part_num) + ": " + last_err.ToString());
        }

        completed_parts.push_back({part_num, etag});
        part_num++;
    }

    in.close();
    return client_->CompleteMultiPart(remote_key, upload_id, completed_parts);
}

} // namespace lsm
