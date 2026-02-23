#pragma once
#include "transport/IpaSession.hpp"
#include "proxy/ITransport.hpp"

#include <boost/asio.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace asio = boost::asio;

namespace proxy::transport {

// TCP server that listens for osmoSGSN connections over IPA.
//
// Implements ITransport with these semantics:
//   send(payload, 0)    → broadcast GSUP frame to ALL active sessions.
//   send(payload, id)   → send GSUP frame to the specific session `id`.
//   onMessage(cb)       → cb invoked with GSUP payload and originating ClientId.
//
// Multiple SGSN connections are supported simultaneously; each receives a
// unique ClientId at connect time.  IPA CCM handshake is transparent.
//
// Thread safety:
//   send() is safe to call from any thread.
//   start() / stop() must be called before io_context::run().

class IpaServer : public ITransport {
public:
    explicit IpaServer(asio::io_context& ioc, uint16_t port = 4222);

    void start();
    void stop();

    uint16_t port() const { return acceptor_.local_endpoint().port(); }

    // True if at least one session has completed the CCM handshake.
    bool hasActiveSession() const { return !sessions_.empty(); }

    // Number of active (post-handshake) sessions.
    std::size_t sessionCount() const { return sessions_.size(); }

    // ── ITransport ────────────────────────────────────────────────────────
    // clientId == 0: broadcast to all active sessions.
    void send(const Bytes& gsupPayload, ClientId clientId = 0) override;

    void onMessage(MessageCallback cb) override { messageCb_ = std::move(cb); }

private:
    void acceptNext();
    void onSessionReady(ClientId id);
    void onSessionDisconnect(ClientId id);

    asio::io_context&       ioc_;
    asio::ip::tcp::acceptor acceptor_;
    MessageCallback         messageCb_;

    // Active (post-handshake) sessions.
    std::unordered_map<ClientId, std::shared_ptr<IpaSession>> sessions_;
    // All sessions including those still in handshake (for cleanup on stop).
    std::unordered_map<ClientId, std::shared_ptr<IpaSession>> allSessions_;
    ClientId nextClientId_{1};
};

} // namespace proxy::transport
