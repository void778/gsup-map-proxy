#pragma once
#include <cstdint>

// GSUP protocol constants (3GPP TS 29.002 / Osmocom GSUP spec)
// https://osmocom.org/projects/cellular-infrastructure/wiki/GSUP

namespace proxy::gsup {

// ── Message types ──────────────────────────────────────────────────────────
// Encoding: bits [7:2] = operation, bits [1:0] = direction (00=Req,01=Err,10=Res)
enum class MessageType : uint8_t {
    // Location / Attach
    UpdateLocationRequest  = 0x04,
    UpdateLocationError    = 0x05,
    UpdateLocationResult   = 0x06,

    // Authentication
    SendAuthInfoRequest    = 0x08,
    SendAuthInfoError      = 0x09,
    SendAuthInfoResult     = 0x0a,

    // Purge
    PurgeMsRequest         = 0x0c,
    PurgeMsError           = 0x0d,
    PurgeMsResult          = 0x0e,

    // Insert Subscriber Data (HLR → SGSN)
    InsertDataRequest      = 0x10,
    InsertDataError        = 0x11,
    InsertDataResult       = 0x12,

    // Delete Subscriber Data (HLR → SGSN)
    DeleteDataRequest      = 0x14,
    DeleteDataError        = 0x15,
    DeleteDataResult       = 0x16,

    // Cancel Location (HLR → SGSN)
    LocationCancelRequest  = 0x1c,
    LocationCancelError    = 0x1d,
    LocationCancelResult   = 0x1e,

    Unknown                = 0xFF,
};

// ── Information Element tags ────────────────────────────────────────────────
enum class IeTag : uint8_t {
    Imsi               = 0x01,
    Cause              = 0x02,
    AuthTuple          = 0x03,  // constructed / nested
    PdpInfoComplete    = 0x04,
    PdpInfo            = 0x05,  // constructed / nested
    CancelType         = 0x06,
    FreezePtmsi        = 0x07,
    Msisdn             = 0x08,
    HlrNumber          = 0x09,
    Pc                 = 0x0a,
    ImeiResult         = 0x0f,
    NumVectorsReq      = 0x10,
    // Auth sub-IEs (used inside AuthTuple)
    Rand               = 0x20,
    Sres               = 0x21,
    Kc                 = 0x22,
    Ik                 = 0x23,
    Ck                 = 0x24,
    Autn               = 0x25,
    Auts               = 0x26,
    Res                = 0x27,
    CnDomain           = 0x28,
    // PDP sub-IEs (used inside PdpInfo)
    PdpContextId       = 0x60,
    PdpType            = 0x61,
    AccessPointName    = 0x62,
    PdpAddress         = 0x63,
    PdpQos             = 0x64,
};

// ── Cancel type values ──────────────────────────────────────────────────────
enum class CancelType : uint8_t {
    UpdateProcedure  = 0,
    Withdraw         = 1,
};

// ── CN Domain values ────────────────────────────────────────────────────────
enum class CnDomain : uint8_t {
    Cs = 0,
    Ps = 1,
};

} // namespace proxy::gsup
