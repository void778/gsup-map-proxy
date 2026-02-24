#include "proxy/Proxy.hpp"
#include "transport/IpaServer.hpp"
#include "transport/MapTransport.hpp"

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include <memory>
#include <csignal>

namespace asio = boost::asio;
using namespace proxy;
using namespace proxy::transport;

// ── Signal handling ───────────────────────────────────────────────────────────

static asio::io_context* gIoc = nullptr;

static void onSignal(int) {
    if (gIoc) gIoc->stop();
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    uint16_t    listenPort = 4222;
    std::string sgHost     = "127.0.0.1";
    uint16_t    sgPort     = 2905;
    uint32_t    opc        = 1;
    uint32_t    dpc        = 2;
    std::string hlrGt      = "+49161000000";
    std::string localGt    = "+49161000001";

    std::optional<uint32_t> routingContext;

    if (argc > 1) listenPort = static_cast<uint16_t>(std::stoi(argv[1]));
    if (argc > 2) sgHost     = argv[2];
    if (argc > 3) sgPort     = static_cast<uint16_t>(std::stoi(argv[3]));
    if (argc > 4) opc        = static_cast<uint32_t>(std::stoul(argv[4]));
    if (argc > 5) dpc        = static_cast<uint32_t>(std::stoul(argv[5]));
    if (argc > 6) hlrGt      = argv[6];
    if (argc > 7) localGt    = argv[7];
    if (argc > 8) routingContext = static_cast<uint32_t>(std::stoul(argv[8]));

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    asio::io_context ioc;
    gIoc = &ioc;

    // SGSN-facing transport (IPA/TCP server)
    auto sgsnTransport = std::make_shared<IpaServer>(ioc, listenPort);

    // HLR-facing transport (M3UA/SCCP/MAP over TCP to Signalling Gateway)
    MapTransportConfig mapCfg;
    mapCfg.sgHost          = sgHost;
    mapCfg.sgPort          = sgPort;
    mapCfg.opc             = opc;
    mapCfg.dpc             = dpc;
    mapCfg.hlrGt           = hlrGt;
    mapCfg.localGt         = localGt;
    mapCfg.routingContext  = routingContext;
    auto hlrTransport = std::make_shared<MapTransport>(ioc, mapCfg);

    Proxy proxy(sgsnTransport, hlrTransport);
    proxy.start();
    sgsnTransport->start();
    hlrTransport->start();

    // Periodically expire stale transactions (no HLR response within 30 s).
    asio::steady_timer expiryTimer(ioc);
    std::function<void(boost::system::error_code)> tick;
    tick = [&](boost::system::error_code ec) {
        if (ec) return;
        proxy.transactions().expireStale();
        proxy.expireHlrTransactions();
        expiryTimer.expires_after(std::chrono::seconds(5));
        expiryTimer.async_wait(tick);
    };
    expiryTimer.expires_after(std::chrono::seconds(5));
    expiryTimer.async_wait(tick);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    spdlog::info("GSUP-MAP proxy starting");
    spdlog::info("  SGSN port : {}", listenPort);
    spdlog::info("  HLR SG    : {}:{}", sgHost, sgPort);
    if (routingContext)
        spdlog::info("  Routing ctx: {}", *routingContext);
    spdlog::info("Press Ctrl+C to stop");

    ioc.run();

    spdlog::info("Proxy stopped");
    return 0;
}
