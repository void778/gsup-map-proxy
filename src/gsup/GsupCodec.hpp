#pragma once
#include "GsupMessage.hpp"
#include "common/Buffer.hpp"
#include <string>
#include <stdexcept>

namespace proxy::gsup {

// ── BCD helpers ─────────────────────────────────────────────────────────────
// Encode/decode IMSI between ASCII digit string and packed BCD.
// BCD format: first digit in low nibble of first byte; filler 0xF if odd length.
Bytes imsiToBcd(const std::string& imsi);
std::string bcdToImsi(const Bytes& bcd);

// Same for ISDN address strings (MSISDN / HLR number) — prefixed with TON/NPI byte.
Bytes isdnToBcd(const std::string& digits);
std::string bcdToIsdn(const Bytes& bcd);

// ── Decoder ─────────────────────────────────────────────────────────────────
// Throws std::runtime_error on malformed input.
GsupMessage decode(const Bytes& payload);
GsupMessage decode(const uint8_t* data, size_t len);

// ── Encoder ─────────────────────────────────────────────────────────────────
Bytes encode(const GsupMessage& msg);

} // namespace proxy::gsup
