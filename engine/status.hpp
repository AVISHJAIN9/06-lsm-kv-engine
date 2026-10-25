#pragma once

#include <string>
#include <ostream>
#include "slice.hpp"

namespace lsm {

class Status {
public:
    enum Code : uint8_t {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5,
        kBusy = 6,
        kAborted = 7
    };

    Status() : code_(kOk), state_("") {}
    Status(Code code, const Slice& msg) : code_(code), state_(msg.ToString()) {}

    static Status OK() { return Status(); }
    static Status NotFound(const Slice& msg = Slice()) { return Status(kNotFound, msg); }
    static Status Corruption(const Slice& msg = Slice()) { return Status(kCorruption, msg); }
    static Status NotSupported(const Slice& msg = Slice()) { return Status(kNotSupported, msg); }
    static Status InvalidArgument(const Slice& msg = Slice()) { return Status(kInvalidArgument, msg); }
    static Status IOError(const Slice& msg = Slice()) { return Status(kIOError, msg); }
    static Status Busy(const Slice& msg = Slice()) { return Status(kBusy, msg); }
    static Status Aborted(const Slice& msg = Slice()) { return Status(kAborted, msg); }

    bool ok() const { return code_ == kOk; }
    bool IsNotFound() const { return code_ == kNotFound; }
    bool IsCorruption() const { return code_ == kCorruption; }
    bool IsNotSupported() const { return code_ == kNotSupported; }
    bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
    bool IsIOError() const { return code_ == kIOError; }
    bool IsBusy() const { return code_ == kBusy; }
    bool IsAborted() const { return code_ == kAborted; }

    Code code() const { return code_; }
    std::string message() const { return state_; }

    std::string ToString() const {
        if (code_ == kOk) return "OK";
        std::string result;
        switch (code_) {
            case kNotFound: result = "NotFound: "; break;
            case kCorruption: result = "Corruption: "; break;
            case kNotSupported: result = "Not implemented: "; break;
            case kInvalidArgument: result = "Invalid argument: "; break;
            case kIOError: result = "IO error: "; break;
            case kBusy: result = "Busy: "; break;
            case kAborted: result = "Aborted: "; break;
            default: result = "Unknown code (" + std::to_string(static_cast<int>(code_)) + "): "; break;
        }
        result += state_;
        return result;
    }

private:
    Code code_{kOk};
    std::string state_{};
};

inline std::ostream& operator<<(std::ostream& os, const Status& s) {
    os << s.ToString();
    return os;
}

} // namespace lsm
