#include "transport/IpaSession.hpp"
#include <spdlog/spdlog.h>

namespace proxy::transport {

namespace bsys = boost::system;

IpaSession::IpaSession(asio::ip::tcp::socket socket)
    : socket_(std::move(socket))
{}

IpaSession::~IpaSession() {
    bsys::error_code ec;
    socket_.close(ec);
}

void IpaSession::start(ReadyHandler onReady, GsupHandler onGsup,
                       DisconnectHandler onDisconnect) {
    onReady_      = std::move(onReady);
    onGsup_       = std::move(onGsup);
    onDisconnect_ = std::move(onDisconnect);
    sendCcmIdGet();
    startRead();
}

void IpaSession::stop() {
    if (state_ == State::Stopped) return;
    state_ = State::Stopped;
    bsys::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

// ── Sending ───────────────────────────────────────────────────────────────────

void IpaSession::sendGsup(const Bytes& gsupPayload) {
    auto frame = ipa::encode(ipa::kStreamGsup, gsupPayload);
    asio::post(socket_.get_executor(),
        [self = shared_from_this(), frame = std::move(frame)]() mutable {
            self->enqueue(std::move(frame));
        });
}

void IpaSession::enqueue(Bytes ipaFrame) {
    writeQueue_.push_back(std::move(ipaFrame));
    if (!writing_) doWrite();
}

void IpaSession::doWrite() {
    if (writeQueue_.empty()) {
        writing_ = false;
        return;
    }
    writing_ = true;
    asio::async_write(socket_, asio::buffer(writeQueue_.front()),
        [self = shared_from_this()](bsys::error_code ec, std::size_t) {
            if (!ec) {
                self->writeQueue_.pop_front();
                self->doWrite();
            } else if (self->state_ != State::Stopped) {
                spdlog::error("[IpaSession] write error: {}", ec.message());
                self->state_ = State::Stopped;
                if (self->onDisconnect_) self->onDisconnect_();
            }
        });
}

// ── Reading ───────────────────────────────────────────────────────────────────

void IpaSession::startRead() {
    socket_.async_read_some(asio::buffer(readBuf_),
        [self = shared_from_this()](bsys::error_code ec, std::size_t n) {
            if (ec) {
                if (self->state_ != State::Stopped) {
                    self->state_ = State::Stopped;
                    if (self->onDisconnect_) self->onDisconnect_();
                }
                return;
            }
            self->decoder_.feed(self->readBuf_.data(), n);
            while (auto frame = self->decoder_.next())
                self->processFrame(*frame);
            if (self->state_ != State::Stopped)
                self->startRead();
        });
}

void IpaSession::processFrame(const ipa::IpaFrame& frame) {
    if (frame.streamId == ipa::kStreamCcm) {
        handleCcm(frame.payload);
    } else if (frame.streamId == ipa::kStreamGsup && state_ == State::Ready) {
        spdlog::debug("[IpaSession] GSUP frame dispatched ({} bytes)", frame.payload.size());
        if (onGsup_) onGsup_(frame.payload);
    }
}

void IpaSession::handleCcm(const Bytes& payload) {
    if (payload.empty()) return;
    switch (payload[0]) {
        case ipa::kCcmIdResp:
            sendCcmIdAck();
            state_ = State::Ready;
            spdlog::debug("[IpaSession] CCM handshake complete → Ready");
            if (onReady_) onReady_();
            break;
        case ipa::kCcmPing:
            spdlog::trace("[IpaSession] CCM PING → PONG");
            sendCcmPong();
            break;
        case ipa::kCcmIdAck:
            break; // Some clients echo ACK — ignore.
        default:
            break;
    }
}

// ── CCM frame helpers ────────────────────────────────────────────────────────

void IpaSession::sendCcmIdGet() {
    Bytes payload = {ipa::kCcmIdReq, 0x01, 0x07}; // ask for UNIT_ID
    enqueue(ipa::encode(ipa::kStreamCcm, payload));
}

void IpaSession::sendCcmIdAck() {
    enqueue(ipa::encode(ipa::kStreamCcm, Bytes{ipa::kCcmIdAck}));
}

void IpaSession::sendCcmPong() {
    enqueue(ipa::encode(ipa::kStreamCcm, Bytes{ipa::kCcmPong}));
}

} // namespace proxy::transport
