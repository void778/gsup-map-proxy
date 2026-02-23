# gsup-map-proxy

A C++17 proxy that bridges **osmoSGSN** (speaking GSUP over IPA) to a real
**HLR** (speaking MAP/TCAP/SCCP over M3UA/SCTP or TCP).

```
osmoSGSN ──IPA/TCP──► gsup-map-proxy ──M3UA/SCCP/MAP──► HLR
         ◄───────────                 ◄──────────────────
```

---

## Table of Contents

1. [Background](#background)
2. [Architecture](#architecture)
3. [Protocol Stack](#protocol-stack)
4. [Component Design](#component-design)
5. [Message Flows](#message-flows)
6. [Supported Operations](#supported-operations)
7. [Building](#building)
8. [Running](#running)
9. [Configuration Reference](#configuration-reference)
10. [Testing](#testing)
11. [Code Coverage](#code-coverage)

---

## Background

[OsmoSGSN](https://osmocom.org/projects/osmo-sgsn) speaks **GSUP**
(Generic Subscriber Update Protocol), an Osmocom-defined binary protocol
that carries MAP-equivalent messages over a simple IPA TCP connection.
Commercial HLRs speak **MAP** encoded in TCAP, transported over SCCP and
M3UA.

This proxy translates between the two protocol worlds in real time, allowing
an osmoSGSN (or any GSUP client) to authenticate subscribers, update
locations, and process other operations against a standard SS7 HLR without
requiring a full MAP/TCAP stack on the SGSN side.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         gsup-map-proxy                           │
│                                                                  │
│  ┌───────────┐    ┌──────────────────────────────┐    ┌───────┐ │
│  │ IpaServer │    │           Proxy               │    │  Map  │ │
│  │ (IPA/TCP  │◄──►│  ┌──────────────────────┐   │◄──►│Trans- │ │
│  │ listener) │    │  │  TransactionManager  │   │    │ port  │ │
│  └───────────┘    │  │  (TID ↔ clientId)    │   │    │(M3UA/ │ │
│                   │  └──────────────────────┘   │    │SCCP)  │ │
│                   │  ┌──────────────────────┐   │    └───────┘ │
│                   │  │     Converter        │   │              │
│                   │  │  gsupToMap()         │   │              │
│                   │  │  mapToGsup()         │   │              │
│                   │  │  mapInvokeToGsup()   │   │              │
│                   │  │  gsupToMapResult()   │   │              │
│                   │  └──────────────────────┘   │              │
│                   └──────────────────────────────┘              │
└──────────────────────────────────────────────────────────────────┘
```

The architecture separates concerns into three layers:

| Layer | Components | Responsibility |
|---|---|---|
| **Transport** | `IpaServer`, `IpaSession`, `MapTransport` | Wire format, framing, connections |
| **Proxy core** | `Proxy`, `TransactionManager` | Message routing, transaction lifecycle |
| **Protocol** | `Converter`, codecs | Encode/decode, GSUP↔MAP translation |

---

## Protocol Stack

### SGSN side (IPA)

```
┌──────────────────────┐
│   GSUP payload       │  Application: subscriber operations
├──────────────────────┤
│   IPA framing        │  2-byte length + 1-byte stream ID (0xEE = GSUP)
├──────────────────────┤
│   TCP                │  Reliable stream
└──────────────────────┘
```

**IPA connection lifecycle:**
1. TCP connect from SGSN
2. Server sends `CCM_ID_REQ`
3. Client replies `CCM_ID_RESP` (with unit ID)
4. Server acknowledges with `CCM_ID_ACK`
5. GSUP messages flow freely; keepalive via `CCM_PING`/`CCM_PONG`

### HLR side (MAP/TCAP/SCCP/M3UA)

```
┌──────────────────────┐
│   MAP operations     │  Application: SendAuthInfo, UpdateGprsLocation, …
├──────────────────────┤
│   TCAP               │  Transaction/component model (Begin/End, Invoke/ReturnResult)
├──────────────────────┤
│   SCCP UDT           │  Connectionless transfer, Global Title addressing
├──────────────────────┤
│   M3UA DATA          │  MTP3 user adaptation (RFC 4666)
├──────────────────────┤
│   TCP or SCTP        │  Transport to Signalling Gateway
└──────────────────────┘
```

**M3UA session lifecycle:**
1. TCP/SCTP connect to Signalling Gateway
2. Send `ASPUP` → receive `ASPUP_ACK`
3. Send `ASPAC` → receive `ASPAC_ACK` (now ACTIVE)
4. Exchange `HEARTBEAT`/`HEARTBEAT_ACK` periodically
5. MAP PDUs flow inside M3UA `DATA` messages

---

## Component Design

### IpaServer & IpaSession

`IpaServer` is a multi-session TCP acceptor implementing `ITransport`.
Each accepted connection gets its own `IpaSession` identified by a `ClientId`
(monotonically increasing `uint64_t`).

```
IpaServer (ITransport)
├── acceptNext()               loop — accept → create IpaSession
├── send(data, clientId=0)     0 = broadcast; non-zero = targeted
├── onMessage(cb)              cb(payload, clientId) per GSUP frame received
└── sessions_                  map<ClientId, IpaSession>

IpaSession
├── start(onReady, onMsg, onDisconnect)
├── Performs CCM handshake (ID_REQ → ID_RESP → ID_ACK)
├── sendGsup(payload)          wraps in IPA frame and async_writes
└── Handles PING → PONG keepalive
```

### MapTransport

`MapTransport` implements `ITransport` for the HLR-facing side. It maintains
a single TCP (or SCTP) connection to the Signalling Gateway and implements
the M3UA state machine.

```
State machine:
  DISCONNECTED ──connect──► CONNECTING
       ▲                        │ onConnect OK
       │ error/disconnect        ▼
  reconnectTimer           ASPSM: send ASPUP
                                │ ASPUP_ACK
                                ▼
                           ASPTM: send ASPAC
                                │ ASPAC_ACK
                                ▼
                             ACTIVE ──── heartbeat timer
```

SCTP support is available when built with `-DENABLE_SCTP_TRANSPORT=ON`.
The socket type is `asio::generic::stream_protocol::socket`, so the same
code path handles both TCP and SCTP; the protocol is selected at runtime
via `MapTransportConfig::useSCTP`.

### Proxy & TransactionManager

`Proxy` is the central coordinator. It wires up the two transports and routes
messages between them.

```
Proxy
├── handleGsupPayload(payload, clientId)
│   ├── Decode GSUP
│   ├── If HLR-initiated response → handleGsupHlrResponse()
│   └── If SGSN-initiated request → gsupToMap() → send to HLR
│                                    store in txMgr_ + pendingOps_
│
├── handleMapPayload(payload)
│   ├── Decode MAP
│   ├── If Invoke → handleHlrInitiated()
│   └── If ReturnResult/Error
│       ├── Recover operation from pendingOps_ (ReturnError carries no op code)
│       ├── mapToGsup() → send to SGSN via stored clientId
│       └── complete() transaction

TransactionManager
└── map<uint32_t TID, PendingTransaction{imsi, clientId, invokeId, …}>

Proxy::pendingOps_
└── map<uint32_t TID, MapOperation>   ← needed because TCAP ReturnError
                                         carries no operation code on wire
```

### Converter

Pure functions that translate between GSUP and MAP message types:

| Function | Direction | Purpose |
|---|---|---|
| `gsupToMap()` | SGSN-initiated | GSUP request → MAP Invoke |
| `mapToGsup()` | SGSN-initiated | MAP ReturnResult/Error → GSUP result/error |
| `mapInvokeToGsup()` | HLR-initiated | MAP Invoke → GSUP request to SGSN |
| `gsupToMapResult()` | HLR-initiated | GSUP result/error from SGSN → MAP ReturnResult/Error |

---

## Message Flows

### SGSN-initiated (e.g., SendAuthInfo)

```
osmoSGSN          IpaServer          Proxy            MapTransport        HLR
    │                  │               │                   │               │
    │──IPA(GSUP SAI)──►│               │                   │               │
    │                  │──SAI payload─►│                   │               │
    │                  │               │─gsupToMap()──────►│               │
    │                  │               │  store TID+clientId               │
    │                  │               │                   │─M3UA DATA────►│
    │                  │               │                   │  (MAP Invoke) │
    │                  │               │                   │◄─M3UA DATA────│
    │                  │               │                   │ (ReturnResult)│
    │                  │               │◄─────────────────►│               │
    │                  │               │ mapToGsup()        │               │
    │                  │               │ lookup clientId    │               │
    │                  │◄─SAI result──│                   │               │
    │◄─IPA(GSUP SAI)──│               │                   │               │
```

### HLR-initiated (e.g., InsertSubscriberData)

```
HLR           MapTransport        Proxy            IpaServer          osmoSGSN
 │                  │               │                   │               │
 │──M3UA DATA──────►│               │                   │               │
 │  (MAP Invoke ISD)│──ISD payload─►│                   │               │
 │                  │               │─mapInvokeToGsup()─►               │
 │                  │               │  store IMSI→TID in hlrTx_         │
 │                  │               │                   │─IPA(GSUP ISD)►│
 │                  │               │                   │               │
 │                  │               │                   │◄─IPA(ISD Res)─│
 │                  │               │◄──ISD Result──────│               │
 │                  │               │ gsupToMapResult()  │               │
 │◄─M3UA DATA──────│               │                   │               │
 │  (ReturnResult)  │               │                   │               │
```

---

## Supported Operations

| GSUP (SGSN→HLR) | MAP Operation | GSUP (HLR→SGSN) |
|---|---|---|
| `SendAuthInfoRequest` | `SendAuthenticationInfo` (56) | — |
| `UpdateLocationRequest` | `UpdateGprsLocation` (23) | — |
| `LocationCancelRequest` | `CancelLocation` (3) | `LocationCancelRequest` |
| `InsertDataRequest` | `InsertSubscriberData` (7) | `InsertDataRequest` |
| `DeleteDataRequest` | `DeleteSubscriberData` (8) | `DeleteDataRequest` |
| `PurgeMsRequest` | `PurgeMS` (67) | `PurgeMsRequest` |

Both directions are supported for CancelLocation, InsertSubscriberData,
DeleteSubscriberData, and PurgeMS (HLR-initiated Invokes).

---

## Building

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| C++ compiler | C++17 | clang++ ≥ 9 or g++ ≥ 9 |
| CMake | ≥ 3.16 | |
| Boost | ≥ 1.71 | Header-only (Boost.Asio); no compiled libraries needed |
| GoogleTest | any | Fetched automatically via CMake `FetchContent` |

### Step-by-step build

**1. Configure**

```bash
cmake -B build -DBUILD_TESTS=ON
```

CMake will download GoogleTest on the first run (requires internet access).
Add `-DCMAKE_BUILD_TYPE=Release` for an optimised binary (default is `Debug`).

**2. Compile**

```bash
cmake --build build --parallel
```

The main binary is built at `build/gsup_map_proxy`.
Test binaries land in `build/tests/`.

**3. Verify the build**

```bash
ls build/gsup_map_proxy build/tests/
```

### Build variants

#### With SCTP transport (Linux only)

SCTP requires the kernel SCTP module and `<netinet/sctp.h>`:

```bash
cmake -B build -DBUILD_TESTS=ON -DENABLE_SCTP_TRANSPORT=ON
cmake --build build --parallel
```

#### With code coverage instrumentation

```bash
cmake -B build-cov -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cov --parallel
ctest --test-dir build-cov --parallel 4
# Coverage data (.gcda files) is written to build-cov/src/
# Use gcov or lcov to generate a report:
lcov --capture --directory build-cov/src --output-file coverage.info
genhtml coverage.info --output-directory coverage-html
```

---

## Running

```
build/gsup_map_proxy [listenPort [sgHost [sgPort [opc [dpc [hlrGt [localGt]]]]]]]
```

| Argument | Default | Description |
|---|---|---|
| `listenPort` | 4222 | TCP port to accept IPA connections from SGSNs |
| `sgHost` | 127.0.0.1 | Signalling Gateway hostname/IP |
| `sgPort` | 2905 | M3UA port on the Signalling Gateway |
| `opc` | 1 | Originating Point Code (this proxy) |
| `dpc` | 2 | Destination Point Code (HLR) |
| `hlrGt` | +49161000000 | HLR Global Title (E.164) |
| `localGt` | +49161000001 | Proxy local Global Title |

**Example — connect to a Signalling Gateway at 192.168.1.10:**

```bash
./build/gsup_map_proxy 4222 192.168.1.10 2905 100 200 +4916100000 +4916100001
```

The proxy runs a single-threaded Boost.Asio event loop. Send `SIGINT` or
`SIGTERM` to stop gracefully.

---

## Configuration Reference

### MapTransportConfig (C++ struct)

```cpp
struct MapTransportConfig {
    std::string sgHost;           // Signalling Gateway host
    uint16_t    sgPort    = 2905; // M3UA port
    uint32_t    opc       = 0;    // Originating Point Code
    uint32_t    dpc       = 0;    // Destination Point Code
    std::optional<uint32_t> routingContext; // M3UA routing context (optional)

    std::string hlrGt;            // HLR Global Title
    uint8_t     hlrSsn = 6;       // HLR Subsystem Number
    std::string localGt;          // Proxy local GT
    uint8_t     localSsn = 142;   // Proxy SSN (SGSN)

    std::chrono::seconds reconnectInterval{5};  // Delay between reconnect attempts
    std::chrono::seconds beatInterval{30};       // M3UA heartbeat interval
    bool useSCTP = false;         // Use SCTP instead of TCP (needs ENABLE_SCTP_TRANSPORT)
};
```

---

## Testing

### Run all tests

After building with `-DBUILD_TESTS=ON` (see [Building](#building)):

```bash
ctest --test-dir build --parallel 4
```

Expected output:

```
100% tests passed, 0 tests failed out of 188
Total Test time (real) =   0.75 sec
```

### Run a single test binary

Each test binary can be run directly for faster iteration or to see verbose
GoogleTest output:

```bash
# List all available test binaries
ls build/tests/

# Run one binary with full output
./build/tests/converter_test

# Run a specific test case by name (GoogleTest filter)
./build/tests/proxy_test --gtest_filter='ProxyTest.HlrInitiatedPurgeMsFullRoundTrip'

# Run all tests whose name contains a keyword
./build/tests/proxy_end_to_end_test --gtest_filter='*HlrInitiated*'
```

### Test structure

| Binary | Tests | Layer |
|---|---|---|
| `gsup_codec_test` | GSUP encode/decode | Protocol |
| `ipa_codec_test` | IPA framing | Protocol |
| `ber_codec_test` | BER primitives | Protocol |
| `map_codec_test` | MAP/TCAP encode/decode | Protocol |
| `converter_test` | GSUP↔MAP conversion | Protocol |
| `transaction_test` | TID allocation, expiry | Protocol |
| `proxy_test` | Proxy routing with mock transports | Protocol |
| `ipa_server_test` | IPA server, multi-session, routing | Transport |
| `m3ua_codec_test` | M3UA encode/decode | Transport |
| `scccp_codec_test` | SCCP UDT encode/decode | Transport |
| `map_transport_test` | M3UA state machine, heartbeat | Transport |
| `proxy_end_to_end_test` | Full stack with mock SG | Integration |

**188 tests total — 100% passing.**

### End-to-end test architecture

```
ProxyEndToEndTest fixture
├── SmartMockSG        ← mock Signalling Gateway (M3UA handshake + auto-respond)
├── IpaServer          ← real IPA listener
├── MapTransport       ← real M3UA client
├── Proxy              ← real proxy logic
└── IpaTestClient      ← simulates osmoSGSN (synchronous TCP client)
```

---

## Code Coverage

Coverage measured with `--coverage` (Apple LLVM 17 / gcov), 188 tests:

| File | Coverage | Lines |
|---|---|---|
| `proxy/TransactionManager.cpp` | **100.0%** | 38 |
| `map/BerCodec.cpp` | **97.8%** | 89 |
| `proxy/Converter.cpp` | **95.9%** | 145 |
| `transport/M3uaCodec.cpp` | **95.7%** | 116 |
| `map/MapCodec.cpp` | **94.0%** | 401 |
| `ipa/IpaCodec.cpp` | **93.6%** | 31 |
| `transport/ScccpCodec.cpp` | **91.8%** | 159 |
| `proxy/Proxy.cpp` | **84.9%** | 126 |
| `gsup/GsupCodec.cpp` | **85.1%** | 215 |
| `transport/IpaSession.cpp` | **86.2%** | 94 |
| `transport/IpaServer.cpp` | **81.0%** | 58 |
| `transport/MapTransport.cpp` | **78.5%** | 186 |
| **Overall** | **89.9%** | **1658** |

Uncovered lines are almost exclusively error-recovery paths that require
simulated TCP write failures, connection drops, or DNS resolution failures —
conditions that cannot be triggered deterministically without mock socket
injection.
