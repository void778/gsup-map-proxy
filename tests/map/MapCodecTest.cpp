#include <gtest/gtest.h>
#include "map/MapCodec.hpp"

using namespace proxy;
using namespace proxy::map;

// Helper: build a basic MapMessage with required fields
static MapMessage makeSendAuthInfoReq(const std::string& imsi = "262019876543210") {
    MapMessage m;
    m.transactionId       = 0x00000001;
    m.component           = ComponentType::Invoke;
    m.invokeId            = 1;
    m.operation           = MapOperation::SendAuthenticationInfo;
    m.imsi                = imsi;
    m.numRequestedVectors = 3;
    return m;
}

// ── SendAuthenticationInfo ────────────────────────────────────────────────────

TEST(MapCodecTest, SendAuthInfoRequestRoundTrip) {
    auto orig = makeSendAuthInfoReq();
    auto decoded = decode(encode(orig));

    EXPECT_EQ(decoded.transactionId, orig.transactionId);
    EXPECT_EQ(decoded.component, ComponentType::Invoke);
    EXPECT_EQ(decoded.invokeId, orig.invokeId);
    EXPECT_EQ(decoded.operation, MapOperation::SendAuthenticationInfo);
    EXPECT_EQ(decoded.imsi, orig.imsi);
    ASSERT_TRUE(decoded.numRequestedVectors.has_value());
    EXPECT_EQ(*decoded.numRequestedVectors, 3);
}

TEST(MapCodecTest, SendAuthInfoResponseWithTriplets) {
    MapMessage resp;
    resp.transactionId = 0x00000001;
    resp.component     = ComponentType::ReturnResult;
    resp.invokeId      = 1;
    resp.operation     = MapOperation::SendAuthenticationInfo;
    resp.imsi          = "262019876543210";

    MapAuthTriplet t;
    t.rand = Bytes(16, 0xAA);
    t.sres = Bytes(4,  0xBB);
    t.kc   = Bytes(8,  0xCC);
    resp.authTriplets.push_back(t);

    auto decoded = decode(encode(resp));
    EXPECT_EQ(decoded.component, ComponentType::ReturnResult);
    EXPECT_EQ(decoded.operation, MapOperation::SendAuthenticationInfo);
    ASSERT_EQ(decoded.authTriplets.size(), 1u);
    EXPECT_EQ(decoded.authTriplets[0].rand, t.rand);
    EXPECT_EQ(decoded.authTriplets[0].sres, t.sres);
    EXPECT_EQ(decoded.authTriplets[0].kc,   t.kc);
}

TEST(MapCodecTest, SendAuthInfoResponseWithQuintuplets) {
    MapMessage resp;
    resp.transactionId = 0x00000002;
    resp.component     = ComponentType::ReturnResult;
    resp.invokeId      = 1;
    resp.operation     = MapOperation::SendAuthenticationInfo;

    MapAuthQuintuplet q;
    q.rand = Bytes(16, 0x11);
    q.xres = Bytes(8,  0x22);
    q.ck   = Bytes(16, 0x33);
    q.ik   = Bytes(16, 0x44);
    q.autn = Bytes(16, 0x55);
    resp.authQuintuplets.push_back(q);

    auto decoded = decode(encode(resp));
    ASSERT_EQ(decoded.authQuintuplets.size(), 1u);
    EXPECT_EQ(decoded.authQuintuplets[0].rand, q.rand);
    EXPECT_EQ(decoded.authQuintuplets[0].xres, q.xres);
    EXPECT_EQ(decoded.authQuintuplets[0].ck,   q.ck);
    EXPECT_EQ(decoded.authQuintuplets[0].ik,   q.ik);
    EXPECT_EQ(decoded.authQuintuplets[0].autn, q.autn);
}

TEST(MapCodecTest, SendAuthInfoMultipleTriplets) {
    MapMessage resp;
    resp.transactionId = 0x00000003;
    resp.component     = ComponentType::ReturnResult;
    resp.invokeId      = 1;
    resp.operation     = MapOperation::SendAuthenticationInfo;
    for (int i = 0; i < 5; ++i) {
        MapAuthTriplet t;
        t.rand = Bytes(16, static_cast<uint8_t>(i));
        t.sres = Bytes(4,  static_cast<uint8_t>(i + 0x10));
        t.kc   = Bytes(8,  static_cast<uint8_t>(i + 0x20));
        resp.authTriplets.push_back(t);
    }

    auto decoded = decode(encode(resp));
    ASSERT_EQ(decoded.authTriplets.size(), 5u);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(decoded.authTriplets[i].rand, Bytes(16, static_cast<uint8_t>(i)));
}

// ── UpdateGprsLocation ────────────────────────────────────────────────────────

