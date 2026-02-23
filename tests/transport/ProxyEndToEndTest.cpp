#include <gtest/gtest.h>

// Transport layer
#include "transport/IpaServer.hpp"
#include "transport/MapTransport.hpp"
#include "transport/M3uaCodec.hpp"
#include "transport/ScccpCodec.hpp"

// Protocol layer
#include "proxy/Proxy.hpp"
#include "gsup/GsupCodec.hpp"
#include "gsup/GsupMessage.hpp"
#include "ipa/IpaCodec.hpp"
#include "ipa/IpaFrame.hpp"
#include "map/MapCodec.hpp"
#include "map/MapMessage.hpp"

#include <boost/asio.hpp>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <future>
#include <atomic>

namespace asio = boost::asio;
namespace bsys = boost::system;
using namespace proxy;
using namespace proxy::transport;
using namespace proxy::transport::m3ua;
using namespace proxy::transport::sccp;

// ── Synchronous IPA test client (simulates osmoSGSN) ─────────────────────────

class IpaTestClient {
public:
    explicit IpaTestClient(uint16_t port) : socket_(clientIoc_) {
        asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
        socket_.connect(ep);
    }
    ~IpaTestClient() {
        bsys::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    void sendRaw(const Bytes& data) { asio::write(socket_, asio::buffer(data)); }

    Bytes readExact(std::size_t n) {
        Bytes buf(n);
        asio::read(socket_, asio::buffer(buf));
        return buf;
    }

    ipa::IpaFrame readFrame() {
        auto hdr = readExact(ipa::kHeaderSize);
        uint16_t wireLen  = static_cast<uint16_t>((hdr[0] << 8) | hdr[1]);
        uint8_t  streamId = hdr[2];
        std::size_t payLen = wireLen > 0 ? wireLen - 1u : 0u;
        Bytes payload;
        if (payLen > 0) payload = readExact(payLen);
        return ipa::IpaFrame{streamId, payload};
    }

    void doHandshake() {
        auto f = readFrame();
        ASSERT_EQ(f.streamId, ipa::kStreamCcm);
        ASSERT_EQ(f.payload[0], ipa::kCcmIdReq);

        Bytes idResp = {ipa::kCcmIdResp, 0x07, 0x00};
        sendRaw(ipa::encode(ipa::kStreamCcm, idResp));

        auto ack = readFrame();
        ASSERT_EQ(ack.streamId, ipa::kStreamCcm);
        ASSERT_EQ(ack.payload[0], ipa::kCcmIdAck);
    }

    void sendGsup(const Bytes& payload) {
        sendRaw(ipa::encode(ipa::kStreamGsup, payload));
    }

    ipa::IpaFrame readGsupFrame() {
        // Skip any CCM frames (e.g. ping) until we get a GSUP frame
        while (true) {
            auto f = readFrame();
            if (f.streamId == ipa::kStreamGsup) return f;
        }
    }

private:
    asio::io_context      clientIoc_;
    asio::ip::tcp::socket socket_;
};

// ── Smart Mock Signalling Gateway ─────────────────────────────────────────────
//
// Performs the M3UA handshake, then for every incoming DATA message:
//   1. Decodes M3UA → SCCP UDT → MAP payload
//   2. If Invoke (SGSN-initiated): records lastRequest_, builds and sends response
//   3. If ReturnResult/ReturnError (HLR-initiated response): records lastResponse_
//
// Also supports sendMapInvoke() to inject HLR-initiated MAP Invokes.

struct SgAddressing {
    std::string proxyGt  = "49161000001";   // proxy's local GT (calledParty target)
    uint8_t     proxySsn = kSsnSgsn;
    std::string hlrGt    = "49161000000";   // HLR's GT (callingParty source)
    uint8_t     hlrSsn   = kSsnHlr;
    uint32_t    proxyOpc = 1;               // proxy's point code
    uint32_t    hlrOpc   = 2;               // HLR's point code
};

class SmartMockSG {
public:
    explicit SmartMockSG(asio::io_context& ioc)
        : acceptor_(ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0))
        , socket_(ioc)
        , ioc_(ioc)
    {
        acceptor_.async_accept(socket_, [this](bsys::error_code ec) {
            if (!ec) startRead();
        });
    }

