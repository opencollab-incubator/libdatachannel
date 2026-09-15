/* SPDX-License-Identifier: MPL-2.0 */
#include "rtc/rtc.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

int main() {
	rtcInitLogger(RTC_LOG_FATAL, nullptr);
	uint64_t now = 0;
	assert(rtcGetUdpMonotonicTimeMs(nullptr) == RTC_ERR_INVALID);
	assert(rtcGetUdpMonotonicTimeMs(&now) == RTC_ERR_SUCCESS && now > 0);
	rtcConfiguration config{};
	config.enableIceUdpMux = true;
	config.disableAutoNegotiation = true;
	config.bindAddress = "127.0.0.1";
	rtcUdpSendLimits limits{256, 1200, now + 10000, nullptr, 0};
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, nullptr) == RTC_ERR_INVALID);
	assert(rtcPrepareIceUdpMuxPeerWithUdpLimits(-1, 0, &config, nullptr, nullptr, nullptr, nullptr) == RTC_ERR_INVALID);
	auto bad = limits; bad.maxDatagrams = 0;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &bad) == RTC_ERR_INVALID);
	bad = limits; bad.maxPayloadBytes = 65508;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &bad) == RTC_ERR_INVALID);
	bad = limits; bad.deadlineMonotonicMs = now;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &bad) == RTC_ERR_INVALID);
	bad = limits; bad.destinationPort = 3478;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &bad) == RTC_ERR_INVALID);
	config.enableIceTcp = true;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &limits) == RTC_ERR_INVALID);
	config.enableIceTcp = false; config.enableIceUdpMux = false;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &limits) == RTC_ERR_INVALID);
	config.enableIceUdpMux = true; config.iceTransportPolicy = RTC_TRANSPORT_POLICY_RELAY;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &limits) == RTC_ERR_INVALID);
	config.iceTransportPolicy = RTC_TRANSPORT_POLICY_ALL;
	const char *turn = "turn:user:pass@127.0.0.1:3478";
	config.iceServers = &turn; config.iceServersCount = 1;
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &limits) == RTC_ERR_INVALID);
	config.iceServers = nullptr; config.iceServersCount = 0;
	config.proxyServer = "http://127.0.0.1:8080";
	assert(rtcCreatePeerConnectionWithUdpLimits(&config, &limits) == RTC_ERR_INVALID);
	config.proxyServer = nullptr;
	int pc = rtcCreatePeerConnectionWithUdpLimits(&config, &limits);
	assert(pc >= 0);
	rtcUdpSendStats stats{};
	assert(rtcGetUdpSendStats(pc, nullptr) == RTC_ERR_INVALID);
	assert(rtcGetUdpSendStats(pc, &stats) == RTC_ERR_NOT_AVAIL); // no ICE agent yet
	int channel = rtcCreateDataChannel(pc, "test");
	assert(channel >= 0);
	assert(rtcSetLocalDescription(pc, "offer") == RTC_ERR_SUCCESS);
	assert(rtcGetUdpSendStats(pc, &stats) == RTC_ERR_SUCCESS);
	assert(stats.reservedDatagrams == 0 && stats.sentDatagrams == 0 && stats.sentBytes == 0);
	assert(rtcClosePeerConnectionAndWait(pc, 5000) == RTC_ERR_SUCCESS);
	assert(rtcDeleteDataChannel(channel) == RTC_ERR_SUCCESS);
	assert(rtcDeletePeerConnection(pc) == RTC_ERR_SUCCESS);
	pc = rtcCreatePeerConnection(&config);
	assert(pc >= 0 && rtcGetUdpSendStats(pc, &stats) == RTC_ERR_NOT_AVAIL);
	assert(rtcDeletePeerConnection(pc) == RTC_ERR_SUCCESS);
	std::cout << "udp-send-limits C API PASS\n";
}
