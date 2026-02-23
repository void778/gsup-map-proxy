#include <gtest/gtest.h>
#include "proxy/Converter.hpp"

using namespace proxy;
using namespace proxy::gsup;
using namespace proxy::map;

// ── gsupToMap ─────────────────────────────────────────────────────────────────

TEST(ConverterTest, SendAuthInfoRequestConversion) {
    GsupMessage gsup;
    gsup.type                 = MessageType::SendAuthInfoRequest;
    gsup.imsi                 = "262019876543210";
    gsup.numVectorsRequested  = 5;

    auto m = gsupToMap(gsup, 0x01, 0x01);
    EXPECT_EQ(m.operation, MapOperation::SendAuthenticationInfo);
    EXPECT_EQ(m.imsi, gsup.imsi);
    EXPECT_EQ(m.component, ComponentType::Invoke);
    ASSERT_TRUE(m.numRequestedVectors.has_value());
    EXPECT_EQ(*m.numRequestedVectors, 5u);
    EXPECT_EQ(m.transactionId, 0x01u);
    EXPECT_EQ(m.invokeId, 0x01u);
}

TEST(ConverterTest, SendAuthInfoRequestDefaultVectors) {
    GsupMessage gsup;
    gsup.type = MessageType::SendAuthInfoRequest;
    gsup.imsi = "001010000000001";
    // numVectorsRequested not set

    auto m = gsupToMap(gsup, 1, 1);
    ASSERT_TRUE(m.numRequestedVectors.has_value());
    EXPECT_EQ(*m.numRequestedVectors, 1u);
}

TEST(ConverterTest, UpdateLocationRequestConversion) {
    GsupMessage gsup;
    gsup.type = MessageType::UpdateLocationRequest;
    gsup.imsi = "262019876543210";

    auto m = gsupToMap(gsup, 2, 1);
    EXPECT_EQ(m.operation, MapOperation::UpdateGprsLocation);
    EXPECT_EQ(m.imsi, gsup.imsi);
    EXPECT_EQ(m.component, ComponentType::Invoke);
}

TEST(ConverterTest, LocationCancelRequestConversion) {
    GsupMessage gsup;
    gsup.type       = MessageType::LocationCancelRequest;
    gsup.imsi       = "262019876543210";
    gsup.cancelType = CancelType::UpdateProcedure;

    auto m = gsupToMap(gsup, 3, 1);
    EXPECT_EQ(m.operation, MapOperation::CancelLocation);
    ASSERT_TRUE(m.cancelType.has_value());
    EXPECT_EQ(*m.cancelType, static_cast<uint8_t>(CancelType::UpdateProcedure));
}

TEST(ConverterTest, LocationCancelWithdrawConversion) {
    GsupMessage gsup;
    gsup.type       = MessageType::LocationCancelRequest;
    gsup.imsi       = "262019876543210";
    gsup.cancelType = CancelType::Withdraw;

    auto m = gsupToMap(gsup, 4, 1);
    ASSERT_TRUE(m.cancelType.has_value());
    EXPECT_EQ(*m.cancelType, static_cast<uint8_t>(CancelType::Withdraw));
}

TEST(ConverterTest, InsertDataRequestConversion) {
    GsupMessage gsup;
    gsup.type   = MessageType::InsertDataRequest;
    gsup.imsi   = "262019876543210";
    gsup.msisdn = Bytes{0x91, 0x49, 0x06, 0x10};

    auto m = gsupToMap(gsup, 5, 1);
    EXPECT_EQ(m.operation, MapOperation::InsertSubscriberData);
    ASSERT_TRUE(m.msisdn.has_value());
    EXPECT_EQ(*m.msisdn, *gsup.msisdn);
}

TEST(ConverterTest, PurgeMsRequestConversion) {
    GsupMessage gsup;
    gsup.type = MessageType::PurgeMsRequest;
    gsup.imsi = "262019876543210";

    auto m = gsupToMap(gsup, 6, 1);
    EXPECT_EQ(m.operation, MapOperation::PurgeMS);
    EXPECT_EQ(m.imsi, gsup.imsi);
}

