#include "cf_flush_controller.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace lsm {

std::vector<std::shared_ptr<ColumnFamilyData>> CFFlushController::PickMemtablesToFlush(
    ColumnFamilySet* cf_set
) {
    std::vector<std::shared_ptr<ColumnFamilyData>> to_flush;
    if (!cf_set) return to_flush;

    auto all_cfds = cf_set->GetAllCFDs();
    for (const auto& cfd : all_cfds) {
        if (!cfd || cfd->IsDropped()) continue;

        if (cfd->TotalMemoryUsage() >= cfd->GetOptions().write_buffer_size ||
            !cfd->GetImmutableQueue().empty()) {
            to_flush.push_back(cfd);
        }
    }
    return to_flush;
}

Status CFFlushController::FlushColumnFamily(
    const std::string& dbname,
    ColumnFamilyData* cfd,
    uint64_t sstable_file_number,
    SequenceNumber current_max_seq
) {
    if (!cfd) {
        return Status::InvalidArgument("Null ColumnFamilyData in FlushColumnFamily");
    }

    // 1. Rotate active memtable into immutable queue
    cfd->RotateMemTable();
    auto imm_list = cfd->GetImmutableQueue();
    if (imm_list.empty()) {
        return Status::OK();
    }

    // 2. Open SSTable output file
    char sst_name[64];
    snprintf(sst_name, sizeof(sst_name), "/%06llu.sst", static_cast<unsigned long long>(sstable_file_number));
    std::string sst_path = dbname + sst_name;

    TableOptions table_opts;
    TableBuilder builder(table_opts, sst_path);

    std::string smallest_key, largest_key;
    bool first_key = true;

    // 3. Write entries from all immutable memtables into SSTable
    for (const auto& mem : imm_list) {
        if (!mem) continue;
        auto it = mem->NewIterator();
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            Slice k = it->key();
            Slice v = it->value();
            builder.Add(k, v);

            if (first_key) {
                smallest_key = k.ToString();
                first_key = false;
            }
            largest_key = k.ToString();
        }
    }

    Status s = builder.Finish();
    if (!s.ok()) {
        return s;
    }

    // 4. Update CF Level 0 metadata and flush watermarks
    FileMetaData meta;
    meta.file_number = sstable_file_number;
    meta.filename = sst_path;
    meta.file_size = builder.FileSize();
    meta.smallest_key = smallest_key;
    meta.largest_key = largest_key;
    meta.min_seq = cfd->GetEarliestMemSeq();
    meta.max_seq = current_max_seq;

    cfd->AddLevelFile(0, meta);
    cfd->ClearImmutableQueue();
    cfd->SetLastFlushedSeq(current_max_seq);

    return Status::OK();
}

SequenceNumber CFFlushController::GetGlobalMinFlushedLSN(ColumnFamilySet* cf_set) {
    if (!cf_set) return 0;

    auto all_cfds = cf_set->GetAllCFDs();
    if (all_cfds.empty()) return 0;

    SequenceNumber min_flushed = kMaxSequenceNumber;
    bool found_active = false;

    for (const auto& cfd : all_cfds) {
        if (!cfd || cfd->IsDropped()) continue;
        SequenceNumber seq = cfd->GetLastFlushedSeq();
        SequenceNumber earliest_mem = cfd->GetEarliestMemSeq();

        // If CF has received writes or has flushed
        if (seq > 0 || earliest_mem > 0 || (cfd->GetActiveMemTable() && cfd->GetActiveMemTable()->Count() > 0)) {
            found_active = true;
            if (seq < min_flushed) {
                min_flushed = seq;
            }
        }
    }

    return found_active ? min_flushed : 0;
}

bool CFFlushController::IsWALSafeToUnlink(ColumnFamilySet* cf_set, SequenceNumber wal_max_seq) {
    SequenceNumber global_min = GetGlobalMinFlushedLSN(cf_set);
    return global_min >= wal_max_seq;
}

} // namespace lsm