    uint16_t port() const { return acceptor_.local_endpoint().port(); }
    bool isActive() const { return active_.load(); }

    void configure(const SgAddressing& addr) { addr_ = addr; }

    // Block until ACTIVE or timeout.
    bool waitForActive(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        auto dl = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < dl) {
            if (active_) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // Wait until at least one SGSN-initiated DATA has been received.
    bool waitForData(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        std::unique_lock lock(mtx_);
        return cv_.wait_for(lock, t, [this] { return dataCount_ > 0; });
    }

    // Wait until the proxy has sent back an HLR-initiated response.
    bool waitForResponse(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        std::unique_lock lock(mtx_);
        return cv_.wait_for(lock, t, [this] { return responseCount_ > 0; });
    }

    // Last decoded MAP request received from MapTransport (SGSN-initiated).
    std::optional<map::MapMessage> lastRequest() {
        std::lock_guard lock(mtx_);
        return lastRequest_;
    }

    // Last MAP ReturnResult/ReturnError received from proxy (HLR-initiated).
    std::optional<map::MapMessage> lastResponse() {
        std::lock_guard lock(mtx_);
        return lastResponse_;
    }

    // Send a MAP Invoke to the proxy (simulate HLR-initiated operation).
    void sendMapInvoke(const map::MapMessage& invoke) {
        Bytes mapBytes;
        try { mapBytes = map::encode(invoke); }
        catch (...) { return; }

        SccpUdt udt;
        udt.protocolClass = kProtoClass0;
        udt.calledParty   = makeGtAddress(addr_.proxyGt, addr_.proxySsn, true);
        udt.callingParty  = makeGtAddress(addr_.hlrGt,   addr_.hlrSsn,   true);
        udt.data          = mapBytes;

        ProtocolData pd;
        pd.opc      = addr_.hlrOpc;
        pd.dpc      = addr_.proxyOpc;
        pd.si       = kSiSccp;
        pd.userData = encodeUdt(udt);

        Bytes frame = encode(makeData(std::move(pd)));
        asio::post(ioc_, [this, frame = std::move(frame)]() mutable {
            asyncSend(std::move(frame));
        });
    }

private:
    void startRead() {
        socket_.async_read_some(asio::buffer(readBuf_),
            [this](bsys::error_code ec, std::size_t n) {
                if (ec) return;
                decoder_.feed(readBuf_.data(), n);
                while (auto msg = decoder_.next()) handleMsg(*msg);
                startRead();
            });
    }

    void handleMsg(const M3uaMessage& msg) {
        if (msg.msgClass == kClassAspsm && msg.msgType == kTypeAspUp) {
            M3uaMessage ack{kClassAspsm, kTypeAspUpAck, {}, {}, {}};
            asyncSend(encode(ack));
            return;
        }
        if (msg.msgClass == kClassAsptm && msg.msgType == kTypeAspAc) {
            M3uaMessage ack{kClassAsptm, kTypeAspAcAck, {}, {}, {}};
            asyncSend(encode(ack));
            active_.store(true);
            return;
        }
        if (msg.msgClass == kClassAspsm && msg.msgType == kTypeHeartbeat) {
            asyncSend(encode(makeHeartbeatAck(msg.heartbeatData)));
            return;
        }
        if (msg.msgClass == kClassTransf && msg.msgType == kTypeData) {
            handleData(msg);
        }
    }

    void handleData(const M3uaMessage& m3uaMsg) {
        if (!m3uaMsg.protocolData) return;
        const auto& pd = *m3uaMsg.protocolData;

        SccpUdt udt;
        try { udt = decodeUdt(pd.userData); }
        catch (...) { return; }

        map::MapMessage mapMsg;
        try { mapMsg = map::decode(udt.data); }
        catch (...) { return; }

        // HLR-initiated response: ReturnResult/ReturnError from proxy back to HLR
        if (mapMsg.component == map::ComponentType::ReturnResult ||
            mapMsg.component == map::ComponentType::ReturnError) {
            std::lock_guard lock(mtx_);
            lastResponse_ = mapMsg;
            ++responseCount_;
            cv_.notify_all();
            return;
        }

        // SGSN-initiated Invoke: record and auto-respond
        {
            std::lock_guard lock(mtx_);
            lastRequest_ = mapMsg;
            ++dataCount_;
            cv_.notify_all();
        }

        map::MapMessage resp = buildResponse(mapMsg);
        Bytes mapRespBytes;
        try { mapRespBytes = map::encode(resp); }
        catch (...) { return; }

        // Wrap response in SCCP UDT (swap called/calling) → M3UA DATA
        SccpUdt respUdt;
        respUdt.protocolClass = kProtoClass0;
        respUdt.calledParty   = udt.callingParty;   // route back to proxy
        respUdt.callingParty  = udt.calledParty;
        respUdt.data          = mapRespBytes;

        ProtocolData respPd;
        respPd.opc      = pd.dpc;
        respPd.dpc      = pd.opc;
        respPd.si       = kSiSccp;
        respPd.userData = encodeUdt(respUdt);

        asyncSend(encode(makeData(std::move(respPd))));
    }

    static map::MapMessage buildResponse(const map::MapMessage& req) {
        map::MapMessage resp;
        resp.transactionId   = req.transactionId;
        resp.component       = map::ComponentType::ReturnResult;
        resp.invokeId        = req.invokeId;
        resp.isLastComponent = true;
        resp.operation       = req.operation;
        resp.imsi            = req.imsi;

        switch (req.operation) {
            case map::MapOperation::SendAuthenticationInfo: {
                map::MapAuthTriplet t;
                t.rand = Bytes(16, 0xAA);
                t.sres = Bytes(4,  0xBB);
                t.kc   = Bytes(8,  0xCC);
                resp.authTriplets.push_back(t);
                break;
            }
            case map::MapOperation::UpdateGprsLocation:
                resp.hlrNumber = Bytes{0x91, 0x49, 0x16, 0x10, 0x00};
                break;
            case map::MapOperation::CancelLocation:
            case map::MapOperation::PurgeMS:
                break; // empty result is fine
            default:
                break;
        }
        return resp;
    }

    void asyncSend(Bytes data) {
        auto buf = std::make_shared<Bytes>(std::move(data));
        asio::async_write(socket_, asio::buffer(*buf),
            [buf](bsys::error_code, std::size_t) {});
    }

    asio::ip::tcp::acceptor   acceptor_;
    asio::ip::tcp::socket     socket_;
    asio::io_context&         ioc_;
    M3uaDecoder               decoder_;
    std::array<uint8_t, 4096> readBuf_;
    SgAddressing              addr_;

    std::atomic<bool>  active_{false};
    std::mutex         mtx_;
    std::condition_variable cv_;
    int                dataCount_{0};
    int                responseCount_{0};
    std::optional<map::MapMessage> lastRequest_;
    std::optional<map::MapMessage> lastResponse_;
};

// ── Test fixture ──────────────────────────────────────────────────────────────

class ProxyEndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        work_ = std::make_unique<
            asio::executor_work_guard<asio::io_context::executor_type>>(
            ioc_.get_executor());

