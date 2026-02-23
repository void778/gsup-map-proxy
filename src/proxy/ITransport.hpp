#pragma once
#include "common/Buffer.hpp"
#include <cstdint>
#include <functional>

namespace proxy {

// Minimal transport abstraction — allows unit testing without real sockets.

// Opaque per-session identifier.  0 means "all sessions" / "broadcast".
using ClientId = uint64_t;

// Callback receives the payload plus the originating ClientId.
using MessageCallback = std::function<void(const Bytes& data, ClientId clientId)>;

class ITransport {
public:
    virtual ~ITransport() = default;

    // Send raw bytes to the peer identified by clientId.
    // clientId == 0: deliver to all connected sessions (broadcast).
    virtual void send(const Bytes& data, ClientId clientId = 0) = 0;

    // Register a callback invoked on every received message/frame.
    virtual void onMessage(MessageCallback cb) = 0;
};

} // namespace proxy
