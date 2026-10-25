#include "compaction_input_selector.hpp"
#include <algorithm>

namespace lsm {

void CompactionInputSelector::GetRange(
    const std::vector<FileMetaData>& files,
    std::string* out_smallest,
    std::string* out_largest
) {
    if (!out_smallest || !out_largest || files.empty()) return;

    out_smallest->clear();
    out_largest->clear();

    for (const auto& f : files) {
        if (out_smallest->empty() || Slice(f.smallest_key).compare(Slice(*out_smallest)) < 0) {
            *out_smallest = f.smallest_key;
        }
        if (out_largest->empty() || Slice(f.largest_key).compare(Slice(*out_largest)) > 0) {
            *out_largest = f.largest_key;
        }
    }
}

void CompactionInputSelector::GetOverlappingInputs(
    const Slice& user_smallest,
    const Slice& user_largest,
    const std::vector<FileMetaData>& level_files,
    std::vector<FileMetaData>* out_overlapping
) {
    if (!out_overlapping || user_smallest.empty() || user_largest.empty()) return;

    for (const auto& file : level_files) {
        // [file.smallest_key, file.largest_key] intersects [user_smallest, user_largest]
        if (Slice(file.smallest_key).compare(user_largest) <= 0 &&
            Slice(file.largest_key).compare(user_smallest) >= 0) {
            out_overlapping->push_back(file);
        }
    }
}

void CompactionInputSelector::ExpandInputs(
    const std::vector<FileMetaData>& level_files,
    const std::vector<FileMetaData>& next_level_files,
    std::vector<FileMetaData>* inout_level_inputs,
    std::vector<FileMetaData>* inout_next_level_inputs
) {
    if (!inout_level_inputs || !inout_next_level_inputs || inout_level_inputs->empty()) return;

    std::string smallest, largest;
    GetRange(*inout_level_inputs, &smallest, &largest);

    inout_next_level_inputs->clear();
    GetOverlappingInputs(Slice(smallest), Slice(largest), next_level_files, inout_next_level_inputs);

    if (inout_next_level_inputs->empty()) return;

    std::string all_smallest, all_largest;
    GetRange(*inout_next_level_inputs, &all_smallest, &all_largest);

    // Expand Level i inputs within the boundary of Level i+1 without expanding Level i+1
    std::vector<FileMetaData> expanded_level_inputs;
    GetOverlappingInputs(Slice(all_smallest), Slice(all_largest), level_files, &expanded_level_inputs);

    if (expanded_level_inputs.size() > inout_level_inputs->size()) {
        std::string new_smallest, new_largest;
        GetRange(expanded_level_inputs, &new_smallest, &new_largest);

        std::vector<FileMetaData> new_next_level_inputs;
        GetOverlappingInputs(Slice(new_smallest), Slice(new_largest), next_level_files, &new_next_level_inputs);

        if (new_next_level_inputs.size() == inout_next_level_inputs->size()) {
            *inout_level_inputs = std::move(expanded_level_inputs);
            *inout_next_level_inputs = std::move(new_next_level_inputs);
        }
    }
}

} // namespace lsm
