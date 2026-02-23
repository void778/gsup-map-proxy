#include <gtest/gtest.h>
#include "transport/MapTransport.hpp"
#include "transport/M3uaCodec.hpp"
#include "transport/ScccpCodec.hpp"
#include "map/MapCodec.hpp"

#include <boost/asio.hpp>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>
#include <chrono>

namespace asio = boost::asio;
namespace bsys = boost::system;
using namespace proxy;
using namespace proxy::transport;
using proxy::ClientId;
using namespace proxy::transport::m3ua;
using namespace proxy::transport::sccp;

// ── Mock Signalling Gateway ───────────────────────────────────────────────────
//
// A minimal TCP server that performs the M3UA handshake and can inject
// DATA messages.  Runs on its own io_context thread.

class MockSG {
public:
    explicit MockSG(asio::io_context& ioc)
        : acceptor_(ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0))
        , socket_(ioc)
    {
        acceptor_.async_accept(socket_, [this](bsys::error_code ec) {
            if (!ec) startRead();
        });
    }

    uint16_t port() const { return acceptor_.local_endpoint().port(); }

    // Inject a DATA message carrying `mapPayload` (wrapped in SCCP UDT).
    void injectMapPayload(const Bytes& mapPayload,
                          const std::string& hlrGt, uint8_t hlrSsn,
                          const std::string& localGt, uint8_t localSsn,
                          uint32_t opc, uint32_t dpc) {
        SccpUdt udt;
        udt.calledParty  = makeGtAddress(localGt, localSsn, true);
        udt.callingParty = makeGtAddress(hlrGt,   hlrSsn,   true);
        udt.data         = mapPayload;

        ProtocolData pd;
        pd.opc      = dpc; // from SG's perspective, roles are swapped
        pd.dpc      = opc;
        pd.si       = kSiSccp;
        pd.userData = encodeUdt(udt);

        auto msg = makeData(std::move(pd));
        auto bytes = encode(msg);

        asio::async_write(socket_, asio::buffer(bytes),
            [](bsys::error_code, std::size_t) {});
    }

    // Number of DATA messages received from MapTransport
    int dataReceived() const { return dataCount_.load(); }

    // Wait until at least `n` DATA messages have been received
    bool waitForData(int n, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (dataCount_.load() >= n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool isActive() const { return active_.load(); }

    int heartbeatsReceived() const { return heartbeatCount_.load(); }

    bool waitForHeartbeat(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        auto deadline = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < deadline) {
            if (heartbeatCount_.load() > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

private:
    void startRead() {
        socket_.async_read_some(asio::buffer(readBuf_),
            [this](bsys::error_code ec, std::size_t n) {
                if (ec) return;
                decoder_.feed(readBuf_.data(), n);
                while (auto msg = decoder_.next()) {
                    handleMsg(*msg);
                }
                startRead();
            });
    }

    void handleMsg(const M3uaMessage& msg) {
        if (msg.msgClass == kClassAspsm && msg.msgType == kTypeAspUp) {
            // Send ASPUP_ACK
            M3uaMessage ack{kClassAspsm, kTypeAspUpAck, std::nullopt, std::nullopt, {}};
            auto bytes = encode(ack);
            asio::async_write(socket_, asio::buffer(bytes),
                [](bsys::error_code, std::size_t) {});
        }
        else if (msg.msgClass == kClassAsptm && msg.msgType == kTypeAspAc) {
            // Send ASPAC_ACK
            M3uaMessage ack{kClassAsptm, kTypeAspAcAck, std::nullopt, std::nullopt, {}};
            auto bytes = encode(ack);
            asio::async_write(socket_, asio::buffer(bytes),
                [bytes, this](bsys::error_code, std::size_t) {
                    active_.store(true);
                });
        }
        else if (msg.msgClass == kClassAspsm && msg.msgType == kTypeHeartbeat) {
            ++heartbeatCount_;
            auto ack = makeHeartbeatAck(msg.heartbeatData);
            auto bytes = encode(ack);
            asio::async_write(socket_, asio::buffer(bytes),
                [](bsys::error_code, std::size_t) {});
        }
        else if (msg.msgClass == kClassTransf && msg.msgType == kTypeData) {
            ++dataCount_;
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket   socket_;
    M3uaDecoder             decoder_;
    std::array<uint8_t, 4096> readBuf_;
    std::atomic<int>  dataCount_{0};
    std::atomic<bool> active_{false};
    std::atomic<int>  heartbeatCount_{0};
};

// ── Test fixture ──────────────────────────────────────────────────────────────

class MapTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        work_ = std::make_unique<
            asio::executor_work_guard<asio::io_context::executor_type>>(
            ioc_.get_executor());
        sg_   = std::make_unique<MockSG>(ioc_);
        ioThread_ = std::thread([this] { ioc_.run(); });

        MapTransportConfig cfg;
        cfg.sgHost          = "127.0.0.1";
        cfg.sgPort          = sg_->port();
        cfg.opc             = 1;
        cfg.dpc             = 2;
        cfg.hlrGt           = "49161000000";
        cfg.localGt         = "49161000001";
        cfg.hlrSsn          = kSsnHlr;
        cfg.localSsn        = kSsnSgsn;
        cfg.reconnectInterval = std::chrono::seconds(1);
        cfg.beatInterval    = std::chrono::seconds(60); // long — don't fire in tests

        transport_ = std::make_unique<MapTransport>(ioc_, cfg);
        transport_->start();
    }

    void TearDown() override {
        // Ensure stop() executes on the io_context thread before we kill it.
        // Use shared_ptr so the promise stays alive even if wait_for times out.
        auto stopped = std::make_shared<std::promise<void>>();
        asio::post(ioc_, [this, stopped] {
            if (transport_) transport_->stop();
            stopped->set_value();
        });
        stopped->get_future().wait_for(std::chrono::seconds(3));
        work_.reset();
        ioc_.stop();
        if (ioThread_.joinable()) ioThread_.join();
    }

    // Wait up to timeout for MockSG to reach ACTIVE state
    bool waitForActive(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        auto deadline = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < deadline) {
            if (sg_->isActive()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    asio::io_context ioc_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::unique_ptr<MockSG>          sg_;
    std::unique_ptr<MapTransport>    transport_;
    std::thread                      ioThread_;
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(MapTransportTest, HandshakeCompletes) {
    EXPECT_TRUE(waitForActive());
}

TEST_F(MapTransportTest, SendDeliversDATAToSG) {
    ASSERT_TRUE(waitForActive());

    Bytes mapPayload = {0x62, 0x03, 0x01, 0x02, 0x03}; // dummy MAP bytes
    transport_->send(mapPayload);

    EXPECT_TRUE(sg_->waitForData(1));
    EXPECT_EQ(sg_->dataReceived(), 1);
}

TEST_F(MapTransportTest, MultipleSendsDeliverMultipleDATA) {
    ASSERT_TRUE(waitForActive());

    for (int i = 0; i < 3; ++i)
        transport_->send({0x62, static_cast<uint8_t>(i)});

    EXPECT_TRUE(sg_->waitForData(3));
    EXPECT_EQ(sg_->dataReceived(), 3);
}

TEST_F(MapTransportTest, InboundDataDeliveredToCallback) {
    ASSERT_TRUE(waitForActive());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Bytes received;
    std::mutex mtx;
    std::condition_variable cv;
    bool got = false;

    transport_->onMessage([&](const Bytes& data, ClientId) {
        std::lock_guard lock(mtx);
        received = data;
        got = true;
        cv.notify_one();
    });

    // Inject a MAP payload from the mock SG
    Bytes mapPayload = {0x64, 0x01, 0xAB}; // arbitrary MAP bytes
    asio::post(ioc_, [this, mapPayload] {
        sg_->injectMapPayload(mapPayload,
            "49161000000", kSsnHlr,
            "49161000001", kSsnSgsn,
            1, 2);
    });

    std::unique_lock lock(mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }))
        << "Timed out waiting for inbound MAP payload";

    EXPECT_EQ(received, mapPayload);
}

// ── Standalone tests (own io_context + MockSG instance) ───────────────────────

// Runs a fresh environment so the heartbeat/SCTP tests don't share MockSG's
// single accept slot with the fixture tests above.
struct StandaloneEnv {
    asio::io_context ioc;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work{
        std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            ioc.get_executor())};
    std::unique_ptr<MockSG> sg{std::make_unique<MockSG>(ioc)};
    std::unique_ptr<MapTransport> transport;
    std::thread ioThread{[this]{ ioc.run(); }};

    MapTransportConfig makeCfg() {
        MapTransportConfig cfg;
        cfg.sgHost  = "127.0.0.1";
        cfg.sgPort  = sg->port();
        cfg.opc = 1; cfg.dpc = 2;
        cfg.hlrGt   = "49161000000"; cfg.hlrSsn   = kSsnHlr;
        cfg.localGt = "49161000001"; cfg.localSsn = kSsnSgsn;
        cfg.reconnectInterval = std::chrono::seconds(1);
        cfg.beatInterval      = std::chrono::seconds(60);
        return cfg;
    }

    void teardown() {
        auto done = std::make_shared<std::promise<void>>();
        asio::post(ioc, [this, done]{
            if (transport) transport->stop();
            done->set_value();
        });
        done->get_future().wait_for(std::chrono::seconds(3));
        work.reset();
        ioc.stop();
        if (ioThread.joinable()) ioThread.join();
    }

    bool waitForActive(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        auto dl = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < dl) {
            if (sg->isActive()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
};

TEST(MapTransportStandaloneTest, HeartbeatIsSentAndAcked) {
    StandaloneEnv env;
    auto cfg = env.makeCfg();
    cfg.beatInterval = std::chrono::seconds(0); // fire immediately after ACTIVE
    env.transport = std::make_unique<MapTransport>(env.ioc, cfg);
    env.transport->start();

    ASSERT_TRUE(env.waitForActive());
    EXPECT_TRUE(env.sg->waitForHeartbeat(std::chrono::seconds(2)));
    EXPECT_GE(env.sg->heartbeatsReceived(), 1);
    env.teardown();
}

TEST(MapTransportStandaloneTest, SctpFallbackWarningWhenNotCompiled) {
    // useSCTP=true without ENABLE_SCTP_TRANSPORT → logs warning, falls back to TCP.
    StandaloneEnv env;
    auto cfg = env.makeCfg();
    cfg.useSCTP = true;
    env.transport = std::make_unique<MapTransport>(env.ioc, cfg);
    env.transport->start();

    EXPECT_TRUE(env.waitForActive());
    env.teardown();
}

TEST_F(MapTransportTest, SendBeforeActiveIsDropped) {
    // Don't wait for handshake — send immediately.
    Bytes payload = {0x01, 0x02, 0x03};
    EXPECT_NO_THROW(transport_->send(payload));
    // SG should not see a DATA message
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(sg_->dataReceived(), 0);
}
