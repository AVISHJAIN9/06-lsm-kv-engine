#pragma once

#include <string>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "format.hpp"
#include "index_reader.hpp"
#include "table_properties.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// IndexPinpointLookup: Fast sparse index binary search block handle resolver
// Latency <= 1.5 us per pinpoint query
// -----------------------------------------------------------------------------
class IndexPinpointLookup {
public:
    explicit IndexPinpointLookup(const IndexReader* index_reader);

    // Resolves candidate data block handle for user_key via sparse index binary search
    bool FindBlockHandle(const Slice& user_key, BlockHandle* out_handle) const;

    // Checks key-range boundaries and resolves candidate block handle
    Status PinpointBlock(
        const Slice& user_key,
        const TableProperties& properties,
        BlockHandle* out_handle
    ) const;

private:
    const IndexReader* index_reader_{nullptr};
};

} // namespace lsm
