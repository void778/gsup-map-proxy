#pragma once
#include "ipa/IpaFrame.hpp"
#include "ipa/IpaCodec.hpp"
#include "common/Buffer.hpp"

#include <boost/asio.hpp>
#include <array>
#include <deque>
#include <functional>
#include <memory>

namespace asio = boost::asio;

namespace proxy::transport {

// Manages a single TCP connection to an IPA peer (osmoSGSN).
//
// Lifecycle:
//   ┌──────────────────────────────────────────────────────────────────────┐
//   │  TCP accept → start() → sends ID_GET → receives ID_RESP             │
//   │            → sends ID_ACK → State::Ready → GSUP traffic flows       │
//   │            → PING received anytime → PONG sent                      │
//   └──────────────────────────────────────────────────────────────────────┘
//
// Thread safety:
//   sendGsup() is safe to call from any thread; it posts to the session's
//   executor.  All other methods must be called on the io_context thread.

class IpaSession : public std::enable_shared_from_this<IpaSession> {
public:
    // Callbacks supplied by IpaServer
    using GsupHandler       = std::function<void(Bytes)>;         // GSUP payload rx
    using ReadyHandler      = std::function<void()>;              // CCM handshake done
    using DisconnectHandler = std::function<void()>;              // TCP connection lost

    explicit IpaSession(asio::ip::tcp::socket socket);
    ~IpaSession();

    // Kick off the CCM handshake and start the read loop.
    void start(ReadyHandler    onReady,
               GsupHandler     onGsup,
               DisconnectHandler onDisconnect);

    // Gracefully shut down the session.
    void stop();

    // Send a GSUP payload.  Thread-safe.
    void sendGsup(const Bytes& gsupPayload);

    bool isReady() const { return state_ == State::Ready; }

private:
    enum class State { Handshaking, Ready, Stopped };

    // Read loop
    void startRead();
    void processFrame(const ipa::IpaFrame& frame);
    void handleCcm(const Bytes& payload);

    // Write queue (must be called on io_context thread)
    void enqueue(Bytes ipaFrame);
    void doWrite();

    // CCM helpers
    void sendCcmIdGet();
    void sendCcmIdAck();
    void sendCcmPong();

    asio::ip::tcp::socket     socket_;
    std::array<uint8_t, 4096> readBuf_;
    ipa::IpaDecoder           decoder_;

    ReadyHandler      onReady_;
    GsupHandler       onGsup_;
    DisconnectHandler onDisconnect_;

    State state_{State::Handshaking};

    std::deque<Bytes> writeQueue_;
    bool              writing_{false};
};

} // namespace proxy::transport
