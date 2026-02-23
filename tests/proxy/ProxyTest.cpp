#include <gtest/gtest.h>
#include "proxy/Proxy.hpp"
#include "gsup/GsupCodec.hpp"
#include "map/MapCodec.hpp"

using namespace proxy;
using namespace proxy::gsup;
using namespace proxy::map;

// ── Simple mock transport ─────────────────────────────────────────────────────

class MockTransport : public ITransport {
public:
    struct SentMsg { Bytes data; ClientId clientId; };

    void send(const Bytes& data, ClientId id = 0) override {
        sent.push_back({data, id});
    }

    void onMessage(MessageCallback cb) override { cb_ = std::move(cb); }

    // Simulate an inbound message arriving from the peer.
    void deliver(const Bytes& data, ClientId id = 0) {
        if (cb_) cb_(data, id);
    }

    std::vector<SentMsg> sent;
    MessageCallback cb_;
};

// ── Helper to build a valid GSUP request payload ──────────────────────────────

static Bytes makeSendAuthInfoReq(const std::string& imsi) {
    GsupMessage msg;
    msg.type                = MessageType::SendAuthInfoRequest;
    msg.imsi                = imsi;
    msg.numVectorsRequested = 1;
    return encode(msg);
}

static Bytes makeInsertDataResultPayload(const std::string& imsi) {
    GsupMessage msg;
    msg.type = MessageType::InsertDataResult;
    msg.imsi = imsi;
    return encode(msg);
}

// ── Fixture ───────────────────────────────────────────────────────────────────

class ProxyTest : public ::testing::Test {
protected:
    void SetUp() override {
        sgsn_ = std::make_shared<MockTransport>();
        hlr_  = std::make_shared<MockTransport>();
        proxy_ = std::make_unique<Proxy>(sgsn_, hlr_);
        proxy_->start();
    }

    std::shared_ptr<MockTransport> sgsn_;
    std::shared_ptr<MockTransport> hlr_;
    std::unique_ptr<Proxy>        proxy_;
};

// ── SGSN-initiated error paths ────────────────────────────────────────────────

TEST_F(ProxyTest, GsupDecodeErrorDropsMessage) {
    // Deliver garbage bytes — should not crash and should not send to HLR.
    proxy_->handleGsupPayload({0xFF, 0xFE, 0xFD}, 1);
    EXPECT_TRUE(hlr_->sent.empty());
    EXPECT_EQ(proxy_->transactions().size(), 0u);
}

TEST_F(ProxyTest, GsupUnsupportedTypeDropsMessage) {
    // SendAuthInfoError is a response type — not convertible by gsupToMap.
    GsupMessage msg;
    msg.type = MessageType::SendAuthInfoError;
    msg.imsi = "262019876543210";
    proxy_->handleGsupPayload(encode(msg), 1);
    EXPECT_TRUE(hlr_->sent.empty());
    EXPECT_EQ(proxy_->transactions().size(), 0u);
}

TEST_F(ProxyTest, ValidGsupRequestForwardedToHlr) {
    proxy_->handleGsupPayload(makeSendAuthInfoReq("262019876543210"), 1);
    ASSERT_EQ(hlr_->sent.size(), 1u);
    EXPECT_EQ(proxy_->transactions().size(), 1u);
}

TEST_F(ProxyTest, GsupClientIdStoredInTransaction) {
    proxy_->handleGsupPayload(makeSendAuthInfoReq("001010000000001"), 42);
    ASSERT_EQ(proxy_->transactions().size(), 1u);
    // clientContext is opaque but we can verify routing later (see below).
}

// ── HLR response routing ──────────────────────────────────────────────────────

TEST_F(ProxyTest, MapResponseRoutedToCorrectSgsn) {
    // Two SGSNs send requests with different clientIds.
    proxy_->handleGsupPayload(makeSendAuthInfoReq("001010000000001"), 11);
    proxy_->handleGsupPayload(makeSendAuthInfoReq("001010000000002"), 22);
    ASSERT_EQ(hlr_->sent.size(), 2u);

    // Decode the MAP requests to get their TIDs.
    auto mapMsg1 = map::decode(hlr_->sent[0].data);
    auto mapMsg2 = map::decode(hlr_->sent[1].data);

    // Build a SendAuthInfo ReturnResult for SGSN 1 (TID = mapMsg1.transactionId).
    MapMessage resp;
    resp.transactionId   = mapMsg1.transactionId;
    resp.component       = ComponentType::ReturnResult;
    resp.invokeId        = mapMsg1.invokeId;
    resp.operation       = MapOperation::SendAuthenticationInfo;
    resp.imsi            = "001010000000001";
    MapAuthTriplet t;
    t.rand = Bytes(16, 0xAA); t.sres = Bytes(4, 0xBB); t.kc = Bytes(8, 0xCC);
    resp.authTriplets.push_back(t);

    proxy_->handleMapPayload(map::encode(resp));
    ASSERT_EQ(sgsn_->sent.size(), 1u);
    // Must be routed to clientId 11, not 22.
    EXPECT_EQ(sgsn_->sent[0].clientId, 11u);
}

