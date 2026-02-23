#pragma once
#include <cstddef>
#include <cstdint>

// M3UA constants per RFC 4666.
//
// Wire format:
//   Version(1) | Reserved(1) | MsgClass(1) | MsgType(1) | Length(4 BE)
//   followed by TLVs, each padded to 4-byte boundary.
//
// TLV format: Tag(2 BE) | Length(2 BE, includes Tag+Length) | Value | Padding

namespace proxy::transport::m3ua {

// ── Protocol version ─────────────────────────────────────────────────────────
constexpr uint8_t kVersion = 0x01;

// ── Message classes ───────────────────────────────────────────────────────────
constexpr uint8_t kClassMgmt   = 0x00; // Management (NTFY, ERR)
constexpr uint8_t kClassTransf = 0x01; // Transfer (DATA)
constexpr uint8_t kClassSsnm   = 0x02; // SS7 Signalling Network Management
constexpr uint8_t kClassAspsm  = 0x03; // ASP State Maintenance
constexpr uint8_t kClassAsptm  = 0x04; // ASP Traffic Maintenance

// ── Message types ─────────────────────────────────────────────────────────────

// MGMT
constexpr uint8_t kTypeErr  = 0x00;
constexpr uint8_t kTypeNtfy = 0x01;

// Transfer
constexpr uint8_t kTypeData = 0x01;

// ASPSM
constexpr uint8_t kTypeAspUp      = 0x01;
constexpr uint8_t kTypeAspUpAck   = 0x02;
constexpr uint8_t kTypeAspDown    = 0x03;
constexpr uint8_t kTypeAspDownAck = 0x04;
constexpr uint8_t kTypeHeartbeat  = 0x05;
constexpr uint8_t kTypeHeartbeatAck = 0x06;

// ASPTM
constexpr uint8_t kTypeAspAc    = 0x01;
constexpr uint8_t kTypeAspAcAck = 0x02;
constexpr uint8_t kTypeAspIa    = 0x03;
constexpr uint8_t kTypeAspIaAck = 0x04;

// ── Common TLV tags ───────────────────────────────────────────────────────────
constexpr uint16_t kTagInfoString    = 0x0004;
constexpr uint16_t kTagRoutingCtx   = 0x0006;
constexpr uint16_t kTagDiagInfo     = 0x0007;
constexpr uint16_t kTagHeartbeatData = 0x0009;
constexpr uint16_t kTagTrafficMode  = 0x000b;
constexpr uint16_t kTagErrorCode    = 0x000c;
constexpr uint16_t kTagStatus       = 0x000d;
constexpr uint16_t kTagAspId        = 0x0011;
constexpr uint16_t kTagAffectedPcInd = 0x0012;
constexpr uint16_t kTagProtocolData = 0x0210; // DATA payload

// ── Protocol Data TLV internals ───────────────────────────────────────────────
// The Protocol Data TLV carries: OPC(4) DPC(4) SI(1) NI(1) MP(1) SLS(1) UserData(N)
//
// SI (Service Indicator): 3 = SCCP
constexpr uint8_t kSiSccp = 0x03;
// NI (Network Indicator): 0 = international, 2 = national
constexpr uint8_t kNiInternational = 0x00;
constexpr uint8_t kNiNational      = 0x02;

// ── Common header size ────────────────────────────────────────────────────────
constexpr std::size_t kCommonHeaderSize = 8;

// ── Traffic mode ──────────────────────────────────────────────────────────────
constexpr uint32_t kTrafficModeOverride   = 1;
constexpr uint32_t kTrafficModeLoadShare  = 2;
constexpr uint32_t kTrafficModeBroadcast  = 3;

} // namespace proxy::transport::m3ua
