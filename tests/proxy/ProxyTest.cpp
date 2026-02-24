#include <gtest/gtest.h>
#include <map>
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

TEST_F(ProxyTest, HlrDisconnectNacksAllPendingTransactions) {
    // Queue two pending SGSN-initiated requests
    gsup::GsupMessage req1;
    req1.type = gsup::MessageType::SendAuthInfoRequest;
    req1.imsi = "262019111111111";
    req1.numVectorsRequested = 1;
    proxy_->handleGsupPayload(gsup::encode(req1), /*clientId=*/7);

    gsup::GsupMessage req2;
    req2.type = gsup::MessageType::UpdateLocationRequest;
    req2.imsi = "262019222222222";
    proxy_->handleGsupPayload(gsup::encode(req2), /*clientId=*/8);

    ASSERT_EQ(hlr_->sent.size(), 2u);
    ASSERT_EQ(sgsn_->sent.size(), 0u);

    // Simulate HLR disconnect — NAK all pending transactions
    proxy_->nackAllPendingTransactions();

    // Both SGSN clients should get errors
    ASSERT_EQ(sgsn_->sent.size(), 2u);

    // Collect errors by clientId
    std::map<ClientId, gsup::MessageType> errors;
    for (auto& s : sgsn_->sent)
        errors[s.clientId] = gsup::decode(s.data).type;

    EXPECT_EQ(errors[7], gsup::MessageType::SendAuthInfoError);
    EXPECT_EQ(errors[8], gsup::MessageType::UpdateLocationError);

    // After nack, no pending transactions remain
    EXPECT_EQ(proxy_->transactions().size(), 0u);
}

TEST_F(ProxyTest, ExpireSgsnTransactionsSendsGsupError) {
    // Create a proxy with a zero timeout so transactions expire immediately.
    auto sgsn = std::make_shared<MockTransport>();
    auto hlr  = std::make_shared<MockTransport>();
    Proxy shortProxy(sgsn, hlr, std::chrono::seconds{0});
    shortProxy.start();

    gsup::GsupMessage req;
    req.type = gsup::MessageType::SendAuthInfoRequest;
    req.imsi = "262019444444444";
    req.numVectorsRequested = 1;
    shortProxy.handleGsupPayload(gsup::encode(req), /*clientId=*/5);
    ASSERT_EQ(shortProxy.transactions().size(), 1u);

    // All transactions are instantly stale with 0s timeout.
    shortProxy.expireSgsnTransactions();

    ASSERT_EQ(sgsn->sent.size(), 1u);
    auto gsupResp = gsup::decode(sgsn->sent[0].data);
    EXPECT_EQ(gsupResp.type, gsup::MessageType::SendAuthInfoError);
    EXPECT_EQ(sgsn->sent[0].clientId, 5u);
    EXPECT_EQ(shortProxy.transactions().size(), 0u);
}

