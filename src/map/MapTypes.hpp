#pragma once
#include <cstdint>

// MAP and TCAP protocol constants (3GPP TS 29.002 / ITU-T Q.773)

namespace proxy::map {

// ── MAP Operation codes (local) ─────────────────────────────────────────────
enum class MapOperation : uint8_t {
    UpdateLocation         = 2,
    CancelLocation         = 3,
    InsertSubscriberData   = 7,
    DeleteSubscriberData   = 8,
    SendAuthenticationInfo = 56,
    UpdateGprsLocation     = 23,
    SendRoutingInfoForGprs = 24,
    PurgeMS                = 67,

    Unknown                = 0xFF,
};

// ── TCAP PDU Application-class tags ────────────────────────────────────────
// ITU-T Q.773 §3.1 — class APPLICATION, CONSTRUCTED
constexpr uint8_t kTcapTagUnidirectional = 0x61; // [APPLICATION 1]
constexpr uint8_t kTcapTagBegin          = 0x62; // [APPLICATION 2]
constexpr uint8_t kTcapTagEnd            = 0x64; // [APPLICATION 4]
constexpr uint8_t kTcapTagContinue      = 0x65; // [APPLICATION 5]
constexpr uint8_t kTcapTagAbort         = 0x67; // [APPLICATION 7]

// TCAP field tags (APPLICATION)
constexpr uint8_t kTcapTagOtid           = 0x48; // [APPLICATION 8] PRIM  — Orig TID
constexpr uint8_t kTcapTagDtid           = 0x49; // [APPLICATION 9] PRIM  — Dest TID
constexpr uint8_t kTcapTagDialogue       = 0x6b; // [APPLICATION 11] CONS
constexpr uint8_t kTcapTagComponents     = 0x6c; // [APPLICATION 12] CONS

// TCAP Component tags (CONTEXT, CONSTRUCTED) — ITU-T Q.773 §3.4
constexpr uint8_t kCompTagInvoke         = 0xa1; // [1] CONS
constexpr uint8_t kCompTagReturnResult   = 0xa2; // [2] CONS
constexpr uint8_t kCompTagReturnError    = 0xa3; // [3] CONS
constexpr uint8_t kCompTagReject         = 0xa4; // [4] CONS

// Universal BER tags
constexpr uint8_t kBerInt                = 0x02; // INTEGER
constexpr uint8_t kBerOctetStr          = 0x04; // OCTET STRING
constexpr uint8_t kBerNull              = 0x05;
constexpr uint8_t kBerOid              = 0x06; // OBJECT IDENTIFIER
constexpr uint8_t kBerSequence         = 0x30; // SEQUENCE (CONS)
constexpr uint8_t kBerSet              = 0x31; // SET (CONS)

} // namespace proxy::map
