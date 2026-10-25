#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "column_family.hpp"

namespace lsm {

// CFWriteBatch: Cross-Column-Family multi-operation atomic write batch (Feature 99).
// Encodes cf_id varint prefix per record, enabling atomic all-or-nothing mutations across distinct CFs.
class CFWriteBatch {
public:
    class Handler {
    public:
        virtual ~Handler() = default;
        virtual void Put(uint32_t cf_id, const Slice& key, const Slice& value) = 0;
        virtual void Delete(uint32_t cf_id, const Slice& key) = 0;
    };

    CFWriteBatch();
    ~CFWriteBatch() = default;

    CFWriteBatch(const CFWriteBatch&) = default;
    CFWriteBatch& operator=(const CFWriteBatch&) = default;
    CFWriteBatch(CFWriteBatch&&) noexcept = default;
    CFWriteBatch& operator=(CFWriteBatch&&) noexcept = default;

    // Mutators
    void Put(uint32_t cf_id, const Slice& key, const Slice& value);
    void Put(ColumnFamilyHandle* handle, const Slice& key, const Slice& value);

    void Delete(uint32_t cf_id, const Slice& key);
    void Delete(ColumnFamilyHandle* handle, const Slice& key);

    void Clear();

    // Accessors & Metainfo
    uint32_t Count() const;
    void SetCount(uint32_t n);

    SequenceNumber Sequence() const;
    void SetSequence(SequenceNumber seq);

    Status Iterate(Handler* handler) const;

    size_t ByteSize() const { return rep_.size(); }
    const std::string& Data() const { return rep_; }

private:
    std::string rep_{}; // [Count: 4B] [Sequence: 8B] [Record 0 .. Record N]
};

} // namespace lsm
