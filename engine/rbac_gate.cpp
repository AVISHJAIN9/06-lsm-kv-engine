#include "rbac_gate.hpp"

namespace lsm {

RBACGate::RBACGate() {
    // Default built-in roles
    RegisterRole("readonly", kPermRead);
    RegisterRole("readwrite", kPermRead | kPermWrite | kPermDelete);
    RegisterRole("admin", kPermAll);
}

void RBACGate::RegisterRole(const std::string& role, uint32_t permissions_mask) {
    std::lock_guard<std::mutex> lock(mutex_);
    role_permissions_[role] = permissions_mask;
}

SecurityToken RBACGate::IssueToken(const std::string& user_id, const std::vector<std::string>& roles) const {
    std::lock_guard<std::mutex> lock(mutex_);
    SecurityToken token;
    token.user_id = user_id;
    token.roles = roles;
    token.permissions_mask = kPermNone;

    for (const auto& r : roles) {
        auto it = role_permissions_.find(r);
        if (it != role_permissions_.end()) {
            token.permissions_mask |= it->second;
        }
    }

    token.signature = "SIG_" + user_id + "_" + std::to_string(token.permissions_mask);
    return token;
}

Status RBACGate::Authorize(
    const SecurityToken& token,
    Permission requested_permission,
    uint32_t cf_id
) const {
    (void)cf_id;
    if (token.HasPermission(requested_permission)) {
        return Status::OK();
    }
    return Status::InvalidArgument("Access denied: NotAuthorized for user " + token.user_id);
}

} // namespace lsm
