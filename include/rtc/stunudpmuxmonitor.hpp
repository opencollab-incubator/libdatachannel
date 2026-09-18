/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RTC_STUN_UDP_MUX_MONITOR_H
#define RTC_STUN_UDP_MUX_MONITOR_H

#include "common.hpp"

namespace rtc {
namespace impl { struct StunUdpMuxMonitor; }

struct StunUdpMuxMonitorConfiguration {
	optional<string> bindAddress;
	uint16_t localPort = 0; // Explicit shared UDP port required
	string serverHost;
	uint16_t serverPort = 3478;
};

enum class StunBindingState { Pending = 0, Succeeded, Failed };

// Owned, atomic observation of one resolved server. An observation is neither a
// NAT lease nor peer reachability. Missing age means no successful response yet.
struct StunBinding {
	string serverAddress;
	uint16_t serverPort = 0;
	string mappedAddress;
	uint16_t mappedPort = 0;
	StunBindingState state = StunBindingState::Pending;
	uint64_t successfulResponses = 0;
	uint64_t failedTransactions = 0;
	uint64_t mappingRevision = 0;
	optional<uint64_t> lastSuccessAgeMs;
};

// Owns a dedicated libjuice agent with no remote peer, DTLS or data channel.
// Polling does not send packets or renew freshness. Other mux owners outlive stop.
class RTC_CPP_EXPORT StunUdpMuxMonitor final : private CheshireCat<impl::StunUdpMuxMonitor> {
public:
	explicit StunUdpMuxMonitor(StunUdpMuxMonitorConfiguration config);
	~StunUdpMuxMonitor();
	void stop(); // Idempotent; safe concurrently with binding()
	optional<StunBinding> binding(unsigned int index) const;
private:
	using CheshireCat<impl::StunUdpMuxMonitor>::impl;
};
} // namespace rtc
#endif
