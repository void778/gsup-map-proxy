#include "proxy/Proxy.hpp"
#include <spdlog/spdlog.h>

namespace proxy {

Proxy::Proxy(std::shared_ptr<ITransport> sgsnTransport,
             std::shared_ptr<ITransport> hlrTransport,
             std::chrono::seconds txTimeout)
    : sgsnTransport_(std::move(sgsnTransport))
    , hlrTransport_(std::move(hlrTransport))
    , txMgr_(txTimeout)
{}

void Proxy::start() {
    // The transport delivers GSUP payloads (IPA framing is the transport's job).
    sgsnTransport_->onMessage([this](const Bytes& gsupPayload, ClientId id) {
        handleGsupPayload(gsupPayload, id);
    });
    // The HLR transport delivers raw MAP TCAP PDUs.
    hlrTransport_->onMessage([this](const Bytes& mapPayload, ClientId /*id*/) {
        handleMapPayload(mapPayload);
    });
    // When the HLR transport disconnects, NAK all pending SGSN transactions.
    hlrTransport_->onDisconnect([this]() {
        nackAllPendingTransactions();
    });
}

void Proxy::handleGsupPayload(const Bytes& gsupPayload, uint64_t clientContext) {
    gsup::GsupMessage gsupMsg;
    try {
        gsupMsg = gsup::decode(gsupPayload);
    } catch (const std::exception& e) {
        spdlog::error("[proxy] GSUP decode error: {}", e.what());
        return;
    }

    // HLR-initiated responses (SGSN replying to InsertSubscriberData or CancelLocation)
    if (isHlrInitiatedGsupType(gsupMsg.type)) {
        handleGsupHlrResponse(gsupMsg);
        return;
    }

    // SGSN-initiated requests
    uint32_t tid    = txMgr_.allocate(gsupMsg.imsi, clientContext);
    uint8_t  invoke = txMgr_.find(tid)->mapInvokeId;

    map::MapMessage mapMsg;
    try {
        mapMsg = gsupToMap(gsupMsg, tid, invoke);
    } catch (const ConversionError& e) {
        spdlog::warn("[proxy] Conversion error (GSUP->MAP): {}", e.what());
        txMgr_.complete(tid);
        return;
    }

    // Remember the MAP operation so ReturnError responses (which carry no
    // operation code on the wire) can still be routed to the right GSUP type.
    pendingOps_[tid] = mapMsg.operation;

    Bytes mapData;
    try {
        mapData = map::encode(mapMsg);
    } catch (const std::exception& e) {
        spdlog::error("[proxy] MAP encode error: {}", e.what());
        txMgr_.complete(tid);
        pendingOps_.erase(tid);
        return;
    }

    hlrTransport_->send(mapData);
}

void Proxy::handleMapPayload(const Bytes& mapPayload) {
    map::MapMessage mapMsg;
    try {
        mapMsg = map::decode(mapPayload);
    } catch (const std::exception& e) {
        spdlog::error("[proxy] MAP decode error: {}", e.what());
        return;
    }

    // HLR-initiated: MAP Invoke from the HLR toward the SGSN
    if (mapMsg.component == map::ComponentType::Invoke) {
        handleHlrInitiated(mapMsg);
        return;
    }

    // SGSN-initiated: MAP ReturnResult/ReturnError from the HLR
    auto pending = txMgr_.find(mapMsg.transactionId);
    if (!pending) {
        spdlog::warn("[proxy] No pending transaction for TID {}", mapMsg.transactionId);
        return;
    }

    if (mapMsg.imsi.empty()) mapMsg.imsi = pending->imsi;

    // ReturnError PDUs carry no operation code on the wire; recover from context.
    if (mapMsg.operation == map::MapOperation::Unknown) {
        auto it = pendingOps_.find(mapMsg.transactionId);
        if (it != pendingOps_.end())
            mapMsg.operation = it->second;
    }

    gsup::GsupMessage gsupMsg;
    try {
        gsupMsg = mapToGsup(mapMsg);
    } catch (const ConversionError& e) {
        spdlog::warn("[proxy] Conversion error (MAP->GSUP): {}", e.what());
        txMgr_.complete(mapMsg.transactionId);
        pendingOps_.erase(mapMsg.transactionId);
        return;
    }

    ClientId clientId = pending->clientContext;
    pendingOps_.erase(mapMsg.transactionId);
    txMgr_.complete(mapMsg.transactionId);

    Bytes gsupData;
    try {
        gsupData = gsup::encode(gsupMsg);
    } catch (const std::exception& e) {
        spdlog::error("[proxy] GSUP encode error: {}", e.what());
        return;
    }

    // Route GSUP response back to the originating SGSN session.
    sgsnTransport_->send(gsupData, clientId);
}

// ── HLR-initiated direction ───────────────────────────────────────────────────

void Proxy::handleHlrInitiated(const map::MapMessage& mapMsg) {
    gsup::GsupMessage gsupMsg;
    try {
        gsupMsg = mapInvokeToGsup(mapMsg);
    } catch (const ConversionError& e) {
        spdlog::warn("[proxy] Conversion error (MAP Invoke->GSUP): {}", e.what());
        return;
    }

    // Track the HLR transaction by IMSI so the SGSN response can be matched.
    HlrTxEntry entry;
    entry.mapTransactionId = mapMsg.transactionId;
    entry.mapInvokeId      = mapMsg.invokeId;
    entry.mapOperation     = mapMsg.operation;
    entry.createdAt        = std::chrono::steady_clock::now();
    hlrTx_[gsupMsg.imsi]  = entry;

    Bytes gsupData;
    try {
        gsupData = gsup::encode(gsupMsg);
    } catch (const std::exception& e) {
        spdlog::error("[proxy] GSUP encode error (HLR-initiated): {}", e.what());
        hlrTx_.erase(gsupMsg.imsi);
        return;
    }

    sgsnTransport_->send(gsupData);
}

void Proxy::handleGsupHlrResponse(const gsup::GsupMessage& gsupMsg) {
    auto it = hlrTx_.find(gsupMsg.imsi);
    if (it == hlrTx_.end()) {
        spdlog::warn("[proxy] No HLR-initiated transaction for IMSI {}", gsupMsg.imsi);
        return;
    }

    const HlrTxEntry& entry = it->second;
    map::MapMessage mapMsg;
    try {
        mapMsg = gsupToMapResult(gsupMsg, entry.mapTransactionId, entry.mapInvokeId);
    } catch (const ConversionError& e) {
        spdlog::warn("[proxy] Conversion error (GSUP->MAP result): {}", e.what());
        hlrTx_.erase(it);
        return;
    }

    hlrTx_.erase(it);

    Bytes mapData;
    try {
        mapData = map::encode(mapMsg);
    } catch (const std::exception& e) {
        spdlog::error("[proxy] MAP encode error (HLR-initiated response): {}", e.what());
        return;
    }

    hlrTransport_->send(mapData);
}

void Proxy::expireHlrTransactions(std::chrono::seconds timeout) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = hlrTx_.begin(); it != hlrTx_.end(); ) {
        if (now - it->second.createdAt > timeout) {
            spdlog::warn("[proxy] HLR-initiated transaction expired for IMSI {}", it->first);
            it = hlrTx_.erase(it);
        } else {
            ++it;
        }
    }
}

