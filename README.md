# gsup-map-proxy

[![CI](https://github.com/void778/gsup-map-proxy/actions/workflows/ci.yml/badge.svg)](https://github.com/void778/gsup-map-proxy/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/void778/gsup-map-proxy/branch/main/graph/badge.svg)](https://codecov.io/gh/void778/gsup-map-proxy)

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
3. [Source Layout](#source-layout)
4. [Protocol Stack](#protocol-stack)
5. [Component Design](#component-design)
6. [Message Flows](#message-flows)
7. [Supported Operations](#supported-operations)
8. [Design Decisions & Tradeoffs](#design-decisions--tradeoffs)
9. [Limitations](#limitations)
10. [Building](#building)
11. [Running](#running)
12. [Configuration Reference](#configuration-reference)
13. [Testing](#testing)
14. [Code Coverage](#code-coverage)

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

Everything runs on a **single Boost.Asio `io_context`** thread. There are
no mutexes in the codebase — all state is accessed exclusively from the
event loop.

---

## Source Layout

```
src/
├── common/
│   └── Buffer.hpp          BufferReader / BufferWriter — safe cursor-based
│                           read/write helpers used by all codecs
├── gsup/
│   └── GsupCodec           GSUP message encode/decode (TLV format)
├── ipa/
│   ├── IpaCodec            IPA frame encode/decode (2-byte len + stream ID)
│   └── IpaFrame.hpp        IpaFrame struct + Bytes type alias
├── map/
│   ├── BerCodec            BER primitive encode/decode (lengths, tags)
│   └── MapCodec            MAP/TCAP PDU encode/decode
├── proxy/
│   ├── ITransport.hpp      Abstract transport interface
│   ├── Proxy               Central coordinator — routes messages, owns state
│   ├── TransactionManager  TID lifecycle, per-transaction metadata, expiry
│   └── Converter           Pure GSUP↔MAP translation functions
└── transport/
    ├── IpaServer           Multi-session IPA TCP acceptor
    ├── IpaSession          Per-connection IPA state machine (CCM + GSUP)
    ├── MapTransport        M3UA client + SCCP wrapper (HLR-facing)
    ├── M3uaCodec           M3UA message encode/decode + stream reassembler
    └── ScccpCodec          SCCP UDT encode/decode + Global Title helpers
```

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
└── sessions_                  map<ClientId, shared_ptr<IpaSession>>

IpaSession  (enable_shared_from_this)
├── start(onReady, onMsg, onDisconnect)
├── State: Handshaking → Ready → Stopped
├── Performs CCM handshake (ID_REQ → ID_RESP → ID_ACK)
├── sendGsup(payload)          posts to executor, enqueues IPA frame
├── Handles PING → PONG keepalive
└── write queue (deque<Bytes>) — one async_write in flight at a time
```

Sessions remain in `allSessions_` from accept until disconnect.  They are
promoted to `sessions_` (the "ready" map) only after the CCM handshake
completes. This prevents GSUP traffic from being routed to a session that
has not finished identifying itself.

### MapTransport

`MapTransport` implements `ITransport` for the HLR-facing side. It maintains
a single TCP (or SCTP) connection to the Signalling Gateway and implements
the M3UA ASP state machine.

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

On any read or write error the state reverts to `DISCONNECTED` and a
reconnect is scheduled after `reconnectInterval` (default 5 s). The write
queue and decoder buffer are cleared on each disconnect so no stale data
leaks into a new connection.

SCTP support is available when built with `-DENABLE_SCTP_TRANSPORT=ON`.
The socket type is `asio::generic::stream_protocol::socket`, so the same
code path handles both TCP and SCTP. The address family (AF_INET / AF_INET6)
is derived from the DNS resolver result, so both IPv4 and IPv6 Signalling
Gateway addresses work.

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
│   ├── Decode MAP TCAP
│   ├── If Invoke → handleHlrInitiated()   (HLR pushes ISD/Cancel/etc.)
│   └── If ReturnResult/Error
│       ├── Recover operation from pendingOps_
│       │   (TCAP ReturnError carries no op code on the wire)
│       ├── mapToGsup() → send to SGSN via stored clientId
│       └── complete() / erase transaction

TransactionManager
└── map<uint32_t OTID, PendingTransaction{imsi, clientId, invokeId, …}>
    └── expireStale() — called every 5 s; sends GSUP Error for timed-out TXs

Proxy::pendingOps_
└── map<uint32_t OTID, MapOperation>
    └── needed because TCAP ReturnError carries no operation code on wire

Proxy::hlrTx_
└── map<std::string IMSI, uint32_t OTID>
    └── correlates HLR-initiated Invokes with the SGSN response
```

### Converter

Pure functions with no side effects that translate between GSUP and MAP:

| Function | Direction | Purpose |
|---|---|---|
| `gsupToMap()` | SGSN-initiated | GSUP request → MAP TCAP Begin (Invoke) |
| `mapToGsup()` | SGSN-initiated | MAP TCAP End (ReturnResult/Error) → GSUP result/error |
| `mapInvokeToGsup()` | HLR-initiated | MAP Invoke from HLR → GSUP request to SGSN |
| `gsupToMapResult()` | HLR-initiated | GSUP result/error from SGSN → MAP ReturnResult/Error |

Being pure functions makes the converter trivially testable — all 45
converter tests exercise these functions directly with no I/O or mocks
required.

### ITransport interface

```cpp
class ITransport {
    using MessageCallback    = std::function<void(Bytes payload, ClientId)>;
    using DisconnectCallback = std::function<void()>;

    virtual void send(const Bytes& payload, ClientId = 0) = 0;
    virtual void onMessage(MessageCallback)    = 0;
    virtual void onDisconnect(DisconnectCallback) {}
};
```

`Proxy` knows nothing about TCP, M3UA, or IPA framing. This abstraction
makes it possible to replace either transport with a mock in tests, and
means the proxy logic is exercised at multiple levels:

- **Unit tests** (`proxy_test`): both transports are `MockTransport` objects
  that capture sent messages in a vector.
- **Integration tests** (`proxy_end_to_end_test`): `IpaServer` and
  `MapTransport` are real; only the remote Signalling Gateway is mocked.

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
    │                  │               │◄──map payload─────│               │
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
DeleteSubscriberData, and PurgeMS — the HLR can initiate these operations
independently of any SGSN request.

---

## Design Decisions & Tradeoffs

### Single-threaded event loop

All I/O — accepting connections, reading/writing TCP data, timer expiry —
runs on a single `boost::asio::io_context`. There are **no mutexes** in the
codebase.

**Why:** In a protocol proxy like this, work is almost entirely I/O-bound.
A single-threaded event loop eliminates an entire class of concurrency bugs
(data races, deadlocks, lock-ordering issues) at zero cost. MAP messages
are small (typically 50–500 bytes), so decode time is negligible compared
to network latency.

**Tradeoff:** A single CPU core is used. If a future change introduces
CPU-heavy processing in a callback it would delay all other I/O. The
mitigation would be to `asio::post` the heavy work to a thread pool, but
this is not needed today.

### ITransport abstraction

`Proxy` depends only on the `ITransport` interface, not on any concrete
transport type. Both `IpaServer` and `MapTransport` implement it.

**Why:** This makes the proxy logic fully testable without any real sockets.
Unit tests use `MockTransport` objects that record sent messages in a
`std::vector`. The integration test suite (`proxy_end_to_end_test`) uses
real transports but with a mock Signalling Gateway. No test needs a real
SGSN or HLR.

**Tradeoff:** A small amount of interface boilerplate. The interface is kept
narrow intentionally — `send`, `onMessage`, and `onDisconnect` cover all
required interactions.

### Shared ownership for async lifetime safety

`IpaSession` and `MapTransport` both use `std::enable_shared_from_this`.
Every async callback captures `[self = shared_from_this()]` rather than
a raw `this` pointer.

**Why:** An async operation (e.g., `async_write`) is dispatched to the
event loop. If the session object were destroyed before that callback fires,
the raw `this` pointer would be dangling. Capturing a `shared_ptr` keeps
the object alive until the callback completes.

**Tradeoff:** Objects must be heap-allocated via `std::make_shared`.
The reference-counting overhead is acceptable for session-lifetime objects
(one per SGSN connection, one for the HLR connection).

### Synchronous DNS resolution

`MapTransport::connect()` calls `resolver.resolve()` synchronously before
starting the async connect.

**Why:** Asynchronous resolve would require an extra state (`Resolving`)
in the connection state machine, additional error handling, and another
level of lambda nesting. Synchronous resolution is simpler and the cost
is negligible — DNS is only called at (re)connect time, not per-message,
and a LAN DNS lookup typically takes < 1 ms.

**Tradeoff:** The io_context thread is blocked during the DNS call. If the
DNS server is unreachable, the thread blocks for the system resolver timeout
(typically 5 s). This is acceptable because it only happens on reconnect,
and the reconnect interval (5 s default) provides natural rate-limiting.

### TCAP Begin/End only (no Continue)

The proxy only generates and consumes TCAP `Begin` and `End` messages. All
MAP operations in scope complete within a single dialog.

**Why:** `SendAuthenticationInfo`, `UpdateGprsLocation`, `InsertSubscriberData`,
and the other supported operations all follow a strict request/response model:
one `Begin` (Invoke) from the proxy, one `End` (ReturnResult or ReturnError)
from the HLR. No `Continue` is needed. Supporting `Continue` would require
tracking dialog state across multiple PDUs.

**Tradeoff:** Operations that require multi-PDU dialogs (rare in the MAP
phase 2+ operations relevant to GPRS) cannot be supported without changes
to the TCAP layer.

### SCCP connectionless (UDT)

All MAP PDUs are wrapped in SCCP **Unitdata (UDT)** messages — the
connectionless SCCP service.

**Why:** Commercial HLRs expect connectionless SCCP for MAP over M3UA
in this application. Connection-oriented SCCP (CC/CREF/RLSD) is more
complex and not required here.

**Tradeoff:** SCCP provides no sequencing guarantee at the SCCP layer.
Ordering is guaranteed by TCP/SCTP at the transport level, which is
sufficient for a point-to-point proxy with a single active connection.

### Random 32-bit TCAP Transaction IDs

Each new GSUP request is assigned a random 32-bit Originating Transaction ID
(OTID) generated by `std::mt19937` seeded from `std::random_device`.

**Why:** Random IDs avoid TID collisions across reconnects (a sequential
counter that resets on reconnect could collide with a TID the HLR still
considers active). They also make the protocol harder to spoof.

**Tradeoff:** There is a small probability of collision between concurrent
transactions. At 100 concurrent transactions the birthday-problem collision
probability is ~1.2 × 10⁻⁶, which is negligible for the expected load. A
collision would cause a misrouted response, not a crash — the unmatched
transaction would expire after 30 s.

### pendingOps\_ map for TCAP ReturnError recovery

When a MAP Invoke is sent, both the transaction metadata (`txMgr_`) and the
operation code (`pendingOps_`) are stored keyed by OTID.

**Why:** This is a TCAP/MAP specification constraint: TCAP `ReturnError`
PDUs carry the original invoke ID but **not** the operation code. Without
storing the operation code at send time, it is impossible to synthesize the
correct GSUP error message type when the HLR returns an error.

**Tradeoff:** Two separate maps (`txMgr_` and `pendingOps_`) must be kept
in sync. They are always updated together and erased together, which is
manageable given the narrow interface.

### HLR-initiated transactions keyed by IMSI

HLR-initiated MAP Invokes (e.g., `InsertSubscriberData`) arrive without
any prior GSUP transaction to correlate against. The IMSI carried in the
MAP message is used as the correlation key in `hlrTx_`.

**Why:** The IMSI is the only stable subscriber identifier present in both
the MAP Invoke and the subsequent GSUP response from the SGSN. It is the
natural correlation key.

**Tradeoff:** This assumes at most one active HLR-initiated operation per
IMSI at a time. If the HLR were to send two concurrent `InsertSubscriberData`
Invokes for the same subscriber, the second would overwrite the first in
`hlrTx_`, and the response to the first would be lost. This scenario is
not observed in practice with commercial HLRs.

### Transaction expiry

`TransactionManager::expireStale()` is called every 5 seconds. Transactions
older than the timeout (default 30 s) are removed and a GSUP error is
returned to the originating SGSN.

**Why:** Without expiry, a non-responsive HLR would cause the SGSN to
wait indefinitely and leak entries in both maps. Expiry bounds memory usage
and ensures the SGSN gets a timely failure indication it can act on.

**Tradeoff:** If the HLR legitimately takes longer than 30 s to respond
(unusual but not impossible under heavy load), the SGSN receives a spurious
error and may retry. The timeout is configurable at `Proxy` construction for
testing and for deployments with slow HLRs.

### Buffer size limits

The M3UA stream reassembler rejects any message with a declared length
greater than 64 KiB. The IPA framing decoder rejects frames with a
wire-length field greater than 4 KiB. Both reset their internal buffer and
throw on violation.

**Why:** Without these guards, a malformed or malicious peer can send a
header claiming a huge payload length, causing the decoder to accumulate
data until memory is exhausted (OOM/DoS). 64 KiB is far above any real
MAP message; 4 KiB is far above any real GSUP message.

**Tradeoff:** A legitimate peer that sends an oversized message (which
should never occur with correct implementations) will be disconnected. The
connection will be re-established via the normal reconnect mechanism.

---

## Limitations

| Area | Limitation |
|---|---|
| **TLS** | IPA and M3UA connections are plaintext. Relies on network-level security (private LAN / VPN). |
| **Single SG connection** | MapTransport maintains one connection to one Signalling Gateway. There is no load balancing or failover to a secondary SG. |
| **TCAP dialogs** | Only TCAP Begin/End are used. Operations requiring mid-dialog `Continue` messages cannot be supported without changes to the TCAP layer. |
| **HLR-initiated concurrency** | At most one active HLR-initiated operation per IMSI at a time. Concurrent Invokes for the same subscriber are not correlated correctly. |
| **SCCP connectionless only** | Only SCCP UDT (connectionless) is generated. Connection-oriented SCCP is not implemented. |
| **SCTP on Linux only** | SCTP transport requires `ENABLE_SCTP_TRANSPORT=ON` at build time and the kernel SCTP module (`modprobe sctp`) at runtime. macOS is not supported for SCTP. |
| **No authentication** | The proxy accepts any GSUP client that connects on the listen port. Access control is expected to be enforced at the network layer. |
| **Argument-only configuration** | All settings are positional CLI arguments. There is no config file support. |

---

## Building

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| C++ compiler | C++17 | clang++ ≥ 9 or g++ ≥ 9 |
| CMake | ≥ 3.16 | |
| Boost | ≥ 1.71 | Header-only (Boost.Asio); no compiled libraries needed |
| GoogleTest | any | Fetched automatically via CMake `FetchContent` |
| spdlog | v1.13.0 | Fetched automatically via CMake `FetchContent` |

### Step-by-step build

**1. Configure**

```bash
cmake -B build -DBUILD_TESTS=ON
```

CMake will download GoogleTest and spdlog on the first run (requires
internet access). Add `-DCMAKE_BUILD_TYPE=Release` for an optimised binary
(default is `Debug`).

**2. Compile**

```bash
cmake --build build --parallel
```

The main binary is built at `build/src/gsup_map_proxy`.
Test binaries land in `build/tests/`.

**3. Verify the build**

```bash
ls build/src/gsup_map_proxy build/tests/
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
# Coverage data (.gcda files) is written to build-cov/
# Use gcov or lcov to generate a report:
lcov --capture --directory build-cov --output-file coverage_raw.info
lcov --extract coverage_raw.info "$(pwd)/src/*" --output-file coverage.info
genhtml coverage.info --output-directory coverage-html
```

---

## Running

```
build/src/gsup_map_proxy [listenPort [sgHost [sgPort [opc [dpc [hlrGt [localGt [routingContext]]]]]]]]]
```

| Argument | Default | Description |
|---|---|---|
| `listenPort` | 4222 | TCP port to accept IPA connections from SGSNs |
| `sgHost` | 127.0.0.1 | Signalling Gateway hostname/IP (IPv4 or IPv6) |
| `sgPort` | 2905 | M3UA port on the Signalling Gateway |
| `opc` | 1 | Originating Point Code (this proxy) |
| `dpc` | 2 | Destination Point Code (HLR) |
| `hlrGt` | +49161000000 | HLR Global Title (E.164) |
| `localGt` | +49161000001 | Proxy local Global Title |
| `routingContext` | _(none)_ | M3UA Routing Context value (required by some SS7 hardware) |

**Example — connect to a Signalling Gateway at 192.168.1.10 with routing context 1:**

```bash
./build/src/gsup_map_proxy 4222 192.168.1.10 2905 100 200 +4916100000 +4916100001 1
```

The proxy runs a single-threaded Boost.Asio event loop. Send `SIGINT` or
`SIGTERM` to stop gracefully.

### Logging

The proxy uses [spdlog](https://github.com/gabime/spdlog) and writes
timestamped log lines to **stderr**.

**Runtime log level** is controlled by the `GSUP_LOG_LEVEL` environment
variable (case-insensitive). If the variable is not set the level defaults
to `info`.

| Level | When to use |
|---|---|
| `error` | Only show errors |
| `warn` | Errors + warnings (unroutable messages, missing transactions) |
| `info` | Normal operation: connect/disconnect, ASPUP/ASPAC, session lifecycle _(default)_ |
| `debug` | Per-message flow: GSUP type, TID allocation, MAP encoding/decoding, routing decisions |
| `trace` | High-frequency events: heartbeats, CCM PING/PONG |

```bash
# Verbose session debug
GSUP_LOG_LEVEL=debug ./build/src/gsup_map_proxy ...

# Trace everything (heartbeats etc.)
GSUP_LOG_LEVEL=trace ./build/src/gsup_map_proxy ...

# Quiet operation
GSUP_LOG_LEVEL=warn  ./build/src/gsup_map_proxy ...
```

At `debug` level each transaction prints a line for every major step:

```
[proxy] GSUP rx: UL-REQ IMSI=262010000000001 client=1
[proxy] TX alloc: TID=0x3a7c1b04 invokeId=1 IMSI=262010000000001 client=1
[proxy] GSUP UL-REQ → MAP UpdateGprsLocation (TID=0x3a7c1b04)
[MapTransport] DATA received: OPC=1 DPC=2 payload=87 bytes
[proxy] MAP rx: op=UpdateGprsLocation TID=0x3a7c1b04 IMSI=262010000000001 component=1
[proxy] MAP UpdateGprsLocation → GSUP UL-RES (TID=0x3a7c1b04 → client=1)
```

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
    uint8_t     hlrSsn = 6;       // HLR Subsystem Number (SSN_HLR)
    std::string localGt;          // Proxy local GT
    uint8_t     localSsn = 142;   // Proxy SSN (SSN_SGSN)

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
100% tests passed, 0 tests failed out of 220
Total Test time (real) =   1.xx sec
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

**220 tests total — 100% passing.**

### End-to-end test architecture

```
ProxyEndToEndTest fixture
├── SmartMockSG        ← mock Signalling Gateway (M3UA handshake + auto-respond)
├── IpaServer          ← real IPA listener
├── MapTransport       ← real M3UA client
├── Proxy              ← real proxy logic
└── IpaTestClient      ← simulates osmoSGSN (synchronous TCP client)
```

The integration tests exercise the full protocol path from a raw IPA byte
stream through to a raw M3UA byte stream and back, using the real codec,
converter, and connection state machines. Only the remote endpoints
(Signalling Gateway, osmoSGSN) are replaced by in-process mocks.

---

## Code Coverage

Coverage is measured on every CI run using gcov + lcov and uploaded to
Codecov automatically. See the badge at the top of this file for the
current percentage, or browse per-file details at:

> https://codecov.io/gh/void778/gsup-map-proxy

The CI coverage job builds with `--coverage -O0` using gcc-12, runs all
220 tests, then uploads `src/**` line coverage only (third-party headers
and generated code are excluded).

Uncovered lines are almost exclusively error-recovery paths that require
simulated TCP write failures, connection drops, or DNS resolution failures —
conditions that cannot be triggered deterministically without mock socket
injection.
