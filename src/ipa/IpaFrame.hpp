#pragma once
#include "common/Buffer.hpp"

// IPA (IP-based Abis) framing protocol used by Osmocom.
// Wire format:
//   [2 bytes big-endian payload length] [1 byte stream ID] [payload bytes]
// Stream IDs relevant to us:
//   0xEE  OSMO (GSUP lives here, identified by the GSUP sub-protocol byte)
//   0xFE  IPA CCM (ping/pong/identity)
//   0xFF  SCCP (not used by GSUP)

namespace proxy::ipa {

constexpr uint8_t kStreamGsup = 0xEE;
constexpr uint8_t kStreamCcm  = 0xFE;

// IPA CCM message types
constexpr uint8_t kCcmPing     = 0x00;
constexpr uint8_t kCcmPong     = 0x01;
constexpr uint8_t kCcmIdReq    = 0x04;
constexpr uint8_t kCcmIdResp   = 0x05;
constexpr uint8_t kCcmIdAck    = 0x06;

// Header size: 2 (length) + 1 (stream)
constexpr size_t kHeaderSize = 3;

struct IpaFrame {
    uint8_t streamId;
    Bytes   payload;
};

} // namespace proxy::ipa
