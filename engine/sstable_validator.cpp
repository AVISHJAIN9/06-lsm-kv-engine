#include "sstable_validator.hpp"
#include "format.hpp"
#include "block_reader.hpp"
#include "block_trailer_envelope.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <filesystem>

namespace lsm {

namespace fs = std::filesystem;

std::string ValidationReport::ToString() const {
    std::ostringstream oss;
    oss << "SSTable Validation Report:\n"
        << "  - Status:             " << (is_valid ? "VALID (100% Integrity Verified)" : "CORRUPT") << "\n"
        << "  - File Size:          " << file_size << " bytes\n"
        << "  - Data Blocks:        " << num_data_blocks << "\n"
        << "  - Keys Verified:      " << num_keys << "\n"
        << "  - Key Range:          ['" << smallest_key << "', '" << largest_key << "']\n";
    if (!is_valid) {
        oss << "  - Failure Offset:     " << failure_offset << "\n"
            << "  - Failure Detail:     " << failure_reason << "\n"
            << "  - Status Code:        " << failure_status.ToString() << "\n";
    }
    return oss.str();
}

Status SSTableValidator::ValidateFile(const std::string& filename, ValidationReport* report) {
    ValidationReport local_report;
    ValidationReport* rep = report ? report : &local_report;

    std::error_code ec;
    uint64_t file_size = fs::file_size(filename, ec);
    if (ec) {
        rep->is_valid = false;
        rep->failure_status = Status::IOError("File does not exist: " + filename);
        rep->failure_reason = ec.message();
        return rep->failure_status;
    }
    rep->file_size = file_size;

    if (file_size < Footer::kEncodedLength) {
        rep->is_valid = false;
        rep->failure_status = Status::Corruption("File size too small for SSTable footer");
        rep->failure_offset = 0;
        rep->failure_reason = "File size < 48 bytes";
        return rep->failure_status;
    }

    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        rep->is_valid = false;
        rep->failure_status = Status::IOError("Failed to open file: " + filename);
        return rep->failure_status;
    }

    // 1. Read & Validate Footer
    char footer_buf[Footer::kEncodedLength];
    ssize_t n = pread(fd, footer_buf, Footer::kEncodedLength, file_size - Footer::kEncodedLength);
    if (n != static_cast<ssize_t>(Footer::kEncodedLength)) {
        close(fd);
        rep->is_valid = false;
        rep->failure_status = Status::IOError("Failed to read footer bytes");
        rep->failure_offset = file_size - Footer::kEncodedLength;
        return rep->failure_status;
    }

    Footer footer;
    Slice footer_slice(footer_buf, Footer::kEncodedLength);
    Status s = footer.DecodeFrom(&footer_slice);
    if (!s.ok()) {
        close(fd);
        rep->is_valid = false;
        rep->failure_status = s;
        rep->failure_offset = file_size - Footer::kEncodedLength;
        rep->failure_reason = "Corrupt footer or invalid table magic constant";
        return s;
    }

    // 2. Read & Validate Metaindex Block
    const size_t meta_raw_len = footer.metaindex_handle().size() + BlockTrailerEnvelope::kTrailerSize;
    std::string meta_raw(meta_raw_len, '\0');
    n = pread(fd, &meta_raw[0], meta_raw_len, footer.metaindex_handle().offset());
    if (n != static_cast<ssize_t>(meta_raw_len)) {
        close(fd);
        rep->is_valid = false;
        rep->failure_status = Status::IOError("Failed to read metaindex block");
        rep->failure_offset = footer.metaindex_handle().offset();
        return rep->failure_status;
    }
    Slice meta_payload;
    s = BlockTrailerEnvelope::VerifyAndStripTrailer(Slice(meta_raw), &meta_payload);
    if (!s.ok()) {
        close(fd);
        rep->is_valid = false;
        rep->failure_status = s;
        rep->failure_offset = footer.metaindex_handle().offset();
        rep->failure_reason = "Metaindex block CRC32C checksum mismatch";
        return s;
    }