        sg_        = std::make_unique<SmartMockSG>(ioc_);
        ipaServer_ = std::make_shared<IpaServer>(ioc_, /*port=*/0);

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
        cfg.beatInterval    = std::chrono::seconds(60);

        SgAddressing addr;
        addr.proxyGt  = cfg.localGt;
        addr.proxySsn = cfg.localSsn;
        addr.hlrGt    = cfg.hlrGt;
        addr.hlrSsn   = cfg.hlrSsn;
        addr.proxyOpc = cfg.opc;
        addr.hlrOpc   = cfg.dpc;
        sg_->configure(addr);

        mapTransport_ = std::make_shared<MapTransport>(ioc_, cfg);
        proxy_ = std::make_unique<Proxy>(ipaServer_, mapTransport_);

        proxy_->start();
        ipaServer_->start();
        mapTransport_->start();

        ioThread_ = std::thread([this] { ioc_.run(); });
    }

    void TearDown() override {
        asio::post(ioc_, [this] {
            ipaServer_->stop();
            mapTransport_->stop();
        });
        work_.reset();
        ioc_.stop();
        if (ioThread_.joinable()) ioThread_.join();
    }

    // Wait for SmartMockSG to complete M3UA handshake
    bool waitForSgActive(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        return sg_->waitForActive(t);
    }

    asio::io_context ioc_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::unique_ptr<SmartMockSG>    sg_;
    std::shared_ptr<IpaServer>      ipaServer_;
    std::shared_ptr<MapTransport>   mapTransport_;
    std::unique_ptr<Proxy>          proxy_;
    std::thread                    ioThread_;
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(ProxyEndToEndTest, SendAuthInfoFullRoundTrip) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Build and send GSUP SendAuthInfoRequest from SGSN
    gsup::GsupMessage req;
    req.type                = gsup::MessageType::SendAuthInfoRequest;
    req.imsi                = "262019876543210";
    req.numVectorsRequested = 1;
    sgsn.sendGsup(gsup::encode(req));

    // Read GSUP response at SGSN
    auto f = sgsn.readGsupFrame();
    ASSERT_EQ(f.streamId, ipa::kStreamGsup);

    auto resp = gsup::decode(f.payload);
    EXPECT_EQ(resp.type, gsup::MessageType::SendAuthInfoResult);
    EXPECT_EQ(resp.imsi, "262019876543210");
    ASSERT_EQ(resp.authTuples.size(), 1u);
    EXPECT_EQ(resp.authTuples[0].rand, Bytes(16, 0xAA));
    EXPECT_EQ(resp.authTuples[0].sres, Bytes(4,  0xBB));
    EXPECT_EQ(resp.authTuples[0].kc,   Bytes(8,  0xCC));
}