TEST(ConverterTest, UnsupportedGsupTypeThrows) {
    GsupMessage gsup;
    gsup.type = MessageType::SendAuthInfoError; // not a convertible type
    gsup.imsi = "262019876543210";
    EXPECT_THROW(gsupToMap(gsup, 1, 1), ConversionError);
}

// ── mapToGsup (responses) ─────────────────────────────────────────────────────

TEST(ConverterTest, SendAuthInfoResultConversion) {
    MapMessage m;
    m.component = ComponentType::ReturnResult;
    m.operation = MapOperation::SendAuthenticationInfo;
    m.imsi      = "262019876543210";

    MapAuthTriplet t;
    t.rand = Bytes(16, 0xAA);
    t.sres = Bytes(4,  0xBB);
    t.kc   = Bytes(8,  0xCC);
    m.authTriplets.push_back(t);

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::SendAuthInfoResult);
    EXPECT_EQ(gsup.imsi, m.imsi);
    ASSERT_EQ(gsup.authTuples.size(), 1u);
    EXPECT_EQ(gsup.authTuples[0].rand, t.rand);
    EXPECT_EQ(gsup.authTuples[0].sres, t.sres);
    EXPECT_EQ(gsup.authTuples[0].kc,   t.kc);
}

TEST(ConverterTest, SendAuthInfoResultWithQuintuplets) {
    MapMessage m;
    m.component = ComponentType::ReturnResult;
    m.operation = MapOperation::SendAuthenticationInfo;
    m.imsi      = "262019876543210";

    MapAuthQuintuplet q;
    q.rand = Bytes(16, 0x11);
    q.xres = Bytes(8,  0x22);
    q.ck   = Bytes(16, 0x33);
    q.ik   = Bytes(16, 0x44);
    q.autn = Bytes(16, 0x55);
    m.authQuintuplets.push_back(q);

    auto gsup = mapToGsup(m);
    ASSERT_EQ(gsup.authTuples.size(), 1u);
    ASSERT_TRUE(gsup.authTuples[0].ik.has_value());
    EXPECT_EQ(*gsup.authTuples[0].ik, q.ik);
    EXPECT_EQ(*gsup.authTuples[0].ck, q.ck);
    EXPECT_EQ(*gsup.authTuples[0].autn, q.autn);
    EXPECT_EQ(*gsup.authTuples[0].res, q.xres);
}

TEST(ConverterTest, UpdateGprsLocationResultConversion) {
    MapMessage m;
    m.component = ComponentType::ReturnResult;
    m.operation = MapOperation::UpdateGprsLocation;
    m.imsi      = "262019876543210";
    m.hlrNumber = Bytes{0x91, 0x49, 0x06, 0x01};

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::UpdateLocationResult);
    ASSERT_TRUE(gsup.hlrNumber.has_value());
    EXPECT_EQ(*gsup.hlrNumber, *m.hlrNumber);
}

TEST(ConverterTest, CancelLocationResultConversion) {
    MapMessage m;
    m.component = ComponentType::ReturnResult;
    m.operation = MapOperation::CancelLocation;
    m.imsi      = "262019876543210";

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::LocationCancelResult);
}

TEST(ConverterTest, InsertDataResultConversion) {
    MapMessage m;
    m.component = ComponentType::ReturnResult;
    m.operation = MapOperation::InsertSubscriberData;

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::InsertDataResult);
}

TEST(ConverterTest, PurgeMsResultConversionWithFreeze) {
    MapMessage m;
    m.component   = ComponentType::ReturnResult;
    m.operation   = MapOperation::PurgeMS;
    m.freezePtmsi = true;

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::PurgeMsResult);
    EXPECT_TRUE(gsup.freezePtmsi);
}

// ── mapToGsup (errors) ────────────────────────────────────────────────────────

TEST(ConverterTest, ReturnErrorSendAuthInfo) {
    MapMessage m;
    m.component = ComponentType::ReturnError;
    m.operation = MapOperation::SendAuthenticationInfo;
    m.errorCode = 34;

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::SendAuthInfoError);
    ASSERT_TRUE(gsup.cause.has_value());
    EXPECT_EQ(*gsup.cause, 34u);
}

