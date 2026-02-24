#pragma once
#include "IpaFrame.hpp"
#include <optional>

namespace proxy::ipa {

// Maximum accepted IPA wire-length field value.  The wire-length includes the
// 1-byte stream ID, so the maximum payload is kMaxIpaWireLen-1 bytes.
// Frames claiming a larger size are rejected to prevent buffer exhaustion.
static constexpr uint16_t kMaxIpaWireLen = 4096u;

// Encode a single IPA frame to wire format.
Bytes encode(uint8_t streamId, const Bytes& payload);

// Stateful framing decoder: feed raw bytes received from TCP; call next()
// repeatedly until it returns std::nullopt (need more data).
class IpaDecoder {
public:
    // Append new raw bytes (e.g., from a recv() call).
    void feed(const uint8_t* data, size_t len);
    void feed(const Bytes& data) { feed(data.data(), data.size()); }

    // Returns the next complete frame, or nullopt if more data is needed.
    std::optional<IpaFrame> next();

    bool hasData() const { return !buf_.empty(); }

private:
    Bytes buf_;
};

} // namespace proxy::ipa
