#include "transport/IpaServer.hpp"
#include <spdlog/spdlog.h>

namespace proxy::transport {

namespace bsys = boost::system;

IpaServer::IpaServer(asio::io_context& ioc, uint16_t port)
    : ioc_(ioc)
    , acceptor_(ioc,
                asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port),
                /*reuse_addr=*/true)
{}

void IpaServer::start() {
    acceptNext();
}

void IpaServer::stop() {
    bsys::error_code ec;
    acceptor_.close(ec);
    for (auto& [id, session] : allSessions_)
        session->stop();
    sessions_.clear();
    allSessions_.clear();
}

void IpaServer::send(const Bytes& gsupPayload, ClientId clientId) {
    auto payload = gsupPayload;
    asio::post(ioc_, [this, payload = std::move(payload), clientId]() {
        if (clientId == 0) {
            // Broadcast to all active sessions
            for (auto& [id, session] : sessions_)
                if (session->isReady()) session->sendGsup(payload);
        } else {
            auto it = sessions_.find(clientId);
            if (it != sessions_.end() && it->second->isReady())
                it->second->sendGsup(payload);
        }
    });
}

void IpaServer::acceptNext() {
    acceptor_.async_accept(
        [this](bsys::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (ec != asio::error::operation_aborted)
                    spdlog::error("[IpaServer] accept error: {}", ec.message());
                return;
            }

            spdlog::info("[IpaServer] connection from {}:{}",
                         socket.remote_endpoint().address().to_string(),
                         socket.remote_endpoint().port());

            ClientId id = nextClientId_++;
            auto session = std::make_shared<IpaSession>(std::move(socket));
            allSessions_[id] = session;

            session->start(
                [this, id]()         { onSessionReady(id); },
                [this, id](Bytes d)  { if (messageCb_) messageCb_(std::move(d), id); },
                [this, id]()         { onSessionDisconnect(id); });

            acceptNext();
        });
}

void IpaServer::onSessionReady(ClientId id) {
    auto it = allSessions_.find(id);
    if (it == allSessions_.end()) return;
    sessions_[id] = it->second;
    spdlog::info("[IpaServer] CCM handshake complete, session {} ready", id);
}

void IpaServer::onSessionDisconnect(ClientId id) {
    sessions_.erase(id);
    allSessions_.erase(id);
    spdlog::info("[IpaServer] session {} disconnected", id);
}

} // namespace proxy::transport
