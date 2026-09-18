/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RTC_IMPL_STUN_UDP_MUX_MONITOR_H
#define RTC_IMPL_STUN_UDP_MUX_MONITOR_H
#include "common.hpp"
#include "rtc/stunudpmuxmonitor.hpp"

#if !USE_NICE
#include <juice/juice.h>
#endif

namespace rtc::impl {
struct StunUdpMuxMonitor final {
	explicit StunUdpMuxMonitor(StunUdpMuxMonitorConfiguration config);
	~StunUdpMuxMonitor();
	void stop();
	optional<StunBinding> binding(unsigned int index) const;
private:
	mutable std::mutex mMutex;
#if !USE_NICE
	juice_agent_t *mAgent = nullptr;
#endif
};
}
#endif