TEST(MapCodecTest, UpdateGprsLocationRequest) {
    MapMessage m;
    m.transactionId = 0x0000000A;
    m.component     = ComponentType::Invoke;
    m.invokeId      = 2;
    m.operation     = MapOperation::UpdateGprsLocation;
    m.imsi          = "262019876543210";
    m.sgsnNumber    = Bytes{0x91, 0x49, 0x06, 0x01, 0x90, 0x00, 0xF8};
    m.sgsnAddress   = Bytes{192, 168, 1, 1}; // IPv4

    auto decoded = decode(encode(m));
    EXPECT_EQ(decoded.operation, MapOperation::UpdateGprsLocation);
    EXPECT_EQ(decoded.imsi, m.imsi);
    ASSERT_TRUE(decoded.sgsnNumber.has_value());
    EXPECT_EQ(*decoded.sgsnNumber, *m.sgsnNumber);
    ASSERT_TRUE(decoded.sgsnAddress.has_value());
    EXPECT_EQ(*decoded.sgsnAddress, *m.sgsnAddress);
}

TEST(MapCodecTest, UpdateGprsLocationResponse) {
    MapMessage m;
    m.transactionId = 0x0000000A;
    m.component     = ComponentType::ReturnResult;
    m.invokeId      = 2;
    m.operation     = MapOperation::UpdateGprsLocation;
    m.hlrNumber     = Bytes{0x91, 0x49, 0x06, 0x01, 0x90, 0x00, 0xF8};

    auto decoded = decode(encode(m));
    EXPECT_EQ(decoded.component, ComponentType::ReturnResult);
    ASSERT_TRUE(decoded.hlrNumber.has_value());
    EXPECT_EQ(*decoded.hlrNumber, *m.hlrNumber);
}

// ── CancelLocation ────────────────────────────────────────────────────────────

TEST(MapCodecTest, CancelLocationRequest) {
    MapMessage m;
    m.transactionId = 0x0000000B;
    m.component     = ComponentType::Invoke;
    m.invokeId      = 3;
    m.operation     = MapOperation::CancelLocation;
    m.imsi          = "262019876543210";
    m.cancelType    = 0; // updateProcedure

    auto decoded = decode(encode(m));
    EXPECT_EQ(decoded.operation, MapOperation::CancelLocation);
    EXPECT_EQ(decoded.imsi, m.imsi);
    ASSERT_TRUE(decoded.cancelType.has_value());
    EXPECT_EQ(*decoded.cancelType, 0u);
}

// ── InsertSubscriberData ──────────────────────────────────────────────────────

TEST(MapCodecTest, InsertSubscriberDataRequest) {
    MapMessage m;
    m.transactionId = 0x0000000C;
    m.component     = ComponentType::Invoke;
    m.invokeId      = 4;
    m.operation     = MapOperation::InsertSubscriberData;
    m.imsi          = "262019876543210";
    m.msisdn        = Bytes{0x91, 0x49, 0x06, 0x10, 0x02, 0x30, 0xF0};

    auto decoded = decode(encode(m));
    EXPECT_EQ(decoded.operation, MapOperation::InsertSubscriberData);
    EXPECT_EQ(decoded.imsi, m.imsi);
    ASSERT_TRUE(decoded.msisdn.has_value());
    EXPECT_EQ(*decoded.msisdn, *m.msisdn);
}

// ── PurgeMS ───────────────────────────────────────────────────────────────────

TEST(MapCodecTest, PurgeMsRequest) {
    MapMessage m;
    m.transactionId = 0x0000000D;
    m.component     = ComponentType::Invoke;
    m.invokeId      = 5;
    m.operation     = MapOperation::PurgeMS;
    m.imsi          = "262019876543210";
    m.sgsnNumber    = Bytes{0x91, 0x49, 0x06, 0x01, 0x90, 0x00, 0xF8};

    auto decoded = decode(encode(m));
    EXPECT_EQ(decoded.operation, MapOperation::PurgeMS);
    EXPECT_EQ(decoded.imsi, m.imsi);
}

TEST(MapCodecTest, PurgeMsResponseWithFreezePtmsi) {
    MapMessage m;
    m.transactionId = 0x0000000D;
    m.component     = ComponentType::ReturnResult;
    m.invokeId      = 5;
    m.operation     = MapOperation::PurgeMS;
    m.freezePtmsi   = true;

    auto decoded = decode(encode(m));
    EXPECT_TRUE(decoded.freezePtmsi);
}

// ── ReturnError ───────────────────────────────────────────────────────────────

TEST(MapCodecTest, ReturnError) {
    MapMessage m;
    m.transactionId = 0x0000000E;
    m.component     = ComponentType::ReturnError;
    m.invokeId      = 6;
    m.operation     = MapOperation::SendAuthenticationInfo;
    m.errorCode     = 34; // systemFailure

    auto decoded = decode(encode(m));
    EXPECT_EQ(decoded.component, ComponentType::ReturnError);
    ASSERT_TRUE(decoded.errorCode.has_value());
    EXPECT_EQ(*decoded.errorCode, 34u);
}

// ── Transaction ID propagation ────────────────────────────────────────────────

TEST(MapCodecTest, TransactionIdIsPreserved) {
    auto m = makeSendAuthInfoReq();
    m.transactionId = 0xDEADBEEF;
    auto decoded = decode(encode(m));
    EXPECT_EQ(decoded.transactionId, 0xDEADBEEFu);
}
