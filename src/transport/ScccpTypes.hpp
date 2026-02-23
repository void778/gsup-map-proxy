#pragma once
#include <cstdint>

// SCCP constants (ITU-T Q.713).
//
// Only Connectionless (UDT/XUDT) is needed for MAP over SIGTRAN.

namespace proxy::transport::sccp {

// ── Message types ─────────────────────────────────────────────────────────────
constexpr uint8_t kMsgUdt  = 0x09; // Unitdata
constexpr uint8_t kMsgUdts = 0x0a; // Unitdata Service (error return)
constexpr uint8_t kMsgXudt = 0x11; // Extended Unitdata

// ── Protocol class (octet 2 of UDT) ─────────────────────────────────────────
constexpr uint8_t kProtoClass0      = 0x00; // class 0, no special options
constexpr uint8_t kProtoClass0Ret   = 0x08; // class 0, return on error

// ── Address Indicator octet ───────────────────────────────────────────────────
// bit 7:   Routing Indicator  0=route by GT, 1=route by SSN/PC
// bits 6-4: Global Title Indicator
// bit 3:   Subsystem Number Indicator (SSN present)
// bit 2-0: Point Code Indicator (not standard — use 0 when routing by GT)

constexpr uint8_t kAiRoutByGt  = 0x00; // route by GT
constexpr uint8_t kAiRoutBySsn = 0x80; // route by DPC+SSN

// Global Title Indicator values (bits 6-4)
constexpr uint8_t kGtiNoGt     = 0x00; // no GT
constexpr uint8_t kGtiNature   = 0x10; // format 1 — Nature of Address only
constexpr uint8_t kGtiTtOnly   = 0x20; // format 2 — Translation Type only
constexpr uint8_t kGtiTtNpEs   = 0x30; // format 3 — TT + NP + ES
constexpr uint8_t kGtiTtNpEsNai= 0x40; // format 4 — TT + NP + ES + NAI (most common for MAP)

// SSN indicator bit
constexpr uint8_t kAiSsnPresent = 0x08;

// ── Well-known SSNs ───────────────────────────────────────────────────────────
constexpr uint8_t kSsnHlr   = 0x06; // HLR
constexpr uint8_t kSsnVlr   = 0x07; // VLR
constexpr uint8_t kSsnSgsn  = 0x08; // SGSN (GPRS)
constexpr uint8_t kSsnGgsn  = 0x09; // GGSN

// ── Global Title format 4 (most common) ──────────────────────────────────────
// Encoding of GT format 4 (GTI=0x04):
//   Translation Type (1 octet)
//   Numbering Plan | Encoding Scheme (1 octet)
//   Nature of Address Indicator (1 octet, bit7=odd/even flag)
//   BCD digits (odd digit packed per two-digit standard)
//
// Numbering Plan (high nibble of NP/ES byte)
constexpr uint8_t kNpIsdnTelephony = 0x10; // E.164

// Encoding Scheme (low nibble of NP/ES byte)
constexpr uint8_t kEsBcdOdd  = 0x01;
constexpr uint8_t kEsBcdEven = 0x02;

// Nature of Address
constexpr uint8_t kNaiInternational = 0x04;

// Translation Type (for MAP)
constexpr uint8_t kTtInternational = 0x00;

} // namespace proxy::transport::sccp
