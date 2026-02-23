#pragma once
#include "proxy/ITransport.hpp"
#include "transport/M3uaCodec.hpp"
#include "transport/ScccpCodec.hpp"

#include <boost/asio.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <chrono>
#include <deque>
#include <memory>
#include <string>

namespace asio = boost::asio;
namespace bsys = boost::system;

namespace proxy::transport {

// ── Configuration ─────────────────────────────────────────────────────────────

struct MapTransportConfig {
    std::string sgHost;           // SS7 Signalling Gateway host
    uint16_t    sgPort    = 2905; // M3UA port (IANA assigned)
    uint32_t    opc       = 0;    // Originating Point Code (this proxy)
    uint32_t    dpc       = 0;    // Destination Point Code (HLR)
    std::optional<uint32_t> routingContext; // M3UA routing context

    // SCCP addressing
    std::string hlrGt;            // HLR Global Title (E.164, e.g. "+49161...")
    uint8_t     hlrSsn = sccp::kSsnHlr;
    std::string localGt;          // Proxy local GT
    uint8_t     localSsn = sccp::kSsnSgsn;

    // Timers
    std::chrono::seconds reconnectInterval{5};
    std::chrono::seconds beatInterval{30};

    // Transport protocol
    // When false (default), a standard TCP socket is used.
    // When true, SCTP is used instead (requires ENABLE_SCTP_TRANSPORT at build time).
    bool useSCTP = false;
};

// ── MapTransport ──────────────────────────────────────────────────────────────
//
// Implements ITransport for the HLR-facing side.
//
// Lifecycle:
//   send(payload)   — wraps MAP TCAP PDU in SCCP UDT → M3UA DATA and sends.
//   onMessage(cb)   — delivers MAP TCAP PDU (SCCP/M3UA stripped) to Proxy.
//
// Connection state machine:
//   DISCONNECTED → TCP connect → ASPSM_UP (send ASPUP, wait ASPUP_ACK)
//                → ASPTM_ACTIVE (send ASPAC, wait ASPAC_ACK) → ACTIVE
//
// Reconnection:
//   On disconnect or connect failure, schedules a reconnect after
//   `reconnectInterval` and transitions back to DISCONNECTED.
//
// Thread safety:
//   start() / stop() must be called before io_context::run().
//   send() is safe to call from any thread (posts to io_context).

class MapTransport : public ITransport {
public:
    MapTransport(asio::io_context& ioc, MapTransportConfig cfg);
    ~MapTransport() override;

    void start();
    void stop();

    // ITransport (clientId is ignored — single connection to SG)
    void send(const Bytes& mapPayload, ClientId clientId = 0) override;
    void onMessage(MessageCallback cb) override { messageCb_ = std::move(cb); }

private:
    enum class State { Disconnected, Connecting, AspUp, Active, Stopped };

    void scheduleConnect();
    void connect();
    void onConnect(bsys::error_code ec);
    void resetConnection(); // clear write queue + decoder on disconnect

    void sendAspUp();
    void sendAspAc();
    void startBeat();
    void onBeat();

    void startRead();
    void processMessage(const m3ua::M3uaMessage& msg);

    void sendRaw(m3ua::M3uaMessage msg);
    void enqueue(Bytes frame);
    void doWrite();

    asio::io_context&                     ioc_;
    MapTransportConfig                    cfg_;
    State                                 state_{State::Disconnected};
    asio::generic::stream_protocol::socket socket_;
    asio::steady_timer    reconnectTimer_;
    asio::steady_timer    beatTimer_;

    m3ua::M3uaDecoder     decoder_;
    std::array<uint8_t, 4096> readBuf_;

    std::deque<Bytes> writeQueue_;
    bool              writing_{false};

    MessageCallback   messageCb_;
};

} // namespace proxy::transport
