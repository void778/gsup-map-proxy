#pragma once
#include "ITransport.hpp"
#include "TransactionManager.hpp"
#include "Converter.hpp"
#include "gsup/GsupCodec.hpp"
#include "map/MapCodec.hpp"
#include <memory>
#include <functional>
#include <unordered_map>
#include <chrono>

namespace proxy {

// Tracks an in-flight HLR-initiated MAP Invoke so that the SGSN's GSUP
// result can be matched back to the original MAP transaction.
struct HlrTxEntry {
    uint32_t          mapTransactionId;
    uint8_t           mapInvokeId;
    map::MapOperation mapOperation;
    std::chrono::steady_clock::time_point createdAt;
};

// Proxy ties together the SGSN-facing and HLR-facing transports.
//
// Transport contract (ITransport semantics for IPA transport):
//   send(data)     → data is a GSUP payload; transport adds IPA framing.
//   onMessage(cb)  → cb receives a GSUP payload; transport strips IPA framing.
//
// The Proxy therefore deals only in decoded GSUP/MAP messages and never
// touches IPA wire framing directly.  This makes it fully unit-testable
// with simple mock transports.

class Proxy {
public:
    explicit Proxy(std::shared_ptr<ITransport> sgsnTransport,
                   std::shared_ptr<ITransport> hlrTransport,
                   std::chrono::seconds txTimeout = std::chrono::seconds{30});

    // Wire up callbacks and start processing.
    void start();

    // Exposed for testing: feed a decoded GSUP payload directly.
    void handleGsupPayload(const Bytes& gsupPayload, uint64_t clientContext = 0);

    // Exposed for testing: feed a decoded MAP TCAP payload directly.
    void handleMapPayload(const Bytes& mapPayload);

    TransactionManager& transactions() { return txMgr_; }

    // Remove stale HLR-initiated transactions (no SGSN response within timeout).
    void expireHlrTransactions(std::chrono::seconds timeout = std::chrono::seconds{30});

    size_t hlrTxSize() const { return hlrTx_.size(); }

    // Send ReturnError GSUP responses for all pending SGSN-initiated transactions
    // and clear them.  Called when the HLR transport disconnects.
    void nackAllPendingTransactions();

    // Expire stale SGSN-initiated transactions and send error responses.
    void expireSgsnTransactions();

private:
    // HLR-initiated direction: MAP Invoke → GSUP Request → SGSN.
    void handleHlrInitiated(const map::MapMessage& mapMsg);

    // HLR-initiated response: GSUP Result from SGSN → MAP ReturnResult → HLR.
    void handleGsupHlrResponse(const gsup::GsupMessage& gsupMsg);

    std::shared_ptr<ITransport> sgsnTransport_;
    std::shared_ptr<ITransport> hlrTransport_;
    TransactionManager          txMgr_;

    // HLR-initiated transactions keyed by IMSI (GSUP results carry no MAP TID).
    std::unordered_map<std::string, HlrTxEntry> hlrTx_;

    // Stores the MAP operation for each pending SGSN-initiated transaction so
    // that ReturnError responses (which carry no operation code on the wire)
    // can still be converted back to the correct GSUP error type.
    std::unordered_map<uint32_t, map::MapOperation> pendingOps_;
};

} // namespace proxy
