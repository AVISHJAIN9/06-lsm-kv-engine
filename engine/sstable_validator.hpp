#pragma once

#include <string>
#include <cstdint>
#include "status.hpp"

namespace lsm {

struct ValidationReport {
    bool is_valid{true};
    uint64_t file_size{0};
    uint64_t num_data_blocks{0};
    uint64_t num_keys{0};
    uint64_t data_bytes_verified{0};
    std::string smallest_key;
    std::string largest_key;

    Status failure_status{Status::OK()};
    uint64_t failure_offset{0};
    std::string failure_reason;

    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// SSTableValidator: Structural, CRC32C, and Key Monotonicity Verifier
// -----------------------------------------------------------------------------
class SSTableValidator {
public:
    static Status ValidateFile(const std::string& filename, ValidationReport* report = nullptr);
};

} // namespace lsm
