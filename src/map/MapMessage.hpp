#pragma once
#include "MapTypes.hpp"
#include "common/Buffer.hpp"
#include <string>
#include <vector>
#include <optional>

namespace proxy::map {

// ── Auth vectors ─────────────────────────────────────────────────────────────
struct MapAuthTriplet {
    Bytes rand;   // 16 bytes
    Bytes sres;   //  4 bytes
    Bytes kc;     //  8 bytes
};

struct MapAuthQuintuplet {
    Bytes rand;   // 16 bytes
    Bytes xres;   // variable
    Bytes ck;     // 16 bytes
    Bytes ik;     // 16 bytes
    Bytes autn;   // 16 bytes
};

// ── TCAP component type ──────────────────────────────────────────────────────
enum class ComponentType { Invoke, ReturnResult, ReturnError, Reject };

// ── Unified MAP message ──────────────────────────────────────────────────────
// One struct covers both request (Invoke) and response (ReturnResult/ReturnError).
// Encode/decode only populate fields relevant to the specific operation.
struct MapMessage {
    // TCAP envelope
    uint32_t      transactionId{0};
    ComponentType component{ComponentType::Invoke};
    uint8_t       invokeId{1};
    bool          isLastComponent{true}; // End vs Continue

    // MAP operation
    MapOperation  operation{MapOperation::Unknown};

    // ── Common field ────────────────────────────────────────────────────────
    std::string   imsi;

    // ── SendAuthenticationInfo Request ──────────────────────────────────────
    std::optional<uint8_t> numRequestedVectors;

    // ── SendAuthenticationInfo Response ─────────────────────────────────────
    std::vector<MapAuthTriplet>    authTriplets;
    std::vector<MapAuthQuintuplet> authQuintuplets;

    // ── UpdateGprsLocation Request ──────────────────────────────────────────
    std::optional<Bytes> sgsnNumber;  // ISDN-AddressString
    std::optional<Bytes> sgsnAddress; // GSN-Address (IP)

    // ── UpdateGprsLocation / InsertSubscriberData Response ──────────────────
    std::optional<Bytes> hlrNumber;
    std::optional<Bytes> msisdn;

    // ── CancelLocation Request ───────────────────────────────────────────────
    std::optional<uint8_t> cancelType;

    // ── PurgeMS Request ──────────────────────────────────────────────────────
    // (sgsnNumber reused)

    // ── PurgeMS Response ────────────────────────────────────────────────────
    bool freezePtmsi{false};

    // ── Error (ReturnError) ──────────────────────────────────────────────────
    std::optional<uint8_t> errorCode;
};

} // namespace proxy::map
