#include "conflict_detector.hpp"

namespace lsm {

Status ConflictDetector::ValidateConflict(
    const std::unordered_set<std::string>& read_set,
    const std::unordered_set<std::string>& write_set,
    SequenceNumber snapshot_seq,
    MemTableCore* memtable
) {
    if (!memtable) {
        return Status::OK();
    }

    auto check_key = [&](const std::string& key) -> bool {
        // Query latest committed version in memtable
        std::unique_ptr<Iterator> it(memtable->NewIterator());
        if (!it) return false;

        it->Seek(Slice(key));
        if (it->Valid() && it->key().compare(Slice(key)) == 0) {
            // Found matching key in memtable; check its sequence number
            // Note: MemTableCore iter gives key & value, point lookup gives exact seq
            std::string val;
            Status s;
            // Point lookup at max sequence to find latest committed sequence
            bool exists = memtable->Get(Slice(key), kMaxSequenceNumber, &val, &s);
            if (exists) {
                // To check if modified since snapshot_seq, check if visible at snapshot_seq vs kMaxSequenceNumber
                std::string snap_val;
                Status snap_s;
                bool snap_exists = memtable->Get(Slice(key), snapshot_seq, &snap_val, &snap_s);

                // If exists at max seq but either not at snapshot_seq or different result, check sequence directly
                if (!snap_exists && exists) {
                    return true; // Conflict: created after snapshot!
                }
                if (snap_exists && exists) {
                    if (snap_s.ok() != s.ok() || snap_val != val) {
                        return true; // Conflict: updated after snapshot!
                    }
                }
            }
        }
        return false;
    };

    // 1. Check all keys in read_set
    for (const auto& key : read_set) {
        if (check_key(key)) {
            return Status::Busy("Write-write / read-write conflict detected on key: " + key);
        }
    }

    // 2. Check all keys in write_set
    for (const auto& key : write_set) {
        if (check_key(key)) {
            return Status::Busy("Write-write conflict detected on key: " + key);
        }
    }

    return Status::OK();
}

} // namespace lsm
