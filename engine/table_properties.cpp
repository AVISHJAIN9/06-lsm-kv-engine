#include "table_properties.hpp"
#include "block_builder.hpp"
#include "block_reader.hpp"
#include <sstream>
#include <iomanip>

namespace lsm {

void TableProperties::EncodeTo(std::string* dst) const {
    BlockBuilder builder(1);

    builder.Add(Slice("stats.data_size"), Slice(std::to_string(data_size)));
    builder.Add(Slice("stats.filter_size"), Slice(std::to_string(filter_size)));
    builder.Add(Slice("stats.format_version"), Slice(std::to_string(format_version)));
    builder.Add(Slice("stats.index_size"), Slice(std::to_string(index_size)));
    builder.Add(Slice("stats.largest_key"), Slice(largest_key));
    builder.Add(Slice("stats.max_seq"), Slice(std::to_string(max_seq)));
    builder.Add(Slice("stats.min_seq"), Slice(std::to_string(min_seq)));
    builder.Add(Slice("stats.num_data_blocks"), Slice(std::to_string(num_data_blocks)));
    builder.Add(Slice("stats.num_entries"), Slice(std::to_string(num_entries)));
    builder.Add(Slice("stats.raw_key_size"), Slice(std::to_string(raw_key_size)));
    builder.Add(Slice("stats.raw_value_size"), Slice(std::to_string(raw_value_size)));
    builder.Add(Slice("stats.smallest_key"), Slice(smallest_key));
    builder.Add(Slice("user.column_family"), Slice(column_family_name));
    builder.Add(Slice("user.feature_flags"), Slice(std::to_string(feature_flags)));
    builder.Add(Slice("user.filter_policy"), Slice(filter_policy_name));

    Slice block_slice = builder.Finish();
    dst->append(block_slice.data(), block_slice.size());
}

Status TableProperties::DecodeFrom(const Slice& input) {
    if (input.empty()) {
        return Status::Corruption("Empty input for TableProperties::DecodeFrom");
    }

    BlockReader reader(input);
    auto iter = reader.NewIterator();
    if (!iter) {
        return Status::Corruption("Failed to create iterator for TableProperties block");
    }

    iter->SeekToFirst();
    while (iter->Valid()) {
        std::string prop_name = iter->key().ToString();
        std::string prop_val = iter->value().ToString();

        try {
            if (prop_name == "stats.data_size") {
                data_size = std::stoull(prop_val);
            } else if (prop_name == "stats.index_size") {
                index_size = std::stoull(prop_val);
            } else if (prop_name == "stats.filter_size") {
                filter_size = std::stoull(prop_val);
            } else if (prop_name == "stats.raw_key_size") {
                raw_key_size = std::stoull(prop_val);
            } else if (prop_name == "stats.raw_value_size") {
                raw_value_size = std::stoull(prop_val);
            } else if (prop_name == "stats.num_entries") {
                num_entries = std::stoull(prop_val);
            } else if (prop_name == "stats.num_data_blocks") {
                num_data_blocks = std::stoull(prop_val);
            } else if (prop_name == "stats.smallest_key") {
                smallest_key = prop_val;
            } else if (prop_name == "stats.largest_key") {
                largest_key = prop_val;
            } else if (prop_name == "stats.min_seq") {
                min_seq = std::stoull(prop_val);
            } else if (prop_name == "stats.max_seq") {
                max_seq = std::stoull(prop_val);
            } else if (prop_name == "stats.format_version") {
                format_version = std::stoul(prop_val);
            } else if (prop_name == "user.feature_flags") {
                feature_flags = std::stoul(prop_val);
            } else if (prop_name == "user.column_family") {
                column_family_name = prop_val;
            } else if (prop_name == "user.filter_policy") {
                filter_policy_name = prop_val;
            }
        } catch (...) {
            return Status::Corruption("Invalid numerical format in property: " + prop_name);
        }

        iter->Next();
    }

    return iter->status();
}

std::string TableProperties::ToString() const {
    std::ostringstream oss;
    oss << "SSTable Properties & Range Statistics:\n"
        << "  - Entries Count:      " << num_entries << "\n"
        << "  - Data Blocks:        " << num_data_blocks << "\n"
        << "  - Data Size:          " << data_size << " bytes\n"
        << "  - Index Size:         " << index_size << " bytes\n"
        << "  - Filter Size:        " << filter_size << " bytes\n"
        << "  - Raw Key Bytes:      " << raw_key_size << " bytes\n"
        << "  - Raw Value Bytes:    " << raw_value_size << " bytes\n"
        << "  - Smallest Key:       '" << smallest_key << "'\n"
        << "  - Largest Key:        '" << largest_key << "'\n"
        << "  - Sequence Range:     [" << min_seq << ", " << max_seq << "]\n"
        << "  - Format Version:     v" << format_version << "\n"
        << "  - Feature Flags:      0x" << std::hex << feature_flags << std::dec << "\n"
        << "  - Filter Policy:      " << filter_policy_name << "\n";
    return oss.str();
}

bool TableProperties::KeyInRange(const Slice& user_key) const {
    if (smallest_key.empty() && largest_key.empty()) return true;
    if (!smallest_key.empty() && user_key.compare(Slice(smallest_key)) < 0) return false;
    if (!largest_key.empty() && user_key.compare(Slice(largest_key)) > 0) return false;
    return true;
}

bool TableProperties::Overlaps(const Slice& smallest, const Slice& largest) const {
    if (smallest_key.empty() || largest_key.empty()) return true;
    if (!largest.empty() && largest.compare(Slice(smallest_key)) < 0) return false;
    if (!smallest.empty() && smallest.compare(Slice(largest_key)) > 0) return false;
    return true;
}

} // namespace lsm