TEST_F(ProxyEndToEndTest, MapRequestContainsCorrectOperation) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    gsup::GsupMessage req;
    req.type                = gsup::MessageType::SendAuthInfoRequest;
    req.imsi                = "001010000000001";
    req.numVectorsRequested = 3;
    sgsn.sendGsup(gsup::encode(req));

    // Wait for SG to receive it
    ASSERT_TRUE(sg_->waitForData());

    auto mapReq = sg_->lastRequest();
    ASSERT_TRUE(mapReq.has_value());
    EXPECT_EQ(mapReq->operation, map::MapOperation::SendAuthenticationInfo);
    EXPECT_EQ(mapReq->imsi, "001010000000001");
    EXPECT_EQ(mapReq->component, map::ComponentType::Invoke);

    // Also consume the GSUP response so the test client doesn't hang on teardown
    sgsn.readGsupFrame();
}

TEST_F(ProxyEndToEndTest, UpdateLocationRoundTrip) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    gsup::GsupMessage req;
    req.type = gsup::MessageType::UpdateLocationRequest;
    req.imsi = "262019000000042";
    sgsn.sendGsup(gsup::encode(req));

    auto f = sgsn.readGsupFrame();
    ASSERT_EQ(f.streamId, ipa::kStreamGsup);

    auto resp = gsup::decode(f.payload);
    EXPECT_EQ(resp.type, gsup::MessageType::UpdateLocationResult);
    EXPECT_EQ(resp.imsi, "262019000000042");
}

TEST_F(ProxyEndToEndTest, MapReturnErrorPropagatesAsGsupError) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Register a custom onMessage on mapTransport so we can inject an error response
    // Instead: send a GSUP request, wait for SG to receive MAP, then verify
    // the SmartMockSG sends back an error response for PurgeMSRequest
    // (SmartMockSG sends empty ReturnResult for PurgeMS, which maps to PurgeMsResult)
    gsup::GsupMessage req;
    req.type = gsup::MessageType::PurgeMsRequest;
    req.imsi = "262019000000099";
    sgsn.sendGsup(gsup::encode(req));

    auto f = sgsn.readGsupFrame();
    ASSERT_EQ(f.streamId, ipa::kStreamGsup);

    auto resp = gsup::decode(f.payload);
    EXPECT_EQ(resp.type, gsup::MessageType::PurgeMsResult);
    EXPECT_EQ(resp.imsi, "262019000000099");
}

