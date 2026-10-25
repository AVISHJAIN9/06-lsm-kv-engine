#pragma once

#include <vector>
#include <string>
#include "slice.hpp"
#include "leveled_compaction.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// CompactionInputSelector: Disjoint key-range overlap resolver and range expansion
// -----------------------------------------------------------------------------
class CompactionInputSelector {
public:
    // Finds all files in level_files that overlap with [user_smallest, user_largest]
    static void GetOverlappingInputs(
        const Slice& user_smallest,
        const Slice& user_largest,
        const std::vector<FileMetaData>& level_files,
        std::vector<FileMetaData>* out_overlapping
    );

    // Computes aggregate key span [smallest, largest] across a set of files
    static void GetRange(
        const std::vector<FileMetaData>& files,
        std::string* out_smallest,
        std::string* out_largest
    );

    // Safely expands Level i inputs if adjacent files do not grow Level i+1 footprint
    static void ExpandInputs(
        const std::vector<FileMetaData>& level_files,
        const std::vector<FileMetaData>& next_level_files,
        std::vector<FileMetaData>* inout_level_inputs,
        std::vector<FileMetaData>* inout_next_level_inputs
    );
};

} // namespace lsm