TEST_F(ProxyTest, ExpireSgsnTransactionsWithNoExpiredDoesNothing) {
    gsup::GsupMessage req;
    req.type = gsup::MessageType::SendAuthInfoRequest;
    req.imsi = "262019333333333";
    req.numVectorsRequested = 1;
    proxy_->handleGsupPayload(gsup::encode(req), /*clientId=*/9);
    ASSERT_EQ(proxy_->transactions().size(), 1u);

    // Default timeout is 30s — nothing should expire
    proxy_->expireSgsnTransactions();
    EXPECT_EQ(sgsn_->sent.size(), 0u);
    EXPECT_EQ(proxy_->transactions().size(), 1u);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

// nackAllPendingTransactions with zero pending transactions must be a no-op.
TEST_F(ProxyTest, NackAllWithNoPendingIsNoop) {
    EXPECT_EQ(proxy_->transactions().size(), 0u);
    EXPECT_NO_THROW(proxy_->nackAllPendingTransactions());
    EXPECT_TRUE(sgsn_->sent.empty());
}

// expireSgsnTransactions with zero pending transactions must be a no-op.
TEST_F(ProxyTest, ExpireWithNoPendingIsNoop) {
    auto sgsn = std::make_shared<MockTransport>();
    auto hlr  = std::make_shared<MockTransport>();
    Proxy shortProxy(sgsn, hlr, std::chrono::seconds{0});
    shortProxy.start();
    EXPECT_NO_THROW(shortProxy.expireSgsnTransactions());
    EXPECT_TRUE(sgsn->sent.empty());
}

// Two different SGSNs may have transactions in flight simultaneously; both
// must be correctly NAKed when the HLR disconnects.
TEST_F(ProxyTest, NackAllWithMultipleClientIdsSendsToEach) {
    gsup::GsupMessage req1;
    req1.type = gsup::MessageType::SendAuthInfoRequest;
    req1.imsi = "262019111111111";
    req1.numVectorsRequested = 1;
    proxy_->handleGsupPayload(gsup::encode(req1), 3);

    gsup::GsupMessage req2;
    req2.type = gsup::MessageType::UpdateLocationRequest;
    req2.imsi = "262019222222222";
    proxy_->handleGsupPayload(gsup::encode(req2), 4);

    gsup::GsupMessage req3;
    req3.type = gsup::MessageType::SendAuthInfoRequest;
    req3.imsi = "262019333333333";
    req3.numVectorsRequested = 1;
    proxy_->handleGsupPayload(gsup::encode(req3), 3); // same client as req1

    proxy_->nackAllPendingTransactions();

    EXPECT_EQ(sgsn_->sent.size(), 3u);
    EXPECT_EQ(proxy_->transactions().size(), 0u);
}

// MAP ReturnError arriving for a TID not in pendingOps_ (operation Unknown)
// must be dropped gracefully without crashing.
TEST_F(ProxyTest, ReturnErrorWithNoRecoveryDropsGracefully) {
    // Send a request so a TID is allocated.
    proxy_->handleGsupPayload(makeSendAuthInfoReq("262019876543210"), 1);
    auto mapMsg = map::decode(hlr_->sent[0].data);

    // Manually erase the pendingOps_ entry to simulate no recovery info.
    // We can't access pendingOps_ directly — instead send a ReturnError
    // with operation=Unknown, which would fail conversion.
    // The proxy recovers op from pendingOps_; if we first complete via a
    // ReturnResult then send a second ReturnError for the same (now gone) TID,
    // it should be dropped (no pending transaction).
    MapMessage result;
    result.transactionId = mapMsg.transactionId;
    result.component     = ComponentType::ReturnResult;
    result.operation     = MapOperation::SendAuthenticationInfo;
    result.imsi          = "262019876543210";
    MapAuthTriplet t;
    t.rand = Bytes(16, 0); t.sres = Bytes(4, 0); t.kc = Bytes(8, 0);
    result.authTriplets.push_back(t);
    proxy_->handleMapPayload(map::encode(result)); // completes the tx

    // Now send ReturnError for the same (already completed) TID.
    MapMessage err;
    err.transactionId = mapMsg.transactionId;
    err.component     = ComponentType::ReturnError;
    err.operation     = MapOperation::SendAuthenticationInfo;
    err.imsi          = "262019876543210";
    err.errorCode     = 5;
    proxy_->handleMapPayload(map::encode(err));

    // Only one response (the ReturnResult) should have been sent.
    EXPECT_EQ(sgsn_->sent.size(), 1u);
    EXPECT_EQ(proxy_->transactions().size(), 0u);
}

// Expiry of multiple transactions sends an error for each, to the correct client.
TEST_F(ProxyTest, ExpireMultipleTransactionsSendsErrorsToClients) {
    auto sgsn = std::make_shared<MockTransport>();
    auto hlr  = std::make_shared<MockTransport>();
    Proxy shortProxy(sgsn, hlr, std::chrono::seconds{0});
    shortProxy.start();

    gsup::GsupMessage req1;
    req1.type = gsup::MessageType::SendAuthInfoRequest;
    req1.imsi = "262019000000001";
    req1.numVectorsRequested = 1;
    shortProxy.handleGsupPayload(gsup::encode(req1), /*clientId=*/10);

    gsup::GsupMessage req2;
    req2.type = gsup::MessageType::UpdateLocationRequest;
    req2.imsi = "262019000000002";
    shortProxy.handleGsupPayload(gsup::encode(req2), /*clientId=*/20);

    shortProxy.expireSgsnTransactions();

    ASSERT_EQ(sgsn->sent.size(), 2u);
    // Each client gets the error for its own request type.
    std::map<ClientId, gsup::MessageType> errors;
    for (auto& s : sgsn->sent)
        errors[s.clientId] = gsup::decode(s.data).type;
    EXPECT_EQ(errors[10], gsup::MessageType::SendAuthInfoError);
    EXPECT_EQ(errors[20], gsup::MessageType::UpdateLocationError);
}
