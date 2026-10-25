#pragma once

#include <string>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"

namespace lsm {

class WriteBatch {
public:
    class Handler {
    public:
        virtual ~Handler() = default;
        virtual void Put(const Slice& key, const Slice& value) = 0;
        virtual void Delete(const Slice& key) = 0;
    };

    WriteBatch();
    ~WriteBatch() = default;

    void Put(const Slice& key, const Slice& value);
    void Delete(const Slice& key);
    void Clear();

    size_t ByteSize() const { return rep_.size(); }
    uint32_t Count() const;
    void SetCount(uint32_t n);

    SequenceNumber Sequence() const;
    void SetSequence(SequenceNumber seq);

    Status Iterate(Handler* handler) const;
    void Append(const WriteBatch& source);

    const std::string& Data() const { return rep_; }

private:
    friend class WriteBatchInternal;
    std::string rep_{}; // 8-byte sequence + 4-byte count + entries
};

class WriteBatchInternal {
public:
    static uint32_t Count(const WriteBatch* batch) { return batch->Count(); }
    static void SetCount(WriteBatch* batch, uint32_t n) { batch->SetCount(n); }
    static SequenceNumber Sequence(const WriteBatch* batch) { return batch->Sequence(); }
    static void SetSequence(WriteBatch* batch, SequenceNumber seq) { batch->SetSequence(seq); }
    static Slice Contents(const WriteBatch* batch) { return Slice(batch->rep_); }
    static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }
    static void SetContents(WriteBatch* batch, const Slice& contents) {
        batch->rep_.assign(contents.data(), contents.size());
    }

    static SequenceNumber Sequence(const Slice* src) {
        if (!src || src->size() < 8) return 0;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(src->data());
        uint64_t seq = 0;
        for (int i = 0; i < 8; ++i) {
            seq |= (static_cast<uint64_t>(p[i]) << (i * 8));
        }
        return seq;
    }

    static uint32_t Count(const Slice* src) {
        if (!src || src->size() < 12) return 0;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(src->data() + 8);
        uint32_t c = 0;
        for (int i = 0; i < 4; ++i) {
            c |= (static_cast<uint32_t>(p[i]) << (i * 8));
        }
        return c;
    }
};

} // namespace lsm
