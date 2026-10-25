#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include "slice.hpp"

namespace lsm {

static constexpr uint8_t kTTLMagic = 0x54; // 'T'

class TTLUtil {
public:
    static uint64_t CurrentTimeSeconds() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
    }

    static std::string EncodeWithTTL(const Slice& value, uint64_t ttl_seconds) {
        uint64_t expiry = CurrentTimeSeconds() + ttl_seconds;
        std::string result;
        result.reserve(value.size() + 9);
        result.append(value.data(), value.size());
        // 8 bytes expiry
        for (int i = 0; i < 8; ++i) {
            result.push_back(static_cast<char>((expiry >> ((7 - i) * 8)) & 0xFF));
        }
        result.push_back(static_cast<char>(kTTLMagic));
        return result;
    }

    static bool DecodeTTL(const Slice& raw_value, Slice* user_value, uint64_t* expiry_timestamp) {
        if (raw_value.size() >= 9 && static_cast<uint8_t>(raw_value[raw_value.size() - 1]) == kTTLMagic) {
            size_t val_len = raw_value.size() - 9;
            if (user_value) {
                *user_value = Slice(raw_value.data(), val_len);
            }
            if (expiry_timestamp) {
                uint64_t exp = 0;
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(raw_value.data() + val_len);
                for (int i = 0; i < 8; ++i) {
                    exp = (exp << 8) | static_cast<uint64_t>(ptr[i]);
                }
                *expiry_timestamp = exp;
            }
            return true;
        }
        // No TTL metadata attached
        if (user_value) {
            *user_value = raw_value;
        }
        if (expiry_timestamp) {
            *expiry_timestamp = 0;
        }
        return false;
    }

    static bool IsExpired(const Slice& raw_value) {
        Slice user_val;
        uint64_t expiry = 0;
        if (DecodeTTL(raw_value, &user_val, &expiry)) {
            if (expiry > 0 && CurrentTimeSeconds() >= expiry) {
                return true;
            }
        }
        return false;
    }
};

} // namespace lsm
