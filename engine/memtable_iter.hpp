#pragma once

#include "iterator.hpp"
#include "memtable_core.hpp"
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"

namespace lsm {

class MemTableIter : public Iterator {
public:
    explicit MemTableIter(MemTableCore::Table* table);
    ~MemTableIter() override = default;

    bool Valid() const override;
    void Seek(const Slice& target) override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;
    Status status() const override { return status_; }

    // Memtable specific field accessors
    Slice user_key() const;
    SequenceNumber sequence() const;
    ValueType type() const;

private:
    MemTableCore::Table::Iterator iter_;
    std::string tmp_key_buf_{};
    Status status_{Status::OK()};

    static Slice ExtractUserKey(const char* entry);
    static Slice ExtractValue(const char* entry);
    static uint64_t ExtractTag(const char* entry);
};

// -----------------------------------------------------------------------------
// MemTableInternalIterator: Yields InternalKey format (user_key + 8-byte tag)
// -----------------------------------------------------------------------------
class MemTableInternalIterator : public Iterator {
public:
    explicit MemTableInternalIterator(std::unique_ptr<Iterator> iter)
        : iter_(std::move(iter)) {}

    bool Valid() const override { return iter_ && iter_->Valid(); }
    void SeekToFirst() override { if (iter_) iter_->SeekToFirst(); }
    void SeekToLast() override { if (iter_) iter_->SeekToLast(); }
    void Seek(const Slice& target) override {
        if (!iter_) return;
        if (target.size() >= 8) {
            InternalKey ikey;
            ikey.DecodeFrom(target);
            iter_->Seek(ikey.user_key());
        } else {
            iter_->Seek(target);
        }
    }
    void Next() override { if (iter_) iter_->Next(); }
    void Prev() override { if (iter_) iter_->Prev(); }

    Slice key() const override {
        if (!Valid()) return Slice();
        auto m_iter = dynamic_cast<MemTableIter*>(iter_.get());
        if (m_iter) {
            saved_key_ = InternalKey(m_iter->user_key(), m_iter->sequence(), m_iter->type());
            return saved_key_.Encode();
        }
        return iter_->key();
    }

    Slice value() const override {
        if (!Valid()) return Slice();
        return iter_->value();
    }

    Status status() const override { return iter_ ? iter_->status() : Status::OK(); }

private:
    std::unique_ptr<Iterator> iter_;
    mutable InternalKey saved_key_;
};

} // namespace lsm
