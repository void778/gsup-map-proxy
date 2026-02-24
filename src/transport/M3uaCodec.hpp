#pragma once
#include "transport/M3uaTypes.hpp"
#include "common/Buffer.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace proxy::transport::m3ua {

// ── Message structures ────────────────────────────────────────────────────────

struct CommonHeader {
    uint8_t  msgClass;
    uint8_t  msgType;
    uint32_t length;  // total including header
};

// Protocol Data TLV payload (inside DATA message)
struct ProtocolData {
    uint32_t opc = 0;
    uint32_t dpc = 0;
    uint8_t  si  = kSiSccp;
    uint8_t  ni  = kNiInternational;
    uint8_t  mp  = 0;
    uint8_t  sls = 0;
    Bytes    userData; // SCCP PDU
};

struct M3uaMessage {
    uint8_t  msgClass;
    uint8_t  msgType;

    // Optional routing context (for ASPAC / DATA)
    std::optional<uint32_t> routingContext;

    // Present in DATA messages
    std::optional<ProtocolData> protocolData;

    // Present in heartbeat / heartbeat ack
    Bytes heartbeatData;
};

// ── Codec ─────────────────────────────────────────────────────────────────────

// Encode a complete M3UA message (header + TLVs) into a byte vector.
Bytes encode(const M3uaMessage& msg);

// Decode one M3UA message from the front of `data`.
// Returns nullopt if data is incomplete; throws std::runtime_error on malformed.
std::optional<M3uaMessage> decode(const Bytes& data);

// ── Convenience builders ──────────────────────────────────────────────────────

// ASP Up  (ASPSM 0x01)
inline M3uaMessage makeAspUp() {
    return {kClassAspsm, kTypeAspUp, std::nullopt, std::nullopt, {}};
}

// ASP Active  (ASPTM 0x01) — optionally with routing context
inline M3uaMessage makeAspAc(std::optional<uint32_t> rc = std::nullopt) {
    return {kClassAsptm, kTypeAspAc, rc, std::nullopt, {}};
}

// Heartbeat (ASPSM 0x05) — echo data back in ack
inline M3uaMessage makeHeartbeat(Bytes data = {}) {
    return {kClassAspsm, kTypeHeartbeat, std::nullopt, std::nullopt, std::move(data)};
}

inline M3uaMessage makeHeartbeatAck(Bytes data = {}) {
    return {kClassAspsm, kTypeHeartbeatAck, std::nullopt, std::nullopt, std::move(data)};
}

// DATA carrying a SCCP PDU
inline M3uaMessage makeData(ProtocolData pd, std::optional<uint32_t> rc = std::nullopt) {
    return {kClassTransf, kTypeData, rc, std::move(pd), {}};
}

// Maximum accepted M3UA message size (64 KiB). Messages claiming to be
// larger are almost certainly malformed/malicious; the decoder resets its
// buffer and throws std::runtime_error.
static constexpr std::size_t kMaxM3uaMessageSize = 64u * 1024u;

// Stateful stream reassembler (same pattern as IpaDecoder)
class M3uaDecoder {
public:
    void feed(const uint8_t* data, std::size_t n);
    std::optional<M3uaMessage> next();

private:
    Bytes buf_;
};

} // namespace proxy::transport::m3ua
