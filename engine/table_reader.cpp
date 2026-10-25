#include "table_reader.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace lsm {

// -----------------------------------------------------------------------------
// BlockCache Implementation
// -----------------------------------------------------------------------------
BlockCache::BlockCache(size_t capacity_bytes)
    : capacity_(capacity_bytes) {}

std::shared_ptr<std::string> BlockCache::Get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second;
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void BlockCache::Insert(const std::string& key, std::shared_ptr<std::string> value) {
    std::lock_guard<std::mutex> lock(mu_);
    map_[key] = std::move(value);
}

// -----------------------------------------------------------------------------
// TwoLevelTableIterator
// -----------------------------------------------------------------------------
class TwoLevelTableIterator : public Iterator {
public:
    TwoLevelTableIterator(const TableReader* reader)
        : reader_(reader),
          index_iter_(reader ? reader->NewIndexIterator() : nullptr) {}

    bool Valid() const override {
        return data_iter_ && data_iter_->Valid();
    }

    void SeekToFirst() override {
        if (!index_iter_) return;
        index_iter_->SeekToFirst();
        InitDataBlock();
        if (data_iter_) {
            data_iter_->SeekToFirst();
        }
    }

    void SeekToLast() override {
        if (!index_iter_) return;
        index_iter_->SeekToLast();
        InitDataBlock();
        if (data_iter_) {
            data_iter_->SeekToLast();
        }
    }

    void Seek(const Slice& target) override {
        if (!index_iter_) return;
        index_iter_->Seek(target);
        InitDataBlock();
        if (data_iter_) {
            data_iter_->Seek(target);
        }
        SkipEmptyBlocksForward();
    }

    void Next() override {
        if (!Valid()) return;
        data_iter_->Next();
        SkipEmptyBlocksForward();
    }

    void Prev() override {
        if (!Valid()) return;
        data_iter_->Prev();
        SkipEmptyBlocksBackward();
    }

    Slice key() const override {
        if (!Valid()) return Slice();
        return data_iter_->key();
    }

    Slice value() const override {
        if (!Valid()) return Slice();
        return data_iter_->value();
    }

    Status status() const override {
        if (!status_.ok()) return status_;
        if (data_iter_ && !data_iter_->status().ok()) return data_iter_->status();
        return Status::OK();
    }

private:
    void InitDataBlock() {
        if (!index_iter_ || !index_iter_->Valid()) {
            data_block_storage_.reset();
            data_reader_.reset();
            data_iter_.reset();
            return;
        }

        Slice handle_slice = index_iter_->value();
        BlockHandle handle;
        Status s = handle.DecodeFrom(&handle_slice);
        if (!s.ok()) {
            status_ = s;
            return;
        }

        std::string payload;
        s = reader_->ReadRawBlock(handle, &payload);
        if (!s.ok()) {
            status_ = s;
            return;
        }

        data_block_storage_ = std::make_shared<std::string>(std::move(payload));
        data_reader_ = std::make_unique<BlockReader>(Slice(*data_block_storage_));
        data_iter_ = data_reader_->NewIterator();
    }

    void SkipEmptyBlocksForward() {
        while (data_iter_ == nullptr || !data_iter_->Valid()) {
            if (!index_iter_ || !index_iter_->Valid()) {
                data_iter_.reset();
                return;
            }
            index_iter_->Next();
            InitDataBlock();
            if (data_iter_) {
                data_iter_->SeekToFirst();
            }
        }
    }

    void SkipEmptyBlocksBackward() {
        while (data_iter_ == nullptr || !data_iter_->Valid()) {
            if (!index_iter_ || !index_iter_->Valid()) {
                data_iter_.reset();
                return;
            }
            index_iter_->Prev();
            InitDataBlock();
            if (data_iter_) {
                data_iter_->SeekToLast();
            }
        }
    }

    const TableReader* reader_{nullptr};
    std::unique_ptr<Iterator> index_iter_;
    std::shared_ptr<std::string> data_block_storage_;
    std::unique_ptr<BlockReader> data_reader_;
    std::unique_ptr<Iterator> data_iter_;
    Status status_{Status::OK()};
};

// -----------------------------------------------------------------------------
// TableReader Implementation
// -----------------------------------------------------------------------------
TableReader::TableReader(
    TableOptions options,
    std::string filename,
    uint64_t file_size,
    int fd,
    std::shared_ptr<BlockCache> cache
) : options_(std::move(options)),
    filename_(std::move(filename)),
    file_size_(file_size),
    fd_(fd),
    block_cache_(std::move(cache)) {}

TableReader::~TableReader() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

Status TableReader::Open(
    const TableOptions& options,
    std::string filename,
    uint64_t file_size,
    std::shared_ptr<BlockCache> block_cache,
    std::unique_ptr<TableReader>* out_reader
) {
    if (file_size < Footer::kEncodedLength) {
        return Status::Corruption("SSTable file size too small for footer: " + filename);
    }

    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::IOError("Failed to open SSTable for reading: " + filename);
    }

    auto reader = std::unique_ptr<TableReader>(new TableReader(options, filename, file_size, fd, block_cache));
    Status s = reader->Init();
    if (!s.ok()) {
        return s;
    }

    if (out_reader) {
        *out_reader = std::move(reader);
    }
    return Status::OK();
}

