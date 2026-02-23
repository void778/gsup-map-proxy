#include "TransactionManager.hpp"

namespace proxy {

TransactionManager::TransactionManager(std::chrono::seconds timeout)
    : timeout_(timeout) {}

uint32_t TransactionManager::nextTid() {
    // Wrap around, skip 0
    if (tidCounter_ == 0) tidCounter_ = 1;
    return tidCounter_++;
}

uint8_t TransactionManager::nextInvokeId() {
    if (invokeIdCounter_ > 127) invokeIdCounter_ = 1;
    return invokeIdCounter_++;
}

uint32_t TransactionManager::allocate(const std::string& imsi, uint64_t clientContext) {
    uint32_t tid = nextTid();
    PendingTransaction t;
    t.mapTransactionId = tid;
    t.mapInvokeId      = nextInvokeId();
    t.imsi             = imsi;
    t.clientContext    = clientContext;
    t.createdAt        = std::chrono::steady_clock::now();
    transactions_.emplace(tid, std::move(t));
    return tid;
}

std::optional<PendingTransaction> TransactionManager::find(uint32_t mapTransactionId) const {
    auto it = transactions_.find(mapTransactionId);
    if (it == transactions_.end()) return std::nullopt;
    return it->second;
}

void TransactionManager::complete(uint32_t mapTransactionId) {
    transactions_.erase(mapTransactionId);
}

std::vector<uint64_t> TransactionManager::expireStale() {
    auto now = std::chrono::steady_clock::now();
    std::vector<uint64_t> expired;
    for (auto it = transactions_.begin(); it != transactions_.end(); ) {
        if (now - it->second.createdAt > timeout_) {
            expired.push_back(it->second.clientContext);
            it = transactions_.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

} // namespace proxy
