#include "transport/MapTransport.hpp"
#include <iostream>
#ifdef ENABLE_SCTP_TRANSPORT
#  include <netinet/sctp.h>
#endif

namespace proxy::transport {

using namespace m3ua;
using namespace sccp;

MapTransport::MapTransport(asio::io_context& ioc, MapTransportConfig cfg)
    : ioc_(ioc)
    , cfg_(std::move(cfg))
    , socket_(ioc)
    , reconnectTimer_(ioc)
    , beatTimer_(ioc)
{}

MapTransport::~MapTransport() {
    // Timer and socket cleaned up by ASIO on destruction.
}

void MapTransport::start() {
    scheduleConnect();
}

void MapTransport::stop() {
    state_ = State::Stopped;
    reconnectTimer_.cancel();
    beatTimer_.cancel();
    bsys::error_code ec;
    socket_.close(ec);
}

// ── Connection management ─────────────────────────────────────────────────────

void MapTransport::scheduleConnect() {
    if (state_ == State::Stopped) return;

    reconnectTimer_.expires_after(
        state_ == State::Disconnected
            ? std::chrono::seconds(0)         // first attempt: immediate
            : cfg_.reconnectInterval);

    reconnectTimer_.async_wait([this](bsys::error_code ec) {
        if (!ec && state_ != State::Stopped)
            connect();
    });
}

void MapTransport::connect() {
    state_ = State::Connecting;

    // Resolve the host synchronously so the endpoint outlives the async op.
    bsys::error_code ec;
    asio::ip::tcp::resolver resolver(ioc_);
    auto results = resolver.resolve(cfg_.sgHost, std::to_string(cfg_.sgPort), ec);
    if (ec || results.empty()) {
        if (!ec) ec = asio::error::host_not_found;
        asio::post(ioc_, [this, ec]{ onConnect(ec); });
        return;
    }
    const auto tcpEp = results.begin()->endpoint();

    // Ensure the socket is closed before (re)opening with a new protocol.
    if (socket_.is_open()) {
        bsys::error_code closeEc;
        socket_.close(closeEc);
    }

    // Open with TCP or SCTP according to configuration.
#ifdef ENABLE_SCTP_TRANSPORT
    if (cfg_.useSCTP) {
        const int family = tcpEp.address().is_v6() ? AF_INET6 : AF_INET;
        socket_.open(asio::generic::stream_protocol(family, IPPROTO_SCTP), ec);
        if (ec) {
            std::cerr << "[MapTransport] SCTP open failed: " << ec.message() << "\n";
            asio::post(ioc_, [this, ec]{ onConnect(ec); });
            return;
        }
    } else {
        socket_.open(asio::ip::tcp::v4(), ec);
        if (ec) { asio::post(ioc_, [this, ec]{ onConnect(ec); }); return; }
    }
#else
    if (cfg_.useSCTP) {
        std::cerr << "[MapTransport] SCTP requested but ENABLE_SCTP_TRANSPORT "
                     "was not set at build time — falling back to TCP\n";
    }
    socket_.open(asio::ip::tcp::v4(), ec);
    if (ec) { asio::post(ioc_, [this, ec]{ onConnect(ec); }); return; }
#endif

    // Convert the TCP endpoint to a generic one (same sockaddr layout).
    asio::generic::stream_protocol::endpoint genEp(tcpEp.data(), tcpEp.size());
    socket_.async_connect(genEp, [this](bsys::error_code ec2) { onConnect(ec2); });
}

void MapTransport::resetConnection() {
    writeQueue_.clear();
    writing_ = false;
    decoder_ = m3ua::M3uaDecoder{}; // discard partial data from old connection
}

void MapTransport::onConnect(bsys::error_code ec) {
    if (ec) {
        std::cerr << "[MapTransport] connect failed: " << ec.message() << "\n";
        resetConnection();
        state_ = State::Disconnected;
        scheduleConnect();
        return;
    }
    std::cout << "[MapTransport] TCP connected to " << cfg_.sgHost
              << ":" << cfg_.sgPort << "\n";
    startRead();
    sendAspUp();
}

// ── M3UA state machine ────────────────────────────────────────────────────────

void MapTransport::sendAspUp() {
    state_ = State::AspUp;
    std::cout << "[MapTransport] sending ASPUP\n";
    sendRaw(makeAspUp());
}

void MapTransport::sendAspAc() {
    std::cout << "[MapTransport] sending ASPAC\n";
    sendRaw(makeAspAc(cfg_.routingContext));
}

void MapTransport::startBeat() {
    beatTimer_.expires_after(cfg_.beatInterval);
    beatTimer_.async_wait([this](bsys::error_code ec) {
        if (!ec && state_ != State::Stopped) onBeat();
    });
}

void MapTransport::onBeat() {
    if (state_ != State::Active) return;
    sendRaw(makeHeartbeat());
    startBeat();
}

void MapTransport::processMessage(const M3uaMessage& msg) {
    // ASPSM: ASP Up Ack
    if (msg.msgClass == kClassAspsm && msg.msgType == kTypeAspUpAck) {
        std::cout << "[MapTransport] ASPUP_ACK received\n";
        sendAspAc();
        return;
    }

    // ASPTM: ASP Active Ack
    if (msg.msgClass == kClassAsptm && msg.msgType == kTypeAspAcAck) {
        std::cout << "[MapTransport] ASPAC_ACK received, session ACTIVE\n";
        state_ = State::Active;
        startBeat();
        return;
    }

    // ASPSM: Heartbeat
    if (msg.msgClass == kClassAspsm && msg.msgType == kTypeHeartbeat) {
        sendRaw(makeHeartbeatAck(msg.heartbeatData));
        return;
    }

    // ASPSM: Heartbeat Ack (ignore)
    if (msg.msgClass == kClassAspsm && msg.msgType == kTypeHeartbeatAck) {
        return;
    }

    // Transfer: Data
    if (msg.msgClass == kClassTransf && msg.msgType == kTypeData) {
        if (!msg.protocolData.has_value()) return;
        const auto& pd = *msg.protocolData;
        if (pd.si != kSiSccp) return;

        // Unwrap SCCP UDT to get MAP payload
        try {
            auto udt = decodeUdt(pd.userData);
            if (messageCb_) messageCb_(udt.data, 0);
        } catch (const std::exception& e) {
            std::cerr << "[MapTransport] SCCP decode error: " << e.what() << "\n";
        }
        return;
    }
}

// ── Sending ───────────────────────────────────────────────────────────────────

void MapTransport::send(const Bytes& mapPayload, ClientId /*clientId*/) {
    auto payload = mapPayload;
    asio::post(ioc_, [this, payload = std::move(payload)]() mutable {
        if (state_ != State::Active) {
            std::cerr << "[MapTransport] send() called but not ACTIVE — dropped\n";
            return;
        }

        // Wrap MAP in SCCP UDT
        SccpUdt udt;
        udt.protocolClass = kProtoClass0;
        udt.calledParty   = makeGtAddress(cfg_.hlrGt, cfg_.hlrSsn, true);
        udt.callingParty  = makeGtAddress(cfg_.localGt, cfg_.localSsn, true);
        udt.data          = payload;
        Bytes sccpFrame   = encodeUdt(udt);

        // Wrap SCCP in M3UA DATA
        ProtocolData pd;
        pd.opc      = cfg_.opc;
        pd.dpc      = cfg_.dpc;
        pd.si       = kSiSccp;
        pd.ni       = kNiInternational;
        pd.sls      = 0;
        pd.userData = std::move(sccpFrame);

        sendRaw(makeData(std::move(pd), cfg_.routingContext));
    });
}

void MapTransport::sendRaw(M3uaMessage msg) {
    enqueue(encode(msg));
}

void MapTransport::enqueue(Bytes frame) {
    writeQueue_.push_back(std::move(frame));
    if (!writing_) doWrite();
}

void MapTransport::doWrite() {
    if (writeQueue_.empty()) {
        writing_ = false;
        return;
    }
    writing_ = true;
    asio::async_write(socket_, asio::buffer(writeQueue_.front()),
        [this](bsys::error_code ec, std::size_t) {
            if (!ec) {
                writeQueue_.pop_front();
                doWrite();
            } else if (state_ != State::Stopped) {
                std::cerr << "[MapTransport] write error: " << ec.message() << "\n";
                state_ = State::Disconnected;
                resetConnection();
                bsys::error_code ignore;
                socket_.close(ignore);
                scheduleConnect();
            }
        });
}

// ── Reading ───────────────────────────────────────────────────────────────────

void MapTransport::startRead() {
    socket_.async_read_some(asio::buffer(readBuf_),
        [this](bsys::error_code ec, std::size_t n) {
            if (ec) {
                if (state_ != State::Stopped) {
                    std::cerr << "[MapTransport] read error: " << ec.message() << "\n";
                    state_ = State::Disconnected;
                    beatTimer_.cancel();
                    resetConnection();
                    bsys::error_code ignore;
                    socket_.close(ignore);
                    scheduleConnect();
                }
                return;
            }
            decoder_.feed(readBuf_.data(), n);
            while (auto msg = decoder_.next()) {
                try {
                    processMessage(*msg);
                } catch (const std::exception& e) {
                    std::cerr << "[MapTransport] processMessage: " << e.what() << "\n";
                }
            }
            if (state_ != State::Stopped && state_ != State::Disconnected)
                startRead();
        });
}

} // namespace proxy::transport