Status TableReader::Init() {
    // 1. Read 48-byte Footer from end of file
    char footer_buf[Footer::kEncodedLength];
    ssize_t n = pread(fd_, footer_buf, Footer::kEncodedLength, file_size_ - Footer::kEncodedLength);
    if (n != static_cast<ssize_t>(Footer::kEncodedLength)) {
        return Status::IOError("Failed to read SSTable footer: " + filename_);
    }

    Slice footer_slice(footer_buf, Footer::kEncodedLength);
    Status s = footer_.DecodeFrom(&footer_slice);
    if (!s.ok()) {
        return s;
    }

    // 2. Read Index Block
    s = ReadRawBlock(footer_.index_handle(), &index_data_);
    if (!s.ok()) {
        return s;
    }
    index_reader_ = std::make_unique<IndexReader>(Slice(index_data_));

    // 3. Read MetaIndex Block and parse Filter & Properties blocks
    std::string metaindex_data;
    s = ReadRawBlock(footer_.metaindex_handle(), &metaindex_data);
    if (s.ok()) {
        Slice meta_slice(metaindex_data);
        BlockReader meta_reader(meta_slice);
        auto meta_iter = meta_reader.NewIterator();
        meta_iter->SeekToFirst();

        while (meta_iter->Valid()) {
            std::string key = meta_iter->key().ToString();
            Slice handle_slice = meta_iter->value();
            BlockHandle handle;
            if (handle.DecodeFrom(&handle_slice).ok()) {
                if (key.rfind("filter.", 0) == 0 && options_.filter_policy) {
                    std::string f_data;
                    if (ReadRawBlock(handle, &f_data).ok()) {
                        filter_data_ = std::move(f_data);
                        filter_reader_ = std::make_unique<FilterBlockReader>(options_.filter_policy.get(), Slice(filter_data_));
                    }
                } else if (key == kTablePropertiesKey) {
                    std::string p_data;
                    if (ReadRawBlock(handle, &p_data).ok()) {
                        properties_.DecodeFrom(Slice(p_data));
                    }
                }
            }
            meta_iter->Next();
        }
    }

    return Status::OK();
}

Status TableReader::ReadRawBlock(const BlockHandle& handle, std::string* out_payload) const {
    const size_t raw_len = handle.size() + BlockTrailerEnvelope::kTrailerSize;
    std::string raw_buf(raw_len, '\0');

    ssize_t n = pread(fd_, &raw_buf[0], raw_len, handle.offset());
    if (n != static_cast<ssize_t>(raw_len)) {
        return Status::IOError("Failed to read raw block from SSTable: " + filename_);
    }

    Slice payload;
    BlockTrailerEnvelope::CompressionType comp_type;
    Status s = BlockTrailerEnvelope::VerifyAndStripTrailer(Slice(raw_buf), &payload, &comp_type);
    if (!s.ok()) {
        return s;
    }

    if (out_payload) {
        *out_payload = payload.ToString();
    }
    return Status::OK();
}

Status TableReader::Get(const Slice& user_key, std::string* value) const {
    // 1. O(1) Key range check
    if (!properties_.KeyInRange(user_key)) {
        return Status::NotFound("Key not in table range");
    }

    // 2. Locate data block handle via sparse index
    BlockHandle handle;
    if (!index_reader_ || !index_reader_->FindBlockHandle(user_key, &handle)) {
        return Status::NotFound("Key not found in table index");
    }

    // 3. Bloom filter probe (avoids disk read on negative lookup)
    if (filter_reader_ && !filter_reader_->KeyMayMatch(handle.offset(), user_key)) {
        return Status::NotFound("Key rejected by Bloom filter");
    }

    // 4. Fetch data block (from BlockCache or disk)
    std::shared_ptr<std::string> block_storage;
    std::string cache_key = filename_ + ":" + std::to_string(handle.offset());

    if (block_cache_) {
        block_storage = block_cache_->Get(cache_key);
    }

    if (!block_storage) {
        std::string payload;
        Status s = ReadRawBlock(handle, &payload);
        if (!s.ok()) {
            return s;
        }
        block_storage = std::make_shared<std::string>(std::move(payload));
        if (block_cache_) {
            block_cache_->Insert(cache_key, block_storage);
        }
    }

    // 5. Binary search data block restart points
    Slice data_slice(*block_storage);
    BlockReader data_reader(data_slice);
    auto iter = data_reader.NewIterator();
    iter->Seek(user_key);

    if (iter->Valid() && iter->key() == user_key) {
        if (value) {
            *value = iter->value().ToString();
        }
        return Status::OK();
    }

    return Status::NotFound("Key not found in data block");
}

std::unique_ptr<Iterator> TableReader::NewIterator() const {
    return std::make_unique<TwoLevelTableIterator>(this);
}

} // namespace lsm