TEST_F(ProxyEndToEndTest, TransactionCompletedAfterRoundTrip) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    gsup::GsupMessage req;
    req.type                = gsup::MessageType::SendAuthInfoRequest;
    req.imsi                = "001010000000001";
    req.numVectorsRequested = 1;
    sgsn.sendGsup(gsup::encode(req));

    // Consume the response
    sgsn.readGsupFrame();

    // After the round-trip the transaction table should be empty
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    asio::post(ioc_, [this] {
        EXPECT_EQ(proxy_->transactions().size(), 0u);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

TEST_F(ProxyEndToEndTest, MultipleSequentialRoundTrips) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 0; i < 3; ++i) {
        gsup::GsupMessage req;
        req.type                = gsup::MessageType::SendAuthInfoRequest;
        req.imsi                = "26201900000000" + std::to_string(i);
        req.numVectorsRequested = 1;
        sgsn.sendGsup(gsup::encode(req));

        auto f = sgsn.readGsupFrame();
        ASSERT_EQ(f.streamId, ipa::kStreamGsup);
        auto resp = gsup::decode(f.payload);
        EXPECT_EQ(resp.type, gsup::MessageType::SendAuthInfoResult);
        EXPECT_EQ(resp.imsi, req.imsi);
    }
}

// ── HLR-initiated tests ────────────────────────────────────────────────────────

TEST_F(ProxyEndToEndTest, HlrInitiatedInsertSubscriberData) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // HLR sends InsertSubscriberData invoke to proxy
    map::MapMessage invoke;
    invoke.transactionId = 0x42;
    invoke.component     = map::ComponentType::Invoke;
    invoke.invokeId      = 1;
    invoke.operation     = map::MapOperation::InsertSubscriberData;
    invoke.imsi          = "262019876543210";
    invoke.msisdn        = Bytes{0x91, 0x49, 0x06, 0x10};
    sg_->sendMapInvoke(invoke);

    // SGSN receives GSUP InsertDataRequest
    auto gsupFrame = sgsn.readGsupFrame();
    ASSERT_EQ(gsupFrame.streamId, ipa::kStreamGsup);
    auto gsupReq = gsup::decode(gsupFrame.payload);
    EXPECT_EQ(gsupReq.type, gsup::MessageType::InsertDataRequest);
    EXPECT_EQ(gsupReq.imsi, "262019876543210");

    // SGSN replies with InsertDataResult
    gsup::GsupMessage gsupResp;
    gsupResp.type = gsup::MessageType::InsertDataResult;
    gsupResp.imsi = "262019876543210";
    sgsn.sendGsup(gsup::encode(gsupResp));

    // HLR (SmartMockSG) should receive MAP ReturnResult
    ASSERT_TRUE(sg_->waitForResponse());
    auto resp = sg_->lastResponse();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->component, map::ComponentType::ReturnResult);
    EXPECT_EQ(resp->operation, map::MapOperation::InsertSubscriberData);
    EXPECT_EQ(resp->transactionId, 0x42u);
    EXPECT_EQ(resp->invokeId, 1u);
}

TEST_F(ProxyEndToEndTest, HlrInitiatedInsertSubscriberDataError) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    map::MapMessage invoke;
    invoke.transactionId = 0x55;
    invoke.component     = map::ComponentType::Invoke;
    invoke.invokeId      = 2;
    invoke.operation     = map::MapOperation::InsertSubscriberData;
    invoke.imsi          = "001010000000001";
    sg_->sendMapInvoke(invoke);

    // SGSN receives GSUP InsertDataRequest
    auto gsupFrame = sgsn.readGsupFrame();
    auto gsupReq = gsup::decode(gsupFrame.payload);
    EXPECT_EQ(gsupReq.type, gsup::MessageType::InsertDataRequest);

    // SGSN replies with InsertDataError
    gsup::GsupMessage gsupErr;
    gsupErr.type  = gsup::MessageType::InsertDataError;
    gsupErr.imsi  = "001010000000001";
    gsupErr.cause = 13;
    sgsn.sendGsup(gsup::encode(gsupErr));

    // HLR receives MAP ReturnError
    // Note: TCAP ReturnError does not carry the operation code on the wire;
    // the receiver identifies the operation from the original Invoke context.
    ASSERT_TRUE(sg_->waitForResponse());
    auto resp = sg_->lastResponse();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->component, map::ComponentType::ReturnError);
    EXPECT_EQ(resp->transactionId, 0x55u);
    EXPECT_EQ(resp->invokeId, 2u);
    ASSERT_TRUE(resp->errorCode.has_value());
    EXPECT_EQ(*resp->errorCode, 13u);
}

