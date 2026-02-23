#pragma once
#include "GsupTypes.hpp"
#include "common/Buffer.hpp"
#include <string>
#include <vector>
#include <optional>

namespace proxy::gsup {

// GSM auth tuple (2G: RAND/SRES/Kc; 3G additionally has IK/CK/AUTN/RES)
struct AuthTuple {
    Bytes rand;   // 16 bytes
    Bytes sres;   //  4 bytes
    Bytes kc;     //  8 bytes

    // UMTS extensions (populated when HSS returns quintuplets)
    std::optional<Bytes> ik;
    std::optional<Bytes> ck;
    std::optional<Bytes> autn;   // 16 bytes
    std::optional<Bytes> res;    // 4–16 bytes (variable in UMTS)
};

// PDP context information (for subscriber data)
struct PdpInfo {
    uint8_t                 contextId{0};
    std::optional<uint16_t> pdpType;      // e.g. 0x0121 = IPv4
    std::optional<std::string> apn;
    std::optional<Bytes>    pdpAddress;
    std::optional<Bytes>    qos;
};

// Unified GSUP message — fields that don't apply to a given MessageType
// are simply left as std::nullopt / empty.
struct GsupMessage {
    MessageType type{MessageType::Unknown};

    // Present in virtually every message
    std::string imsi;

    // Error / cause code
    std::optional<uint8_t> cause;

    // SEND_AUTH_INFO_REQUEST
    std::optional<uint8_t> numVectorsRequested;

    // SEND_AUTH_INFO_RESULT
    std::vector<AuthTuple> authTuples;

    // UPDATE_LOCATION / INSERT_DATA
    std::optional<Bytes> msisdn;           // encoded ISDN address
    std::optional<Bytes> hlrNumber;        // encoded ISDN address
    std::optional<CnDomain> cnDomain;

    // INSERT_DATA / DELETE_DATA
    std::vector<PdpInfo> pdpInfoList;
    bool pdpInfoComplete{false};

    // LOCATION_CANCEL
    std::optional<CancelType> cancelType;
    bool freezePtmsi{false};

    // PURGE_MS_RESULT
    bool freezePtmsiResult{false};
};

} // namespace proxy::gsup
