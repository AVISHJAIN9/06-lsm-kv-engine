#pragma once

#include <string>
#include <cstdint>
#include <map>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// TableProperties: Metadata and range statistics stored in SSTable meta-blocks
// -----------------------------------------------------------------------------
struct TableProperties {
    uint64_t data_size{0};
    uint64_t index_size{0};
    uint64_t filter_size{0};
    uint64_t raw_key_size{0};
    uint64_t raw_value_size{0};
    uint64_t num_entries{0};
    uint64_t num_data_blocks{0};

    std::string smallest_key;
    std::string largest_key;
    SequenceNumber min_seq{0};
    SequenceNumber max_seq{0};

    uint32_t format_version{1};
    uint32_t feature_flags{0};

    std::string column_family_name{"default"};
    std::string filter_policy_name{"lsm.BuiltinBloomFilter2"};

    // Encodes properties into a key-value property block format
    void EncodeTo(std::string* dst) const;

    // Decodes properties from a serialized property block
    Status DecodeFrom(const Slice& input);

    // Human-readable summary
    std::string ToString() const;

    // Checks if key is within [smallest_key, largest_key]
    bool KeyInRange(const Slice& user_key) const;

    // Checks if [smallest, largest] overlaps with this table's range
    bool Overlaps(const Slice& smallest, const Slice& largest) const;
};

// Reserved metaindex key for table properties
static const std::string kTablePropertiesKey = "stats.table_properties";

} // namespace lsm
