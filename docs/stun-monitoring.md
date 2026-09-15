# STUN monitoring on a shared UDP socket

`StunUdpMuxMonitor` owns one libjuice discovery agent and its socket share. It creates no remote ICE peer, DTLS transport or data channel. Native code keeps the STUN binding warm independently of application polling.

When an `IceUdpMuxListener` already owns the endpoint, use its factory to inherit the exact native bind address and fixed port:

```cpp
auto monitor = listener.monitorStun(stunServerHost, stunServerPort);
auto observed = monitor->binding(0);
if (observed && observed->lastSuccessAgeMs) {
    // Consume owned mappedAddress/mappedPort, mappingRevision, success/failure
    // counters and age. Apply the application's own observation freshness limit.
}
monitor->stop();
```

Alternatively construct a monitor with `StunUdpMuxMonitorConfiguration`. Its bind-address spelling and local port must exactly match the listener/peer configuration to share their socket. An explicit nonzero local port is required. Configuration strings are copied before the constructor returns.

`binding(index)` returns an owned, atomic snapshot for one resolved STUN endpoint, or `nullopt` when that index is unavailable. Each address family/server has its own observation. Counters and mapped endpoints are copied together. The copied age describes the snapshot time; retained snapshots continue ageing in the application. A fresh unchanged response increments `successfulResponses` without changing `mappingRevision`. Errors retain the historical mapping and its age. Neither polling nor a successful STUN response establishes arbitrary-source reachability.

`stop()` is idempotent and serialized with snapshot reads. Calls after stop throw a closed-state error. The monitor remains caller-owned after its listener stops; stopping a monitor leaves other listeners/peers on the same mux operational. A dedicated discovery agent never changes an established peer's identity or selected pair.

The C API provides corresponding create/get/delete operations. `rtcCreateIceUdpMuxStunMonitor` inherits a live listener's binding. `rtcGetStunUdpMuxBinding` fills caller-owned fixed-size strings and values only on success. It returns `RTC_ERR_NOT_AVAIL` for an unresolved/nonexistent index; invalid/deleted handles are errors. Reads concurrent with deletion retain native ownership and either complete with a valid copy or return an error.

Resolution behavior is inherited from libjuice: it resolves once, retains at most two results, and does not guarantee one result from each family. Applications needing controlled dual-family refresh should select numeric STUN endpoints and replace dedicated monitors as their selected DNS results change, keeping the actual listener/peer socket owners alive. Profile freshness and probe invalidation belong to the application.

Build the local C/C++ regression tests with `-DSTUN_UDP_MUX_TESTS=ON`, then run `ctest --test-dir build --output-on-failure -R stun-udp-mux`. They use real IPv4/IPv6 loopback STUN exchanges and validate socket sharing, copy ownership, refresh revisions, concurrent deletion and zero peer construction. The selected libjuice revision must provide the monitoring API. Tests using fixture STUN mappings do not establish real NAT traversal or application connectivity.
