#pragma once
#include "MapMessage.hpp"
#include "common/Buffer.hpp"

namespace proxy::map {

// ── Encoder ──────────────────────────────────────────────────────────────────
// Produces a complete TCAP PDU (Begin for requests, End for responses)
// carrying the MAP operation inside an Invoke or ReturnResult component.
Bytes encode(const MapMessage& msg);

// ── Decoder ──────────────────────────────────────────────────────────────────
// Parses a TCAP PDU and extracts the MAP message.
// Throws std::runtime_error on malformed input.
MapMessage decode(const Bytes& data);
MapMessage decode(const uint8_t* data, size_t len);

} // namespace proxy::map