TEST_F(ProxyTest, MapDecodeErrorDropsResponse) {
    proxy_->handleMapPayload({0xFF, 0xFE});
    EXPECT_TRUE(sgsn_->sent.empty());
}

TEST_F(ProxyTest, MapNoPendingTransactionDropsResponse) {
    MapMessage resp;
    resp.transactionId = 0xDEAD;
    resp.component     = ComponentType::ReturnResult;
    resp.operation     = MapOperation::SendAuthenticationInfo;
    resp.imsi          = "262019876543210";
    proxy_->handleMapPayload(map::encode(resp));
    EXPECT_TRUE(sgsn_->sent.empty());
}

TEST_F(ProxyTest, MapTransactionCompletedAfterResponse) {
    proxy_->handleGsupPayload(makeSendAuthInfoReq("262019876543210"), 1);
    ASSERT_EQ(proxy_->transactions().size(), 1u);
    auto mapMsg = map::decode(hlr_->sent[0].data);

    MapMessage resp;
    resp.transactionId   = mapMsg.transactionId;
    resp.component       = ComponentType::ReturnResult;
    resp.operation       = MapOperation::SendAuthenticationInfo;
    resp.imsi            = "262019876543210";
    MapAuthTriplet t;
    t.rand = Bytes(16, 0); t.sres = Bytes(4, 0); t.kc = Bytes(8, 0);
    resp.authTriplets.push_back(t);
    proxy_->handleMapPayload(map::encode(resp));

    EXPECT_EQ(proxy_->transactions().size(), 0u);
}

TEST_F(ProxyTest, MapReturnErrorForwardedAsSgsupError) {
    proxy_->handleGsupPayload(makeSendAuthInfoReq("262019876543210"), 7);
    auto mapMsg = map::decode(hlr_->sent[0].data);

    MapMessage err;
    err.transactionId = mapMsg.transactionId;
    err.component     = ComponentType::ReturnError;
    err.operation     = MapOperation::SendAuthenticationInfo;
    err.imsi          = "262019876543210";
    err.errorCode     = 34;
    proxy_->handleMapPayload(map::encode(err));

    ASSERT_EQ(sgsn_->sent.size(), 1u);
    auto gsupResp = gsup::decode(sgsn_->sent[0].data);
    EXPECT_EQ(gsupResp.type, MessageType::SendAuthInfoError);
    EXPECT_EQ(sgsn_->sent[0].clientId, 7u);
}

// ── HLR-initiated error paths ─────────────────────────────────────────────────

TEST_F(ProxyTest, HlrInitiatedInvokeSentToSgsn) {
    MapMessage invoke;
    invoke.transactionId = 0x10;
    invoke.component     = ComponentType::Invoke;
    invoke.invokeId      = 1;
    invoke.operation     = MapOperation::InsertSubscriberData;
    invoke.imsi          = "262019876543210";
    proxy_->handleMapPayload(map::encode(invoke));

    ASSERT_EQ(sgsn_->sent.size(), 1u);
    // HLR-initiated broadcasts (clientId == 0).
    EXPECT_EQ(sgsn_->sent[0].clientId, 0u);
    EXPECT_EQ(proxy_->hlrTxSize(), 1u);
}

TEST_F(ProxyTest, HlrInitiatedUnknownImsiDropsResponse) {
    // SGSN sends InsertDataResult for an IMSI that has no HLR tx entry.
    proxy_->handleGsupPayload(makeInsertDataResultPayload("262019876543210"), 5);
    // Should not send anything to HLR, should not crash.
    EXPECT_TRUE(hlr_->sent.empty());
}

TEST_F(ProxyTest, HlrInitiatedUnsupportedInvokeDropped) {
    // MAP Invoke for an operation not supported in the HLR-initiated direction.
    MapMessage invoke;
    invoke.transactionId = 0x20;
    invoke.component     = ComponentType::Invoke;
    invoke.invokeId      = 1;
    invoke.operation     = MapOperation::SendRoutingInfoForGprs;
    invoke.imsi          = "262019876543210";
    proxy_->handleMapPayload(map::encode(invoke));
    // No GSUP sent, no HLR tx stored.
    EXPECT_TRUE(sgsn_->sent.empty());
    EXPECT_EQ(proxy_->hlrTxSize(), 0u);
}

TEST_F(ProxyTest, HlrInitiatedFullRoundTrip) {
    MapMessage invoke;
    invoke.transactionId = 0x42;
    invoke.component     = ComponentType::Invoke;
    invoke.invokeId      = 1;
    invoke.operation     = MapOperation::InsertSubscriberData;
    invoke.imsi          = "001010000000001";
    proxy_->handleMapPayload(map::encode(invoke));

    ASSERT_EQ(sgsn_->sent.size(), 1u);
    EXPECT_EQ(proxy_->hlrTxSize(), 1u);

    // SGSN replies with InsertDataResult.
    proxy_->handleGsupPayload(makeInsertDataResultPayload("001010000000001"), 5);

    // Proxy should have sent MAP ReturnResult to HLR.
    ASSERT_EQ(hlr_->sent.size(), 1u);
    auto mapResp = map::decode(hlr_->sent[0].data);
    EXPECT_EQ(mapResp.component, ComponentType::ReturnResult);
    EXPECT_EQ(mapResp.transactionId, 0x42u);
    EXPECT_EQ(mapResp.invokeId, 1u);
    // HLR tx entry cleaned up.
    EXPECT_EQ(proxy_->hlrTxSize(), 0u);
}