TEST(ConverterTest, ReturnErrorUpdateLocation) {
    MapMessage m;
    m.component = ComponentType::ReturnError;
    m.operation = MapOperation::UpdateGprsLocation;
    m.errorCode = 1;

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::UpdateLocationError);
}

TEST(ConverterTest, ReturnErrorCancelLocation) {
    MapMessage m;
    m.component = ComponentType::ReturnError;
    m.operation = MapOperation::CancelLocation;

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::LocationCancelError);
}

TEST(ConverterTest, ReturnErrorPurgeMs) {
    MapMessage m;
    m.component = ComponentType::ReturnError;
    m.operation = MapOperation::PurgeMS;

    auto gsup = mapToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::PurgeMsError);
}

// ── Helper functions ──────────────────────────────────────────────────────────

TEST(ConverterTest, ExpectedGsupResult) {
    EXPECT_EQ(expectedGsupResult(MessageType::SendAuthInfoRequest),
              MessageType::SendAuthInfoResult);
    EXPECT_EQ(expectedGsupResult(MessageType::UpdateLocationRequest),
              MessageType::UpdateLocationResult);
    EXPECT_EQ(expectedGsupResult(MessageType::LocationCancelRequest),
              MessageType::LocationCancelResult);
    EXPECT_EQ(expectedGsupResult(MessageType::InsertDataRequest),
              MessageType::InsertDataResult);
    EXPECT_EQ(expectedGsupResult(MessageType::PurgeMsRequest),
              MessageType::PurgeMsResult);
}

TEST(ConverterTest, ExpectedGsupError) {
    EXPECT_EQ(expectedGsupError(MessageType::SendAuthInfoRequest),
              MessageType::SendAuthInfoError);
    EXPECT_EQ(expectedGsupError(MessageType::UpdateLocationRequest),
              MessageType::UpdateLocationError);
    EXPECT_EQ(expectedGsupError(MessageType::PurgeMsRequest),
              MessageType::PurgeMsError);
}

TEST(ConverterTest, ExpectedGsupResultInvalidThrows) {
    EXPECT_THROW(expectedGsupResult(MessageType::SendAuthInfoError), ConversionError);
}

TEST(ConverterTest, ExpectedGsupErrorInvalidThrows) {
    EXPECT_THROW(expectedGsupError(MessageType::UpdateLocationError), ConversionError);
}

// ── mapInvokeToGsup (HLR-initiated requests) ──────────────────────────────────

TEST(ConverterTest, MapInvokeInsertSubscriberData) {
    MapMessage m;
    m.component  = ComponentType::Invoke;
    m.operation  = MapOperation::InsertSubscriberData;
    m.imsi       = "262019876543210";
    m.msisdn     = Bytes{0x91, 0x49, 0x06, 0x10};

    auto gsup = mapInvokeToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::InsertDataRequest);
    EXPECT_EQ(gsup.imsi, m.imsi);
    ASSERT_TRUE(gsup.msisdn.has_value());
    EXPECT_EQ(*gsup.msisdn, *m.msisdn);
}

TEST(ConverterTest, MapInvokeInsertSubscriberDataNoMsisdn) {
    MapMessage m;
    m.component = ComponentType::Invoke;
    m.operation = MapOperation::InsertSubscriberData;
    m.imsi      = "001010000000001";
    // msisdn not set

    auto gsup = mapInvokeToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::InsertDataRequest);
    EXPECT_FALSE(gsup.msisdn.has_value());
}

TEST(ConverterTest, MapInvokeCancelLocationUpdateProcedure) {
    MapMessage m;
    m.component  = ComponentType::Invoke;
    m.operation  = MapOperation::CancelLocation;
    m.imsi       = "262019876543210";
    m.cancelType = static_cast<uint8_t>(CancelType::UpdateProcedure);

    auto gsup = mapInvokeToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::LocationCancelRequest);
    EXPECT_EQ(gsup.imsi, m.imsi);
    ASSERT_TRUE(gsup.cancelType.has_value());
    EXPECT_EQ(*gsup.cancelType, CancelType::UpdateProcedure);
}