    // 3. Read & Validate Index Block
    const size_t index_raw_len = footer.index_handle().size() + BlockTrailerEnvelope::kTrailerSize;
    std::string index_raw(index_raw_len, '\0');
    n = pread(fd, &index_raw[0], index_raw_len, footer.index_handle().offset());
    if (n != static_cast<ssize_t>(index_raw_len)) {
        close(fd);
        rep->is_valid = false;
        rep->failure_status = Status::IOError("Failed to read index block");
        rep->failure_offset = footer.index_handle().offset();
        return rep->failure_status;
    }
    Slice index_payload;
    s = BlockTrailerEnvelope::VerifyAndStripTrailer(Slice(index_raw), &index_payload);
    if (!s.ok()) {
        close(fd);
        rep->is_valid = false;
        rep->failure_status = s;
        rep->failure_offset = footer.index_handle().offset();
        rep->failure_reason = "Index block CRC32C checksum mismatch";
        return s;
    }

    // 4. Iterate and validate every data block and its key monotonicity
    BlockReader index_reader(index_payload);
    auto index_iter = index_reader.NewIterator();
    index_iter->SeekToFirst();

    std::string previous_key;
    bool has_prev_key = false;

    while (index_iter->Valid()) {
        Slice handle_slice = index_iter->value();
        BlockHandle data_handle;
        s = data_handle.DecodeFrom(&handle_slice);
        if (!s.ok()) {
            close(fd);
            rep->is_valid = false;
            rep->failure_status = s;
            rep->failure_reason = "Corrupt BlockHandle in index block";
            return s;
        }

        const size_t data_raw_len = data_handle.size() + BlockTrailerEnvelope::kTrailerSize;
        if (data_handle.offset() + data_raw_len > file_size) {
            close(fd);
            rep->is_valid = false;
            rep->failure_status = Status::Corruption("Data block extends beyond EOF");
            rep->failure_offset = data_handle.offset();
            rep->failure_reason = "Data block offset + length > file_size";
            return rep->failure_status;
        }

        std::string data_raw(data_raw_len, '\0');
        n = pread(fd, &data_raw[0], data_raw_len, data_handle.offset());
        if (n != static_cast<ssize_t>(data_raw_len)) {
            close(fd);
            rep->is_valid = false;
            rep->failure_status = Status::IOError("Failed to read data block");
            rep->failure_offset = data_handle.offset();
            return rep->failure_status;
        }

        Slice data_payload;
        s = BlockTrailerEnvelope::VerifyAndStripTrailer(Slice(data_raw), &data_payload);
        if (!s.ok()) {
            close(fd);
            rep->is_valid = false;
            rep->failure_status = s;
            rep->failure_offset = data_handle.offset();
            rep->failure_reason = "Data block CRC32C checksum mismatch";
            return s;
        }

        BlockReader data_reader(data_payload);
        auto data_iter = data_reader.NewIterator();
        data_iter->SeekToFirst();

        while (data_iter->Valid()) {
            Slice curr_key = data_iter->key();
            if (rep->num_keys == 0) {
                rep->smallest_key = curr_key.ToString();
            }
            rep->largest_key = curr_key.ToString();

            if (has_prev_key && curr_key.compare(Slice(previous_key)) < 0) {
                close(fd);
                rep->is_valid = false;
                rep->failure_status = Status::Corruption("Non-monotonic out-of-order key encountered");
                rep->failure_offset = data_handle.offset();
                rep->failure_reason = "Key '" + curr_key.ToString() + "' < previous key '" + previous_key + "'";
                return rep->failure_status;
            }

            previous_key = curr_key.ToString();
            has_prev_key = true;
            rep->num_keys++;
            data_iter->Next();
        }

        rep->num_data_blocks++;
        rep->data_bytes_verified += data_raw_len;
        index_iter->Next();
    }

    close(fd);
    rep->is_valid = true;
    rep->failure_status = Status::OK();
    return Status::OK();
}

} // namespace lsm
