#include "flush_scheduler.hpp"
#include "memtable_iter.hpp"
#include "wal_format.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <sstream>

namespace fs = std::filesystem;

namespace lsm {

FlushScheduler::FlushScheduler(std::string db_directory)
    : db_dir_(std::move(db_directory)) {
    fs::create_directories(db_dir_);
    worker_ = std::thread(&FlushScheduler::WorkerLoop, this);
}

FlushScheduler::~FlushScheduler() {
    Shutdown();
}

void FlushScheduler::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_requested_) return;
        stop_requested_ = true;
        cv_work_.notify_all();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

Status FlushScheduler::ScheduleFlush(
    std::shared_ptr<MemTableCore> mem,
    uint64_t log_number,
    std::function<void(Status, uint64_t, const std::string&)> on_complete
) {
    if (!mem) return Status::InvalidArgument("Null MemTable in ScheduleFlush");

    std::lock_guard<std::mutex> lock(mu_);
    if (stop_requested_) {
        return Status::Busy("FlushScheduler is shutting down");
    }

    FlushTask task;
    task.mem = std::move(mem);
    task.log_number = log_number;
    task.on_complete = std::move(on_complete);

    queue_.push_back(std::move(task));
    cv_work_.notify_one();
    return Status::OK();
}

void FlushScheduler::WaitForFlush() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_flush_done_.wait(lock, [this] {
        return queue_.empty() && !is_flushing_.load(std::memory_order_acquire);
    });
}

FlushStats FlushScheduler::GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

size_t FlushScheduler::PendingFlushCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size() + (is_flushing_.load(std::memory_order_relaxed) ? 1 : 0);
}

void FlushScheduler::WorkerLoop() {
    while (true) {
        FlushTask task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_work_.wait(lock, [this] {
                return stop_requested_ || !queue_.empty();
            });

            if (stop_requested_ && queue_.empty()) {
                break;
            }

            if (queue_.empty()) {
                continue;
            }

            task = std::move(queue_.front());
            queue_.pop_front();
            is_flushing_.store(true, std::memory_order_release);
        }

        auto start_t = std::chrono::high_resolution_clock::now();

        std::string sst_path;
        uint64_t keys_flushed = 0;
        uint64_t bytes_flushed = 0;

        Status s = FlushMemTableToSST(task, &sst_path, &keys_flushed, &bytes_flushed);

        auto end_t = std::chrono::high_resolution_clock::now();
        uint64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end_t - start_t).count();

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (s.ok()) {
                stats_.flushes_completed++;
                stats_.total_keys_flushed += keys_flushed;
                stats_.total_bytes_flushed += bytes_flushed;
                stats_.total_flush_duration_micros += dur_us;
            } else {
                stats_.flushes_failed++;
            }
            is_flushing_.store(false, std::memory_order_release);
            cv_flush_done_.notify_all();
        }

        if (task.on_complete) {
            task.on_complete(s, task.log_number, sst_path);
        }
    }
}

Status FlushScheduler::FlushMemTableToSST(
    const FlushTask& task,
    std::string* out_sst_path,
    uint64_t* out_keys,
    uint64_t* out_bytes
) {
    if (!task.mem) return Status::InvalidArgument("Null memtable in task");

    uint64_t file_num = next_file_number_.fetch_add(1, std::memory_order_relaxed);
    std::string sst_filename = "L0_" + std::to_string(file_num) + ".sst";
    std::string sst_filepath = (fs::path(db_dir_) / sst_filename).string();

    std::ofstream ofs(sst_filepath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::IOError("Failed to open SSTable destination file: " + sst_filepath);
    }

    auto iter = task.mem->NewIterator();
    iter->SeekToFirst();

    uint64_t key_count = 0;
    uint64_t byte_count = 0;
    std::string data_block;

    while (iter->Valid()) {
        Slice k = iter->key();
        Slice v = iter->value();

        // Pack: [k.size() (4B)] [k.data()] [v.size() (4B)] [v.data()]
        uint32_t klen = static_cast<uint32_t>(k.size());
        uint32_t vlen = static_cast<uint32_t>(v.size());

        char hdr[8];
        hdr[0] = static_cast<char>(klen & 0xff);
        hdr[1] = static_cast<char>((klen >> 8) & 0xff);
        hdr[2] = static_cast<char>((klen >> 16) & 0xff);
        hdr[3] = static_cast<char>((klen >> 24) & 0xff);

        hdr[4] = static_cast<char>(vlen & 0xff);
        hdr[5] = static_cast<char>((vlen >> 8) & 0xff);
        hdr[6] = static_cast<char>((vlen >> 16) & 0xff);
        hdr[7] = static_cast<char>((vlen >> 24) & 0xff);

        data_block.append(hdr, 8);
        data_block.append(k.data(), k.size());
        data_block.append(v.data(), v.size());

        key_count++;
        iter->Next();
    }

    // Compute checksum
    uint32_t crc = (data_block.size() > 0) ? wal::CRC32C::Compute(data_block.data(), data_block.size()) : 0;

    // Write payload block
    if (!data_block.empty()) {
        ofs.write(data_block.data(), data_block.size());
    }

    // Write footer: [CRC (4B)] [KeyCount (8B)] [Magic (4B)]
    char footer[16];
    footer[0] = static_cast<char>(crc & 0xff);
    footer[1] = static_cast<char>((crc >> 8) & 0xff);
    footer[2] = static_cast<char>((crc >> 16) & 0xff);
    footer[3] = static_cast<char>((crc >> 24) & 0xff);

    for (int i = 0; i < 8; ++i) {
        footer[4 + i] = static_cast<char>((key_count >> (i * 8)) & 0xff);
    }
    footer[12] = 'S'; footer[13] = 'S'; footer[14] = 'T'; footer[15] = '0';

    ofs.write(footer, 16);
    ofs.flush();
    ofs.close();

    byte_count = fs::file_size(sst_filepath);

    if (out_sst_path) *out_sst_path = sst_filepath;
    if (out_keys) *out_keys = key_count;
    if (out_bytes) *out_bytes = byte_count;

    return Status::OK();
}

} // namespace lsm
