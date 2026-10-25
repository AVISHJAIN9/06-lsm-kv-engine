#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "status.hpp"

namespace lsm {

enum Permission : uint32_t {
    kPermNone   = 0,
    kPermRead   = 1 << 0,
    kPermWrite  = 1 << 1,
    kPermDelete = 1 << 2,
    kPermAdmin  = 1 << 3,
    kPermAll    = 0xFFFFFFFF
};

struct SecurityToken {
    std::string user_id;
    std::vector<std::string> roles;
    uint32_t permissions_mask{kPermNone};
    std::string signature;

    bool HasPermission(Permission p) const {
        return (permissions_mask & static_cast<uint32_t>(p)) == static_cast<uint32_t>(p);
    }
};

// RBACGate: Sub-10ns bitmask client authorization gate and role-based access controller (Feature 142)
class RBACGate {
public:
    RBACGate();
    ~RBACGate() = default;

    // Registers a role and its associated permission bitmask
    void RegisterRole(const std::string& role, uint32_t permissions_mask);

    // Issues an authenticated SecurityToken combining all assigned roles
    SecurityToken IssueToken(const std::string& user_id, const std::vector<std::string>& roles) const;

    // Fast O(1) bitmask authorization check
    Status Authorize(
        const SecurityToken& token,
        Permission requested_permission,
        uint32_t cf_id = 0
    ) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, uint32_t> role_permissions_;
};

} // namespace lsm
