#include "BerCodec.hpp"
#include <algorithm>
#include <stdexcept>

namespace proxy::map::ber {

// ── Length ───────────────────────────────────────────────────────────────────

void writeLength(BufferWriter& w, size_t len) {
    if (len <= 0x7F) {
        w.write8(static_cast<uint8_t>(len));
    } else if (len <= 0xFF) {
        w.write8(0x81);
        w.write8(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        w.write8(0x82);
        w.write8(static_cast<uint8_t>((len >> 8) & 0xFF));
        w.write8(static_cast<uint8_t>(len & 0xFF));
    } else {
        throw std::runtime_error("BER: length too large for 2-byte form");
    }
}

size_t readLength(BufferReader& r) {
    uint8_t first = r.read8();
    if (first <= 0x7F)
        return first;
    if (first == 0x80)
        throw std::runtime_error("BER: indefinite-length form not supported");
    uint8_t numBytes = first & 0x7F;
    if (numBytes > 4)
        throw std::runtime_error("BER: length field too large");
    size_t len = 0;
    for (uint8_t i = 0; i < numBytes; ++i)
        len = (len << 8) | r.read8();
    return len;
}

// ── TLV ──────────────────────────────────────────────────────────────────────

void writeTlv(BufferWriter& w, uint8_t tag, const Bytes& value) {
    w.write8(tag);
    writeLength(w, value.size());
    w.writeBytes(value);
}

void writeTlvInt(BufferWriter& w, uint8_t tag, int64_t value) {
    writeTlv(w, tag, encodeInt(value));
}

void writeTlvOctet(BufferWriter& w, uint8_t tag, uint8_t value) {
    w.write8(tag);
    w.write8(1);
    w.write8(value);
}

uint8_t readTlv(BufferReader& r, Bytes& out) {
    uint8_t tag = r.read8();
    size_t len  = readLength(r);
    out = r.readBytes(len);
    return tag;
}

void wrapConstructed(BufferWriter& w, uint8_t tag, const Bytes& inner) {
    w.write8(tag);
    writeLength(w, inner.size());
    w.writeBytes(inner);
}

// ── Integer ───────────────────────────────────────────────────────────────────

Bytes encodeInt(int64_t value) {
    // Minimal two's-complement encoding.
    if (value == 0) return {0x00};

    Bytes be;
    bool neg = value < 0;
    uint64_t uv = neg ? static_cast<uint64_t>(-(value + 1)) : static_cast<uint64_t>(value);

    // Extract bytes big-endian
    while (uv > 0) {
        be.push_back(static_cast<uint8_t>(uv & 0xFF));
        uv >>= 8;
    }
    std::reverse(be.begin(), be.end());

    // If negative, flip bits (one's complement), then add 1 is already handled
    // by the -(value+1) above — but we need to flip:
    if (neg) {
        for (auto& b : be) b = ~b;
        // Now be represents -(value+1) XOR 0xFF..., which is the two's complement
        // of -value ... let me redo this properly.
        // Actually just encode directly:
        be.clear();
        int64_t tmp = value;
        do {
            be.push_back(static_cast<uint8_t>(tmp & 0xFF));
            tmp >>= 8;
        } while (tmp != -1 || (be.back() & 0x80) == 0);
        std::reverse(be.begin(), be.end());
        return be;
    }

    // Positive: ensure high bit is 0 (add leading zero if needed)
    if (be[0] & 0x80)
        be.insert(be.begin(), 0x00);
    return be;
}

int64_t decodeInt(const Bytes& value) {
    if (value.empty())
        throw std::runtime_error("BER: empty INTEGER value");
    int64_t result = 0;
    bool neg = (value[0] & 0x80) != 0;
    for (uint8_t b : value) {
        result = (result << 8) | b;
    }
    if (neg) {
        // Sign-extend
        size_t bits = value.size() * 8;
        if (bits < 64) {
            int64_t mask = -(INT64_C(1) << bits);
            result |= mask;
        }
    }
    return result;
}

} // namespace proxy::map::ber
