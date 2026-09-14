/* SPDX-License-Identifier: MPL-2.0 */
#include "stunudpmuxmonitor.hpp"
#include "impl/stunudpmuxmonitor.hpp"

namespace rtc {
StunUdpMuxMonitor::StunUdpMuxMonitor(StunUdpMuxMonitorConfiguration config)
    : CheshireCat<impl::StunUdpMuxMonitor>(std::move(config)) {}
StunUdpMuxMonitor::~StunUdpMuxMonitor() = default;
void StunUdpMuxMonitor::stop() { impl()->stop(); }
optional<StunBinding> StunUdpMuxMonitor::binding(unsigned int index) const {
	return impl()->binding(index);
}
} // namespace rtc
