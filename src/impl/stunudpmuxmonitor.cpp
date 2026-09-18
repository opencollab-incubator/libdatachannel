/* SPDX-License-Identifier: MPL-2.0 */
#include "stunudpmuxmonitor.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace rtc::impl {
StunUdpMuxMonitor::StunUdpMuxMonitor(StunUdpMuxMonitorConfiguration config) {
	if (!config.localPort || !config.serverPort || config.serverHost.empty() ||
	    config.serverHost.size() > 255 || config.serverHost.find('\0') != string::npos ||
	    (config.bindAddress && (config.bindAddress->empty() ||
	                            config.bindAddress->find('\0') != string::npos)))
		throw std::invalid_argument("Explicit UDP port and valid STUN host/port are required");
#if !USE_NICE
	juice_config_t native{};
	native.concurrency_mode = JUICE_CONCURRENCY_MODE_MUX;
	native.bind_address = config.bindAddress ? config.bindAddress->c_str() : nullptr;
	native.local_port_range_begin = native.local_port_range_end = config.localPort;
	native.stun_server_host = config.serverHost.c_str();
	native.stun_server_port = config.serverPort;
	// juice_create owns copies of all configuration strings. No C++ callback or
	// application pointer is retained by the background resolver/receive thread.
	auto agent = juice_create(&native);
	if (!agent)
		throw std::runtime_error("Failed to create STUN UDP mux monitor");
	if (juice_set_stun_monitoring(agent, true) != JUICE_ERR_SUCCESS ||
	    juice_gather_candidates(agent) != JUICE_ERR_SUCCESS) {
		juice_destroy(agent);
		throw std::runtime_error("Failed to start STUN UDP mux monitor");
	}
	mAgent = agent;
#else
	throw std::runtime_error("STUN UDP mux monitoring requires libjuice");
#endif
}

StunUdpMuxMonitor::~StunUdpMuxMonitor() { stop(); }

void StunUdpMuxMonitor::stop() {
	std::lock_guard lock(mMutex);
#if !USE_NICE
	if (auto agent = std::exchange(mAgent, nullptr))
		juice_destroy(agent);
#endif
}

optional<StunBinding> StunUdpMuxMonitor::binding(unsigned int index) const {
	std::lock_guard lock(mMutex);
#if !USE_NICE
	if (!mAgent)
		throw std::logic_error("STUN UDP mux monitor is stopped");
	juice_stun_binding_t native{};
	int result = juice_get_stun_binding(mAgent, index, &native);
	if (result == JUICE_ERR_NOT_AVAIL)
		return nullopt;
	if (result != JUICE_ERR_SUCCESS)
		throw std::runtime_error("STUN binding observation unavailable");
	StunBinding copy;
	copy.serverAddress = native.server_address;
	copy.serverPort = native.server_port;
	copy.mappedAddress = native.mapped_address;
	copy.mappedPort = native.mapped_port;
	copy.state = static_cast<StunBindingState>(native.state);
	copy.successfulResponses = native.successful_responses;
	copy.failedTransactions = native.failed_transactions;
	copy.mappingRevision = native.mapping_revision;
	if (native.last_success_age_ms != std::numeric_limits<uint64_t>::max())
		copy.lastSuccessAgeMs = native.last_success_age_ms;
	return copy;
#else
	(void)index;
	throw std::runtime_error("STUN UDP mux monitoring requires libjuice");
#endif
}
} // namespace rtc::impl