TEST_F(ProxyTest, UpdateLocationReturnErrorConverted) {
    // Verify that ReturnError for UpdateGprsLocation is correctly converted
    // to a GSUP UpdateLocationError using the pendingOps_ recovery mechanism.
    gsup::GsupMessage req;
    req.type = gsup::MessageType::UpdateLocationRequest;
    req.imsi = "262019876543210";
    proxy_->handleGsupPayload(gsup::encode(req), 3);
    ASSERT_EQ(hlr_->sent.size(), 1u);

    auto mapMsg = map::decode(hlr_->sent[0].data);

    MapMessage err;
    err.transactionId = mapMsg.transactionId;
    err.component     = ComponentType::ReturnError;
    err.operation     = MapOperation::UpdateGprsLocation;
    err.imsi          = "262019876543210";
    err.errorCode     = 11;
    proxy_->handleMapPayload(map::encode(err));

    ASSERT_EQ(sgsn_->sent.size(), 1u);
    auto gsupResp = gsup::decode(sgsn_->sent[0].data);
    EXPECT_EQ(gsupResp.type, gsup::MessageType::UpdateLocationError);
    EXPECT_EQ(sgsn_->sent[0].clientId, 3u);
}

TEST_F(ProxyTest, HlrInitiatedPurgeMsFullRoundTrip) {
    // HLR sends PurgeMS Invoke → proxy forwards as GSUP PurgeMsRequest
    // → SGSN replies with PurgeMsResult → proxy sends MAP ReturnResult to HLR.
    MapMessage invoke;
    invoke.transactionId = 0x60;
    invoke.component     = ComponentType::Invoke;
    invoke.invokeId      = 1;
    invoke.operation     = MapOperation::PurgeMS;
    invoke.imsi          = "262019555555555";
    proxy_->handleMapPayload(map::encode(invoke));

    ASSERT_EQ(sgsn_->sent.size(), 1u);
    EXPECT_EQ(proxy_->hlrTxSize(), 1u);

    // Verify the GSUP message sent to SGSN
    auto gsupReq = gsup::decode(sgsn_->sent[0].data);
    EXPECT_EQ(gsupReq.type, gsup::MessageType::PurgeMsRequest);
    EXPECT_EQ(gsupReq.imsi, "262019555555555");

    // SGSN replies with PurgeMsResult
    gsup::GsupMessage result;
    result.type = gsup::MessageType::PurgeMsResult;
    result.imsi = "262019555555555";
    proxy_->handleGsupPayload(gsup::encode(result), 0);

    ASSERT_EQ(hlr_->sent.size(), 1u);
    auto mapResp = map::decode(hlr_->sent[0].data);
    EXPECT_EQ(mapResp.component, ComponentType::ReturnResult);
    EXPECT_EQ(mapResp.transactionId, 0x60u);
    EXPECT_EQ(mapResp.invokeId, 1u);
    EXPECT_EQ(proxy_->hlrTxSize(), 0u);
}

TEST_F(ProxyTest, HlrInitiatedPurgeMsErrorRoundTrip) {
    MapMessage invoke;
    invoke.transactionId = 0x70;
    invoke.component     = ComponentType::Invoke;
    invoke.invokeId      = 2;
    invoke.operation     = MapOperation::PurgeMS;
    invoke.imsi          = "262019666666666";
    proxy_->handleMapPayload(map::encode(invoke));
    ASSERT_EQ(sgsn_->sent.size(), 1u);

    gsup::GsupMessage err;
    err.type  = gsup::MessageType::PurgeMsError;
    err.imsi  = "262019666666666";
    err.cause = 7;
    proxy_->handleGsupPayload(gsup::encode(err), 0);

    ASSERT_EQ(hlr_->sent.size(), 1u);
    auto mapResp = map::decode(hlr_->sent[0].data);
    EXPECT_EQ(mapResp.component, ComponentType::ReturnError);
    EXPECT_EQ(mapResp.transactionId, 0x70u);
    EXPECT_EQ(proxy_->hlrTxSize(), 0u);
}

TEST_F(ProxyTest, HlrTxExpiryRemovesStaleEntries) {
    MapMessage invoke;
    invoke.transactionId = 0x50;
    invoke.component     = ComponentType::Invoke;
    invoke.invokeId      = 1;
    invoke.operation     = MapOperation::CancelLocation;
    invoke.imsi          = "262019999999999";
    proxy_->handleMapPayload(map::encode(invoke));

    EXPECT_EQ(proxy_->hlrTxSize(), 1u);

    // Expire with 0-second timeout — everything is immediately stale.
    proxy_->expireHlrTransactions(std::chrono::seconds(0));
    EXPECT_EQ(proxy_->hlrTxSize(), 0u);
}
