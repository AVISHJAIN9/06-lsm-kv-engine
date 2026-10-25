#include "block_reader.hpp"
#include <cstring>
#include <algorithm>
#include <vector>

namespace lsm {

namespace {

static inline uint32_t DecodeFixed32(const char* ptr) {
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(ptr);
    return (static_cast<uint32_t>(buffer[0])) |
           (static_cast<uint32_t>(buffer[1]) << 8) |
           (static_cast<uint32_t>(buffer[2]) << 16) |
           (static_cast<uint32_t>(buffer[3]) << 24);
}

static const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
    if (p >= limit) return nullptr;
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
        p++;
        if (byte & 128) {
            result |= ((byte & 127) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;
}

} // namespace

class BlockIterator : public Iterator {
public:
    BlockIterator(const char* data, uint32_t restarts_offset, uint32_t num_restarts)
        : data_(data),
          restarts_offset_(restarts_offset),
          num_restarts_(num_restarts),
          current_(restarts_offset),
          restart_index_(0),
          value_(data ? (data + restarts_offset) : nullptr, 0) {}

    bool Valid() const override {
        return current_ < restarts_offset_;
    }

    Status status() const override {
        return status_;
    }

    Slice key() const override {
        if (!Valid()) return Slice();
        return Slice(key_);
    }

    Slice value() const override {
        if (!Valid()) return Slice();
        return value_;
    }

    void Next() override {
        if (!Valid()) return;
        ParseNextKey();
    }

    void Prev() override {
        if (!Valid()) return;

        const uint32_t original = current_;
        while (GetRestartPoint(restart_index_) >= original) {
            if (restart_index_ == 0) {
                // No more entries before this
                current_ = restarts_offset_;
                restart_index_ = num_restarts_;
                return;
            }
            restart_index_--;
        }

        SeekToRestartPoint(restart_index_);
        do {
            if (!ParseNextKey()) {
                break;
            }
        } while (NextEntryOffset() < original);
    }

    void Seek(const Slice& target) override {
        if (num_restarts_ == 0) {
            current_ = restarts_offset_;
            return;
        }

        // Binary search restart array
        uint32_t left = 0;
        uint32_t right = num_restarts_ - 1;

        while (left < right) {
            uint32_t mid = left + (right - left + 1) / 2;
            uint32_t region_offset = GetRestartPoint(mid);

            uint32_t shared = 0, non_shared = 0, value_length = 0;
            const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_offset_,
                                              &shared, &non_shared, &value_length);
            if (key_ptr == nullptr || shared != 0) {
                CorruptionError();
                return;
            }

            Slice mid_key(key_ptr, non_shared);
            if (mid_key.compare(target) < 0) {
                left = mid;
            } else {
                right = mid - 1;
            }
        }

        // Seek to the selected restart point and scan linearly
        SeekToRestartPoint(left);
        while (true) {
            if (!ParseNextKey()) {
                return;
            }
            if (Slice(key_).compare(target) >= 0) {
                return;
            }
        }
    }

    void SeekToFirst() override {
        if (num_restarts_ == 0) {
            current_ = restarts_offset_;
            return;
        }
        SeekToRestartPoint(0);
        ParseNextKey();
    }

    void SeekToLast() override {
        if (num_restarts_ == 0) {
            current_ = restarts_offset_;
            return;
        }
        SeekToRestartPoint(num_restarts_ - 1);
        while (ParseNextKey()) {
            if (NextEntryOffset() >= restarts_offset_) {
                break;
            }
        }
    }

private:
    uint32_t GetRestartPoint(uint32_t index) const {
        return DecodeFixed32(data_ + restarts_offset_ + index * sizeof(uint32_t));
    }

    void SeekToRestartPoint(uint32_t index) {
        key_.clear();
        restart_index_ = index;
        current_ = GetRestartPoint(index);
        value_ = Slice(data_ + current_, 0);
    }

    uint32_t NextEntryOffset() const {
        return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
    }

    const char* DecodeEntry(const char* p, const char* limit,
                            uint32_t* shared, uint32_t* non_shared, uint32_t* value_length) {
        if (p >= limit) return nullptr;
        p = GetVarint32Ptr(p, limit, shared);
        if (p == nullptr) return nullptr;
        p = GetVarint32Ptr(p, limit, non_shared);
        if (p == nullptr) return nullptr;
        p = GetVarint32Ptr(p, limit, value_length);
        if (p == nullptr) return nullptr;

        if (static_cast<size_t>(limit - p) < (*non_shared + *value_length)) {
            return nullptr;
        }
        return p;
    }

    bool ParseNextKey() {
        current_ = NextEntryOffset();
        const char* p = data_ + current_;
        const char* limit = data_ + restarts_offset_;
        if (p >= limit) {
            current_ = restarts_offset_;
            return false;
        }

        uint32_t shared = 0, non_shared = 0, value_length = 0;
        p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
        if (p == nullptr || key_.size() < shared) {
            CorruptionError();
            return false;
        }

        key_.resize(shared);
        key_.append(p, non_shared);
        value_ = Slice(p + non_shared, value_length);

        while (restart_index_ + 1 < num_restarts_ &&
               GetRestartPoint(restart_index_ + 1) <= current_) {
            restart_index_++;
        }

        return true;
    }

    void CorruptionError() {
        current_ = restarts_offset_;
        restart_index_ = num_restarts_;
        status_ = Status::Corruption("Bad entry in BlockIterator");
        key_.clear();
        value_ = Slice();
    }

    const char* data_{nullptr};
    uint32_t restarts_offset_{0};
    uint32_t num_restarts_{0};
    uint32_t current_{0};
    uint32_t restart_index_{0};
    std::string key_;
    Slice value_;
    Status status_{Status::OK()};
};

BlockReader::BlockReader(Slice contents)
    : data_(contents.data()), size_(contents.size()) {
    if (size_ < sizeof(uint32_t)) {
        size_ = 0;
        num_restarts_ = 0;
        restart_offset_ = 0;
    } else {
        size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
        num_restarts_ = DecodeFixed32(data_ + size_ - sizeof(uint32_t));
        if (num_restarts_ > max_restarts_allowed) {
            // Corrupt block
            size_ = 0;
            num_restarts_ = 0;
            restart_offset_ = 0;
        } else {
            restart_offset_ = static_cast<uint32_t>(size_ - (1 + num_restarts_) * sizeof(uint32_t));
        }
    }
}

std::unique_ptr<Iterator> BlockReader::NewIterator() const {
    if (size_ == 0) {
        return std::make_unique<BlockIterator>(nullptr, 0, 0);
    }
    return std::make_unique<BlockIterator>(data_, restart_offset_, num_restarts_);
}

} // namespace lsm
