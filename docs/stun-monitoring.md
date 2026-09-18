# STUN monitoring on a shared UDP socket

`StunUdpMuxMonitor` owns a libjuice monitoring agent without creating a remote ICE peer, DTLS transport or data channel. Libjuice performs STUN refreshes and records observations; libdatachannel provides C/C++ ownership and listener integration.

Use the listener factory to inherit its exact bind address and fixed port:

```cpp
auto monitor = listener.monitorStun(stunServerHost, stunServerPort);
auto observed = monitor->binding(0);
if (observed && observed->lastSuccessAgeMs) {
    // Apply the application's freshness and reachability policy.
}
monitor->stop();
```

Alternatively, `StunUdpMuxMonitorConfiguration` creates a standalone monitor. An explicit nonzero local port is required; its bind-address spelling and port must match the listener/peer to share their socket. Configuration strings are copied before construction returns.

`binding(index)` returns an owned snapshot or `nullopt` when the index is unavailable. `stop()` is idempotent and serialized with reads; subsequent reads throw a closed-state error. The monitor remains caller-owned after its listener stops. Stopping it leaves other owners of the shared socket operational.

The C API provides create/get/delete operations. `rtcCreateIceUdpMuxStunMonitor` inherits a live listener's binding. `rtcGetStunUdpMuxBinding` fills caller-owned output only on success and returns `RTC_ERR_NOT_AVAIL` for an unavailable index. Invalid/deleted handles are errors. A read racing deletion either completes with a valid copy or returns an error.

See the pinned [libjuice monitoring documentation](../deps/libjuice/docs/stun-monitoring.md) for refresh, mapping revision, response age and resolver semantics. A STUN observation alone does not establish external reachability.

The normal test runner covers C/C++ shared sockets, copied results and concurrent deletion on IPv4 and IPv6. These loopback checks use the first STUN response and do not wait for periodic refresh timers.
