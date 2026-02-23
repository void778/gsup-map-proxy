#include "IpaCodec.hpp"
#include <stdexcept>

namespace proxy::ipa {

Bytes encode(uint8_t streamId, const Bytes& payload) {
    if (payload.size() > 0xFFFF)
        throw std::invalid_argument("IPA payload too large");

    // length field covers payload + stream byte
    uint16_t wireLen = static_cast<uint16_t>(payload.size() + 1);

    Bytes out;
    out.reserve(kHeaderSize + payload.size());
    out.push_back((wireLen >> 8) & 0xFF);
    out.push_back(wireLen & 0xFF);
    out.push_back(streamId);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void IpaDecoder::feed(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

std::optional<IpaFrame> IpaDecoder::next() {
    // Need at least the 3-byte header.
    if (buf_.size() < kHeaderSize)
        return std::nullopt;

    uint16_t wireLen = (static_cast<uint16_t>(buf_[0]) << 8) | buf_[1];
    if (wireLen == 0)
        throw std::runtime_error("IPA: zero-length wire frame");

    // wireLen includes the stream byte, so payload = wireLen - 1
    size_t totalSize = kHeaderSize + static_cast<size_t>(wireLen) - 1;
    if (buf_.size() < totalSize)
        return std::nullopt; // wait for more data

    IpaFrame frame;
    frame.streamId = buf_[2];
    frame.payload.assign(buf_.begin() + kHeaderSize,
                         buf_.begin() + static_cast<ptrdiff_t>(totalSize));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(totalSize));
    return frame;
}

} // namespace proxy::ipa