TEST(ConverterTest, MapInvokeCancelLocationWithdraw) {
    MapMessage m;
    m.component  = ComponentType::Invoke;
    m.operation  = MapOperation::CancelLocation;
    m.imsi       = "262019876543210";
    m.cancelType = static_cast<uint8_t>(CancelType::Withdraw);

    auto gsup = mapInvokeToGsup(m);
    ASSERT_TRUE(gsup.cancelType.has_value());
    EXPECT_EQ(*gsup.cancelType, CancelType::Withdraw);
}

TEST(ConverterTest, MapInvokeUnsupportedOperationThrows) {
    MapMessage m;
    m.component = ComponentType::Invoke;
    m.operation = MapOperation::SendRoutingInfoForGprs; // not in HLR-initiated direction
    m.imsi      = "262019876543210";
    EXPECT_THROW(mapInvokeToGsup(m), ConversionError);
}

TEST(ConverterTest, MapInvokeNotInvokeComponentThrows) {
    MapMessage m;
    m.component = ComponentType::ReturnResult; // wrong direction
    m.operation = MapOperation::InsertSubscriberData;
    m.imsi      = "262019876543210";
    EXPECT_THROW(mapInvokeToGsup(m), ConversionError);
}

// ── gsupToMapResult (HLR-initiated responses) ─────────────────────────────────

TEST(ConverterTest, GsupInsertDataResultToMapReturnResult) {
    GsupMessage gsup;
    gsup.type = MessageType::InsertDataResult;
    gsup.imsi = "262019876543210";

    auto m = gsupToMapResult(gsup, 0xAB, 0x02);
    EXPECT_EQ(m.component, ComponentType::ReturnResult);
    EXPECT_EQ(m.operation, MapOperation::InsertSubscriberData);
    EXPECT_EQ(m.transactionId, 0xABu);
    EXPECT_EQ(m.invokeId, 0x02u);
    EXPECT_EQ(m.imsi, gsup.imsi);
    EXPECT_TRUE(m.isLastComponent);
    EXPECT_FALSE(m.errorCode.has_value());
}

TEST(ConverterTest, GsupInsertDataErrorToMapReturnError) {
    GsupMessage gsup;
    gsup.type  = MessageType::InsertDataError;
    gsup.imsi  = "262019876543210";
    gsup.cause = 13;

    auto m = gsupToMapResult(gsup, 0xCD, 0x03);
    EXPECT_EQ(m.component, ComponentType::ReturnError);
    EXPECT_EQ(m.operation, MapOperation::InsertSubscriberData);
    EXPECT_EQ(m.transactionId, 0xCDu);
    ASSERT_TRUE(m.errorCode.has_value());
    EXPECT_EQ(*m.errorCode, 13u);
}

TEST(ConverterTest, GsupLocationCancelResultToMapReturnResult) {
    GsupMessage gsup;
    gsup.type = MessageType::LocationCancelResult;
    gsup.imsi = "262019876543210";

    auto m = gsupToMapResult(gsup, 0x11, 0x01);
    EXPECT_EQ(m.component, ComponentType::ReturnResult);
    EXPECT_EQ(m.operation, MapOperation::CancelLocation);
    EXPECT_EQ(m.transactionId, 0x11u);
}

TEST(ConverterTest, GsupLocationCancelErrorToMapReturnError) {
    GsupMessage gsup;
    gsup.type  = MessageType::LocationCancelError;
    gsup.imsi  = "262019876543210";
    gsup.cause = 7;

    auto m = gsupToMapResult(gsup, 0x22, 0x01);
    EXPECT_EQ(m.component, ComponentType::ReturnError);
    EXPECT_EQ(m.operation, MapOperation::CancelLocation);
    ASSERT_TRUE(m.errorCode.has_value());
    EXPECT_EQ(*m.errorCode, 7u);
}

TEST(ConverterTest, GsupToMapResultInvalidTypeThrows) {
    GsupMessage gsup;
    gsup.type = MessageType::SendAuthInfoResult; // not an HLR-initiated type
    gsup.imsi = "262019876543210";
    EXPECT_THROW(gsupToMapResult(gsup, 1, 1), ConversionError);
}

// ── isHlrInitiatedGsupType ────────────────────────────────────────────────────

