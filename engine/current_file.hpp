#pragma once

#include <string>
#include <cstdint>
#include "status.hpp"

namespace lsm {

// Synchronizes the root CURRENT pointer to active MANIFEST file (Feature 94).
// Employs atomic temp-file creation, posix fdatasync/fsync, atomic rename,
// and parent directory fsync to guarantee zero torn reads and power-loss resilience.
Status SetCurrentFile(const std::string& dbname, uint64_t manifest_file_number);

// Reads and validates the CURRENT file pointing to the active manifest name.
Status ReadCurrentFile(const std::string& dbname, std::string* out_manifest_filename);

} // namespace lsm
