#pragma once
#include "gsup/GsupMessage.hpp"
#include "map/MapMessage.hpp"
#include <stdexcept>

namespace proxy {

// Thrown when a message type has no supported conversion.
struct ConversionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ── GSUP → MAP ───────────────────────────────────────────────────────────────
// Converts an inbound GSUP request (from SGSN) into a MAP request to send
// to the HLR.  The caller supplies the TCAP transactionId and invokeId that
// were allocated by TransactionManager.
map::MapMessage gsupToMap(const gsup::GsupMessage& gsup,
                          uint32_t transactionId,
                          uint8_t  invokeId);

// ── MAP → GSUP ───────────────────────────────────────────────────────────────
// Converts an inbound MAP response (from HLR) into a GSUP response to send
// back to the SGSN.
gsup::GsupMessage mapToGsup(const map::MapMessage& map);

// ── MAP Invoke → GSUP Request (HLR-initiated direction) ──────────────────────
// Converts a MAP Invoke from the HLR (InsertSubscriberData, CancelLocation)
// into a GSUP Request to be forwarded to the SGSN.
gsup::GsupMessage mapInvokeToGsup(const map::MapMessage& map);

// ── GSUP Result → MAP ReturnResult (HLR-initiated response) ──────────────────
// Converts a GSUP result/error from the SGSN back into a MAP ReturnResult or
// ReturnError to be sent to the HLR.  The caller supplies the TCAP transactionId
// and invokeId from the original MAP Invoke.
map::MapMessage gsupToMapResult(const gsup::GsupMessage& gsup,
                                uint32_t transactionId,
                                uint8_t  invokeId);

// ── Helper: true if this GSUP type is an HLR-initiated result/error ──────────
bool isHlrInitiatedGsupType(gsup::MessageType type);

// ── Helper: derive the expected GSUP result/error type from a GSUP request ──
gsup::MessageType expectedGsupResult(gsup::MessageType requestType);
gsup::MessageType expectedGsupError(gsup::MessageType requestType);

} // namespace proxy