TEST(ConverterTest, IsHlrInitiatedGsupType) {
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::InsertDataResult));
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::InsertDataError));
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::LocationCancelResult));
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::LocationCancelError));
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::DeleteDataResult));
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::DeleteDataError));
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::PurgeMsResult));
    EXPECT_TRUE(isHlrInitiatedGsupType(MessageType::PurgeMsError));

    EXPECT_FALSE(isHlrInitiatedGsupType(MessageType::SendAuthInfoResult));
    EXPECT_FALSE(isHlrInitiatedGsupType(MessageType::UpdateLocationResult));
    EXPECT_FALSE(isHlrInitiatedGsupType(MessageType::InsertDataRequest));
}

// ── DeleteSubscriberData (HLR-initiated) ─────────────────────────────────────

TEST(ConverterTest, MapInvokeDeleteSubscriberData) {
    MapMessage m;
    m.component = ComponentType::Invoke;
    m.operation = MapOperation::DeleteSubscriberData;
    m.imsi      = "262019876543210";

    auto gsup = mapInvokeToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::DeleteDataRequest);
    EXPECT_EQ(gsup.imsi, m.imsi);
}

TEST(ConverterTest, GsupDeleteDataResultToMapReturnResult) {
    GsupMessage gsup;
    gsup.type = MessageType::DeleteDataResult;
    gsup.imsi = "262019876543210";

    auto m = gsupToMapResult(gsup, 0x33, 0x05);
    EXPECT_EQ(m.component, ComponentType::ReturnResult);
    EXPECT_EQ(m.operation, MapOperation::DeleteSubscriberData);
    EXPECT_EQ(m.transactionId, 0x33u);
    EXPECT_EQ(m.invokeId, 0x05u);
}

TEST(ConverterTest, GsupDeleteDataErrorToMapReturnError) {
    GsupMessage gsup;
    gsup.type  = MessageType::DeleteDataError;
    gsup.imsi  = "262019876543210";
    gsup.cause = 5;

    auto m = gsupToMapResult(gsup, 0x44, 0x06);
    EXPECT_EQ(m.component, ComponentType::ReturnError);
    EXPECT_EQ(m.operation, MapOperation::DeleteSubscriberData);
    ASSERT_TRUE(m.errorCode.has_value());
    EXPECT_EQ(*m.errorCode, 5u);
}

// ── PurgeMS HLR-initiated ─────────────────────────────────────────────────────

TEST(ConverterTest, MapInvokePurgeMs) {
    MapMessage m;
    m.component = ComponentType::Invoke;
    m.operation = MapOperation::PurgeMS;
    m.imsi      = "262019876543210";

    auto gsup = mapInvokeToGsup(m);
    EXPECT_EQ(gsup.type, MessageType::PurgeMsRequest);
    EXPECT_EQ(gsup.imsi, m.imsi);
}

TEST(ConverterTest, GsupPurgeMsResultToMapReturnResult) {
    GsupMessage gsup;
    gsup.type = MessageType::PurgeMsResult;
    gsup.imsi = "262019876543210";

    auto m = gsupToMapResult(gsup, 0x55, 0x07);
    EXPECT_EQ(m.component, ComponentType::ReturnResult);
    EXPECT_EQ(m.operation, MapOperation::PurgeMS);
    EXPECT_EQ(m.transactionId, 0x55u);
    EXPECT_EQ(m.invokeId, 0x07u);
    EXPECT_TRUE(m.isLastComponent);
    EXPECT_FALSE(m.errorCode.has_value());
}

TEST(ConverterTest, GsupPurgeMsErrorToMapReturnError) {
    GsupMessage gsup;
    gsup.type  = MessageType::PurgeMsError;
    gsup.imsi  = "262019876543210";
    gsup.cause = 9;

    auto m = gsupToMapResult(gsup, 0x66, 0x08);
    EXPECT_EQ(m.component, ComponentType::ReturnError);
    EXPECT_EQ(m.operation, MapOperation::PurgeMS);
    ASSERT_TRUE(m.errorCode.has_value());
    EXPECT_EQ(*m.errorCode, 9u);
}
