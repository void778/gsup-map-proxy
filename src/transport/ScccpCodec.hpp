#pragma once
#include "transport/ScccpTypes.hpp"
#include "common/Buffer.hpp"
#include <string>

namespace proxy::transport::sccp {

// ── Address structure ─────────────────────────────────────────────────────────

struct SccpAddress {
    // When routeByGt == true: encode GT-format-4 address
    // When routeByGt == false: encode PC+SSN (not GT)
    bool        routeByGt = true;

    // Global Title format 4 fields (used when routeByGt == true)
    std::string digits;        // E.164 digits (e.g. "+49161...")
    uint8_t     tt  = kTtInternational;
    uint8_t     np  = kNpIsdnTelephony;
    uint8_t     nai = kNaiInternational;

    // SSN — present when routeByGt==false (or optionally alongside GT)
    bool    ssnPresent = false;
    uint8_t ssn        = 0;

    // Point code — used when routeByGt==false
    bool     pcPresent = false;
    uint32_t pc        = 0;
};

// ── UDT message structure ─────────────────────────────────────────────────────

struct SccpUdt {
    uint8_t     protocolClass = kProtoClass0;
    SccpAddress calledParty;   // destination
    SccpAddress callingParty;  // source
    Bytes       data;          // MAP TCAP PDU
};

// ── Codec ─────────────────────────────────────────────────────────────────────

// Encode an SCCP UDT message.
Bytes encodeUdt(const SccpUdt& udt);

// Decode an SCCP UDT message.
// Throws std::runtime_error on malformed input.
SccpUdt decodeUdt(const Bytes& raw);

// ── Address helpers ───────────────────────────────────────────────────────────

// Build a GT-format-4 address from E.164 digits (international, no SSN).
SccpAddress makeGtAddress(const std::string& digits,
                          uint8_t ssn = 0, bool hasSsn = false);

// Build an SSN-only address (no GT, route by PC+SSN).
SccpAddress makeSsnAddress(uint32_t pc, uint8_t ssn);

} // namespace proxy::transport::sccp