void Proxy::nackAllPendingTransactions() {
    for (auto& [tid, op] : pendingOps_) {
        auto pending = txMgr_.find(tid);
        if (!pending) continue;

        map::MapMessage errMsg;
        errMsg.transactionId = tid;
        errMsg.operation     = op;
        errMsg.component     = map::ComponentType::ReturnError;
        errMsg.imsi          = pending->imsi;

        try {
            auto gsupMsg  = mapToGsup(errMsg);
            auto gsupData = gsup::encode(gsupMsg);
            sgsnTransport_->send(gsupData, pending->clientContext);
        } catch (const std::exception&) {}

        txMgr_.complete(tid);
    }
    pendingOps_.clear();
}

void Proxy::expireSgsnTransactions() {
    auto expired = txMgr_.expireStale();
    for (auto& tx : expired) {
        auto it = pendingOps_.find(tx.mapTransactionId);
        if (it == pendingOps_.end()) continue;

        map::MapMessage errMsg;
        errMsg.transactionId = tx.mapTransactionId;
        errMsg.operation     = it->second;
        errMsg.component     = map::ComponentType::ReturnError;
        errMsg.imsi          = tx.imsi;

        try {
            auto gsupMsg  = mapToGsup(errMsg);
            auto gsupData = gsup::encode(gsupMsg);
            sgsnTransport_->send(gsupData, tx.clientContext);
        } catch (const std::exception&) {}

        pendingOps_.erase(it);
    }
}

} // namespace proxy
