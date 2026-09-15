# Immutable UDP send limits

`Configuration::udpSendLimits` and the additive C constructors
`rtcCreatePeerConnectionWithUdpLimits` / `rtcPrepareIceUdpMuxPeerWithUdpLimits`
install an optional budget on the actual libjuice agent before gathering or
incoming acceptance. Existing C configuration struct layout is unchanged.

The absolute deadline must use `PeerConnection::udpMonotonicTimeMs()` or
`rtcGetUdpMonotonicTimeMs()`. Capture it before asynchronous authorization work;
never derive a new duration when accepting or reusing a peer. The optional
numeric destination address and port are copied into owned storage. Existing
incoming agents retain their original limits when the listener reuses them.

The guard runs inside the UDP mux send lock immediately before every UDP OS
send, including ICE requests/replies, STUN refresh/retransmission and DTLS/SCTP
traffic. The count reserves failed OS send attempts too. Payload excludes IP
and UDP headers. Failed policy checks never reach the OS. The immutable budget
cannot be replaced, renewed or reset. No limit is installed on ordinary peers.
Unsupported non-mux, local TURN/relay-only, ICE-TCP, proxy and libnice
configurations reject. Numeric destination validation occurs before agent
connection setup; the API never resolves destination hostnames.

`udpSendStats()` / `rtcGetUdpSendStats()` return an owned atomic snapshot or
unavailable if no limited ICE agent exists. Successful-send counters mean OS
acceptance, not remote delivery. Closing the peer follows the existing native
transport lifetime; exhaustion does not itself close it. Callers must own
bounded cleanup. Shared mux listeners and unrelated unlimited peers are not
budgeted by this per-agent mechanism.

For a 1200-byte UDP payload cap, configure MTU 1248 (the transport subtracts the
48-byte IPv6 + UDP overhead). The native guard is authoritative even if the MTU
is misconfigured. The generic library does not authorize diagnostic workloads,
validate application frame sizes or enforce purpose-specific channel policy.

`-DUDP_LIMIT_API_TESTS=ON` enables the C API validation/lifetime test. The pinned
libjuice dependency has separate real-UDP count, size, deadline, destination,
STUN retransmission, concurrency, sharing and IPv4/IPv6 tests. Full transport
tests are supplied by the Java bridge's native probe.