TEST_F(ProxyEndToEndTest, HlrInitiatedCancelLocation) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    map::MapMessage invoke;
    invoke.transactionId = 0x77;
    invoke.component     = map::ComponentType::Invoke;
    invoke.invokeId      = 3;
    invoke.operation     = map::MapOperation::CancelLocation;
    invoke.imsi          = "262019000000042";
    invoke.cancelType    = static_cast<uint8_t>(gsup::CancelType::UpdateProcedure);
    sg_->sendMapInvoke(invoke);

    // SGSN receives GSUP LocationCancelRequest
    auto gsupFrame = sgsn.readGsupFrame();
    auto gsupReq = gsup::decode(gsupFrame.payload);
    EXPECT_EQ(gsupReq.type, gsup::MessageType::LocationCancelRequest);
    EXPECT_EQ(gsupReq.imsi, "262019000000042");
    ASSERT_TRUE(gsupReq.cancelType.has_value());
    EXPECT_EQ(*gsupReq.cancelType, gsup::CancelType::UpdateProcedure);

    // SGSN replies with LocationCancelResult
    gsup::GsupMessage gsupResp;
    gsupResp.type = gsup::MessageType::LocationCancelResult;
    gsupResp.imsi = "262019000000042";
    sgsn.sendGsup(gsup::encode(gsupResp));

    // HLR receives MAP ReturnResult(CancelLocation)
    ASSERT_TRUE(sg_->waitForResponse());
    auto resp = sg_->lastResponse();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->component, map::ComponentType::ReturnResult);
    EXPECT_EQ(resp->operation, map::MapOperation::CancelLocation);
    EXPECT_EQ(resp->transactionId, 0x77u);
}

TEST_F(ProxyEndToEndTest, HlrInitiatedPurgeMs) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // HLR sends PurgeMS Invoke to proxy
    map::MapMessage invoke;
    invoke.transactionId = 0x88;
    invoke.component     = map::ComponentType::Invoke;
    invoke.invokeId      = 4;
    invoke.operation     = map::MapOperation::PurgeMS;
    invoke.imsi          = "262019777777777";
    sg_->sendMapInvoke(invoke);

    // SGSN receives GSUP PurgeMsRequest
    auto gsupFrame = sgsn.readGsupFrame();
    auto gsupReq = gsup::decode(gsupFrame.payload);
    EXPECT_EQ(gsupReq.type, gsup::MessageType::PurgeMsRequest);
    EXPECT_EQ(gsupReq.imsi, "262019777777777");

    // SGSN replies with PurgeMsResult
    gsup::GsupMessage gsupResp;
    gsupResp.type = gsup::MessageType::PurgeMsResult;
    gsupResp.imsi = "262019777777777";
    sgsn.sendGsup(gsup::encode(gsupResp));

    // HLR receives MAP ReturnResult(PurgeMS)
    ASSERT_TRUE(sg_->waitForResponse());
    auto resp = sg_->lastResponse();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->component, map::ComponentType::ReturnResult);
    EXPECT_EQ(resp->operation, map::MapOperation::PurgeMS);
    EXPECT_EQ(resp->transactionId, 0x88u);
    EXPECT_EQ(resp->invokeId, 4u);
}

TEST_F(ProxyEndToEndTest, HlrInitiatedTxCleanedUpAfterResponse) {
    ASSERT_TRUE(waitForSgActive());

    IpaTestClient sgsn(ipaServer_->port());
    sgsn.doHandshake();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    map::MapMessage invoke;
    invoke.transactionId = 0x10;
    invoke.component     = map::ComponentType::Invoke;
    invoke.invokeId      = 1;
    invoke.operation     = map::MapOperation::InsertSubscriberData;
    invoke.imsi          = "262019111111111";
    sg_->sendMapInvoke(invoke);

    sgsn.readGsupFrame(); // consume the GSUP request

    gsup::GsupMessage gsupResp;
    gsupResp.type = gsup::MessageType::InsertDataResult;
    gsupResp.imsi = "262019111111111";
    sgsn.sendGsup(gsup::encode(gsupResp));

    ASSERT_TRUE(sg_->waitForResponse());

    // After the round-trip, the HLR transaction table should be empty
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    asio::post(ioc_, [this] {
        EXPECT_EQ(proxy_->hlrTxSize(), 0u);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
