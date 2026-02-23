#include "transport/M3uaCodec.hpp"
#include "common/Buffer.hpp"
#include <stdexcept>
#include <cstring>

namespace proxy::transport::m3ua {

// ── TLV helpers ───────────────────────────────────────────────────────────────

static void writeTlv(BufferWriter& w, uint16_t tag, const Bytes& value) {
    uint16_t len = static_cast<uint16_t>(4 + value.size()); // tag(2)+len(2)+value
    w.writeBE16(tag);
    w.writeBE16(len);
    w.writeBytes(value);
    // Pad to 4-byte boundary
    std::size_t pad = (4 - (value.size() % 4)) % 4;
    for (std::size_t i = 0; i < pad; ++i) w.write8(0);
}

static void writeTlvU32(BufferWriter& w, uint16_t tag, uint32_t v) {
    Bytes val(4);
    val[0] = (v >> 24) & 0xFF;
    val[1] = (v >> 16) & 0xFF;
    val[2] = (v >>  8) & 0xFF;
    val[3] = (v      ) & 0xFF;
    writeTlv(w, tag, val);
}

// ── Encode ────────────────────────────────────────────────────────────────────

Bytes encode(const M3uaMessage& msg) {
    BufferWriter body;

    // Routing Context TLV (optional)
    if (msg.routingContext.has_value()) {
        writeTlvU32(body, kTagRoutingCtx, *msg.routingContext);
    }

    // Heartbeat TLV
    if (!msg.heartbeatData.empty()) {
        writeTlv(body, kTagHeartbeatData, msg.heartbeatData);
    }

    // Protocol Data TLV (DATA messages)
    if (msg.protocolData.has_value()) {
        const auto& pd = *msg.protocolData;
        BufferWriter pdw;
        pdw.writeBE32(pd.opc);
        pdw.writeBE32(pd.dpc);
        pdw.write8(pd.si);
        pdw.write8(pd.ni);
        pdw.write8(pd.mp);
        pdw.write8(pd.sls);
        pdw.writeBytes(pd.userData);
        writeTlv(body, kTagProtocolData, pdw.bytes());
    }

    const Bytes& bodyBytes = body.bytes();
    uint32_t totalLen = static_cast<uint32_t>(kCommonHeaderSize + bodyBytes.size());

    BufferWriter w;
    w.write8(kVersion);
    w.write8(0x00); // reserved
    w.write8(msg.msgClass);
    w.write8(msg.msgType);
    w.writeBE32(totalLen);
    w.writeBytes(bodyBytes);
    return w.bytes();
}

// ── Decode ────────────────────────────────────────────────────────────────────

std::optional<M3uaMessage> decode(const Bytes& data) {
    if (data.size() < kCommonHeaderSize) return std::nullopt;

    BufferReader r(data);
    uint8_t version  = r.read8();
    r.read8();                        // reserved
    uint8_t msgClass = r.read8();
    uint8_t msgType  = r.read8();
    uint32_t totalLen = r.readBE32();

    if (version != kVersion)
        throw std::runtime_error("M3UA: unexpected version");
    if (totalLen < kCommonHeaderSize)
        throw std::runtime_error("M3UA: length < header size");
    if (data.size() < totalLen)
        return std::nullopt; // incomplete

    M3uaMessage msg;
    msg.msgClass = msgClass;
    msg.msgType  = msgType;

    // Parse TLVs in body
    std::size_t bodyEnd = totalLen; // bytes from start of `data`
    while (r.pos() + 4 <= bodyEnd) {
        uint16_t tag = r.readBE16();
        uint16_t len = r.readBE16(); // includes tag(2)+len(2)
        if (len < 4)
            throw std::runtime_error("M3UA TLV: length < 4");
        std::size_t valueLen = len - 4;
        if (r.pos() + valueLen > bodyEnd)
            throw std::runtime_error("M3UA TLV: value extends past message");

        Bytes value = r.readBytes(valueLen);

        // Skip padding to next 4-byte boundary
        std::size_t pad = (4 - (valueLen % 4)) % 4;
        if (pad > 0 && r.pos() + pad <= bodyEnd)
            r.readBytes(pad); // consume alignment padding

        switch (tag) {
            case kTagRoutingCtx:
                if (value.size() >= 4) {
                    msg.routingContext =
                        (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) |
                        (uint32_t(value[2]) <<  8) |  uint32_t(value[3]);
                }
                break;

            case kTagHeartbeatData:
                msg.heartbeatData = value;
                break;

            case kTagProtocolData: {
                if (value.size() < 12)
                    throw std::runtime_error("M3UA ProtocolData TLV too short");
                BufferReader pr(value);
                ProtocolData pd;
                pd.opc = pr.readBE32();
                pd.dpc = pr.readBE32();
                pd.si  = pr.read8();
                pd.ni  = pr.read8();
                pd.mp  = pr.read8();
                pd.sls = pr.read8();
                pd.userData = pr.readBytes(pr.remaining());
                msg.protocolData = std::move(pd);
                break;
            }

            default:
                break; // ignore unknown TLVs
        }
    }

    return msg;
}

// ── Decoder ───────────────────────────────────────────────────────────────────

void M3uaDecoder::feed(const uint8_t* data, std::size_t n) {
    buf_.insert(buf_.end(), data, data + n);
}

std::optional<M3uaMessage> M3uaDecoder::next() {
    if (buf_.size() < kCommonHeaderSize) return std::nullopt;

    uint32_t totalLen =
        (uint32_t(buf_[4]) << 24) | (uint32_t(buf_[5]) << 16) |
        (uint32_t(buf_[6]) <<  8) |  uint32_t(buf_[7]);

    if (buf_.size() < totalLen) return std::nullopt;

    Bytes msg(buf_.begin(), buf_.begin() + totalLen);
    buf_.erase(buf_.begin(), buf_.begin() + totalLen);
    return decode(msg);
}

} // namespace proxy::transport::m3ua
