#pragma once
#include "common/Buffer.hpp"
#include <cstdint>
#include <stdexcept>

// Basic BER (Basic Encoding Rules) encoding and decoding utilities.
// Handles the subset of BER needed for TCAP/MAP.
// Only definite-length encoding is produced; indefinite-length is NOT supported.

namespace proxy::map::ber {

// ── Length encoding / decoding ────────────────────────────────────────────

// Write BER length to buffer (short or long form).
void writeLength(BufferWriter& w, size_t len);

// Read BER length from reader; advances reader past the length bytes.
size_t readLength(BufferReader& r);

// ── TLV helpers ───────────────────────────────────────────────────────────

// Encode a complete TLV: tag (1 byte) + BER length + value.
void writeTlv(BufferWriter& w, uint8_t tag, const Bytes& value);

// Convenience overloads
void writeTlvInt(BufferWriter& w, uint8_t tag, int64_t value);
void writeTlvOctet(BufferWriter& w, uint8_t tag, uint8_t value);

// Decode one TLV from reader. Tag is returned; value is written to out.
// Advances reader past the complete TLV.
uint8_t readTlv(BufferReader& r, Bytes& out);

// ── Constructed TLV helper ────────────────────────────────────────────────
// For encoding a constructed (nested) TLV, write the inner content into
// a temporary BufferWriter, then use this to wrap it.
void wrapConstructed(BufferWriter& w, uint8_t tag, const Bytes& inner);

// ── Integer encode/decode ────────────────────────────────────────────────
Bytes encodeInt(int64_t value);
int64_t decodeInt(const Bytes& value);

} // namespace proxy::map::ber
