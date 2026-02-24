#include "Converter.hpp"
#include <spdlog/spdlog.h>

namespace proxy {

using namespace gsup;
using namespace map;

// ── GSUP → MAP ───────────────────────────────────────────────────────────────

MapMessage gsupToMap(const GsupMessage& gsup, uint32_t transactionId, uint8_t invokeId) {
    spdlog::debug("[converter] gsupToMap: type={} IMSI={} TID={:#010x}", static_cast<int>(gsup.type), gsup.imsi, transactionId);
    MapMessage m;
    m.transactionId = transactionId;
    m.invokeId      = invokeId;
    m.component     = ComponentType::Invoke;
    m.imsi          = gsup.imsi;

    switch (gsup.type) {
        // ── SendAuthInfo ──────────────────────────────────────────────────────
        case MessageType::SendAuthInfoRequest:
            m.operation             = MapOperation::SendAuthenticationInfo;
            m.numRequestedVectors   = gsup.numVectorsRequested.value_or(1);
            break;

        // ── UpdateGprsLocation ────────────────────────────────────────────────
        case MessageType::UpdateLocationRequest:
            m.operation = MapOperation::UpdateGprsLocation;
            // SGSN number / address are not carried in basic GSUP UpdateLocation;
            // in a real deployment they come from the IPA connection metadata.
            break;

        // ── CancelLocation ────────────────────────────────────────────────────
        case MessageType::LocationCancelRequest:
            m.operation  = MapOperation::CancelLocation;
            if (gsup.cancelType)
                m.cancelType = static_cast<uint8_t>(*gsup.cancelType);
            break;

        // ── InsertSubscriberData ──────────────────────────────────────────────
        case MessageType::InsertDataRequest:
            m.operation = MapOperation::InsertSubscriberData;
            m.msisdn    = gsup.msisdn;
            break;

        // ── PurgeMS ───────────────────────────────────────────────────────────
        case MessageType::PurgeMsRequest:
            m.operation = MapOperation::PurgeMS;
            break;

        default:
            throw ConversionError("gsupToMap: unsupported GSUP message type");
    }
    return m;
}

// ── MAP → GSUP ───────────────────────────────────────────────────────────────

GsupMessage mapToGsup(const MapMessage& map) {
    spdlog::debug("[converter] mapToGsup: op={} component={} IMSI={} TID={:#010x}", static_cast<int>(map.operation), static_cast<int>(map.component), map.imsi, map.transactionId);
    GsupMessage g;
    g.imsi = map.imsi;

    // ReturnError → GSUP Error message
    if (map.component == ComponentType::ReturnError) {
        switch (map.operation) {
            case MapOperation::SendAuthenticationInfo:
                g.type  = MessageType::SendAuthInfoError;
                break;
            case MapOperation::UpdateGprsLocation:
                g.type  = MessageType::UpdateLocationError;
                break;
            case MapOperation::CancelLocation:
                g.type  = MessageType::LocationCancelError;
                break;
            case MapOperation::InsertSubscriberData:
                g.type  = MessageType::InsertDataError;
                break;
            case MapOperation::PurgeMS:
                g.type  = MessageType::PurgeMsError;
                break;
            default:
                throw ConversionError("mapToGsup: unknown operation in ReturnError");
        }
        // MAP error code → GSUP cause (best-effort mapping)
        g.cause = map.errorCode;
        return g;
    }

    // ReturnResult → GSUP Result message
    switch (map.operation) {
        // ── SendAuthInfo ──────────────────────────────────────────────────────
        case MapOperation::SendAuthenticationInfo:
            g.type = MessageType::SendAuthInfoResult;
            for (const auto& t : map.authTriplets) {
                AuthTuple at;
                at.rand = t.rand;
                at.sres = t.sres;
                at.kc   = t.kc;
                g.authTuples.push_back(std::move(at));
            }
            for (const auto& q : map.authQuintuplets) {
                AuthTuple at;
                at.rand = q.rand;
                at.sres = {};     // not in quintuplets — use XRES
                at.kc   = {};
                at.ik   = q.ik;
                at.ck   = q.ck;
                at.autn = q.autn;
                at.res  = q.xres;
                g.authTuples.push_back(std::move(at));
            }
            break;

        // ── UpdateGprsLocation ────────────────────────────────────────────────
        case MapOperation::UpdateGprsLocation:
            g.type      = MessageType::UpdateLocationResult;
            g.hlrNumber = map.hlrNumber;
            break;

        // ── CancelLocation ────────────────────────────────────────────────────
        case MapOperation::CancelLocation:
            g.type = MessageType::LocationCancelResult;
            break;

        // ── InsertSubscriberData ──────────────────────────────────────────────
        case MapOperation::InsertSubscriberData:
            g.type = MessageType::InsertDataResult;
            break;

        // ── PurgeMS ───────────────────────────────────────────────────────────
        case MapOperation::PurgeMS:
            g.type        = MessageType::PurgeMsResult;
            g.freezePtmsi = map.freezePtmsi;
            break;

        default:
            throw ConversionError("mapToGsup: unsupported MAP operation");
    }
    return g;
}

// ── MAP Invoke → GSUP Request (HLR-initiated) ────────────────────────────────

GsupMessage mapInvokeToGsup(const MapMessage& map) {
    spdlog::debug("[converter] mapInvokeToGsup: op={} IMSI={} TID={:#010x}", static_cast<int>(map.operation), map.imsi, map.transactionId);
    if (map.component != ComponentType::Invoke)
        throw ConversionError("mapInvokeToGsup: expected Invoke component");

    GsupMessage g;
    g.imsi = map.imsi;

    switch (map.operation) {
        // ── InsertSubscriberData ──────────────────────────────────────────────
        case MapOperation::InsertSubscriberData:
            g.type   = MessageType::InsertDataRequest;
            g.msisdn = map.msisdn;
            break;

        // ── CancelLocation ────────────────────────────────────────────────────
        case MapOperation::CancelLocation:
            g.type = MessageType::LocationCancelRequest;
            if (map.cancelType)
                g.cancelType = static_cast<CancelType>(*map.cancelType);
            break;

        // ── DeleteSubscriberData ──────────────────────────────────────────────
        case MapOperation::DeleteSubscriberData:
            g.type = MessageType::DeleteDataRequest;
            break;

        // ── PurgeMS ───────────────────────────────────────────────────────────
        case MapOperation::PurgeMS:
            g.type = MessageType::PurgeMsRequest;
            break;

        default:
            throw ConversionError("mapInvokeToGsup: unsupported MAP operation");
    }
    return g;
}

// ── GSUP Result → MAP ReturnResult (HLR-initiated response) ──────────────────

MapMessage gsupToMapResult(const GsupMessage& gsup, uint32_t transactionId, uint8_t invokeId) {
    spdlog::debug("[converter] gsupToMapResult: type={} IMSI={} TID={:#010x}", static_cast<int>(gsup.type), gsup.imsi, transactionId);
    MapMessage m;
    m.transactionId   = transactionId;
    m.invokeId        = invokeId;
    m.imsi            = gsup.imsi;
    m.isLastComponent = true;

    // Determine result vs. error from the GSUP type
    const bool isError =
        gsup.type == MessageType::InsertDataError ||
        gsup.type == MessageType::LocationCancelError ||
        gsup.type == MessageType::DeleteDataError ||
        gsup.type == MessageType::PurgeMsError;

    m.component = isError ? ComponentType::ReturnError : ComponentType::ReturnResult;
    if (isError) m.errorCode = gsup.cause;

    switch (gsup.type) {
        case MessageType::InsertDataResult:
        case MessageType::InsertDataError:
            m.operation = MapOperation::InsertSubscriberData;
            break;

        case MessageType::LocationCancelResult:
        case MessageType::LocationCancelError:
            m.operation = MapOperation::CancelLocation;
            break;

        case MessageType::DeleteDataResult:
        case MessageType::DeleteDataError:
            m.operation = MapOperation::DeleteSubscriberData;
            break;

        case MessageType::PurgeMsResult:
        case MessageType::PurgeMsError:
            m.operation = MapOperation::PurgeMS;
            break;

        default:
            throw ConversionError("gsupToMapResult: not an HLR-initiated result type");
    }
    return m;
}

// ── isHlrInitiatedGsupType ────────────────────────────────────────────────────

bool isHlrInitiatedGsupType(MessageType type) {
    switch (type) {
        case MessageType::InsertDataResult:
        case MessageType::InsertDataError:
        case MessageType::LocationCancelResult:
        case MessageType::LocationCancelError:
        case MessageType::DeleteDataResult:
        case MessageType::DeleteDataError:
            return true;
        case MessageType::PurgeMsResult:
        case MessageType::PurgeMsError:
            return true;
        default:
            return false;
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

gsup::MessageType expectedGsupResult(gsup::MessageType req) {
    switch (req) {
        case MessageType::SendAuthInfoRequest:   return MessageType::SendAuthInfoResult;
        case MessageType::UpdateLocationRequest: return MessageType::UpdateLocationResult;
        case MessageType::LocationCancelRequest: return MessageType::LocationCancelResult;
        case MessageType::InsertDataRequest:     return MessageType::InsertDataResult;
        case MessageType::PurgeMsRequest:        return MessageType::PurgeMsResult;
        default: throw ConversionError("expectedGsupResult: not a request type");
    }
}

gsup::MessageType expectedGsupError(gsup::MessageType req) {
    switch (req) {
        case MessageType::SendAuthInfoRequest:   return MessageType::SendAuthInfoError;
        case MessageType::UpdateLocationRequest: return MessageType::UpdateLocationError;
        case MessageType::LocationCancelRequest: return MessageType::LocationCancelError;
        case MessageType::InsertDataRequest:     return MessageType::InsertDataError;
        case MessageType::PurgeMsRequest:        return MessageType::PurgeMsError;
        default: throw ConversionError("expectedGsupError: not a request type");
    }
}

} // namespace proxy
