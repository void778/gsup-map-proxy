#pragma once
#include "common/Buffer.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <stdexcept>

namespace proxy {

// Tracks in-flight MAP transactions so that HLR responses can be matched
// back to the original GSUP request (and its originating SGSN connection).

struct PendingTransaction {
    uint32_t    mapTransactionId;  // TCAP TID used toward HLR
    uint8_t     mapInvokeId;       // TCAP component invoke ID
    std::string imsi;              // for correlation / logging
    // Opaque context the caller stores to route the response back.
    // In production this would carry the SGSN connection handle / session ID.
    uint64_t    clientContext;
    std::chrono::steady_clock::time_point createdAt;
};

class TransactionManager {
public:
    explicit TransactionManager(std::chrono::seconds timeout = std::chrono::seconds{30});

    // Allocate a new MAP transaction ID and register the pending transaction.
    // Returns the allocated TCAP transaction ID.
    uint32_t allocate(const std::string& imsi, uint64_t clientContext);

    // Look up a transaction by MAP TID. Returns nullopt if not found.
    std::optional<PendingTransaction> find(uint32_t mapTransactionId) const;

    // Remove a transaction after receiving the response.
    void complete(uint32_t mapTransactionId);

    // Expire stale transactions; returns the full PendingTransaction records.
    std::vector<PendingTransaction> expireStale();

    size_t size() const { return transactions_.size(); }

    // Invoke ID counter (per-proxy, simple monotonic counter mod 127).
    uint8_t nextInvokeId();

private:
    uint32_t nextTid();

    uint32_t tidCounter_{1};
    uint8_t  invokeIdCounter_{1};
    std::chrono::seconds timeout_;

    std::unordered_map<uint32_t, PendingTransaction> transactions_;
};

} // namespace proxy
