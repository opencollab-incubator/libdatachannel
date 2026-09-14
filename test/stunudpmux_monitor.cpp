/* SPDX-License-Identifier: MPL-2.0 */
#include "rtc/rtc.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

struct Socket {
	int fd;
	uint16_t port;
	explicit Socket(int family) : fd(socket(family, SOCK_DGRAM, 0)) {
		assert(fd >= 0);
		sockaddr_storage address{};
		socklen_t len;
		if (family == AF_INET) {
			auto *value = reinterpret_cast<sockaddr_in *>(&address);
			value->sin_family = family;
			value->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			len = sizeof(*value);
		} else {
			auto *value = reinterpret_cast<sockaddr_in6 *>(&address);
			value->sin6_family = family;
			value->sin6_addr = in6addr_loopback;
			len = sizeof(*value);
		}
		assert(bind(fd, reinterpret_cast<sockaddr *>(&address), len) == 0);
		assert(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &len) == 0);
		port = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in *>(&address)->sin_port :
		                                reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
	}
	~Socket() { close(fd); }
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;
};

static uint16_t availablePort(int family) { return Socket(family).port; }

static void respond(Socket &server, int family, uint16_t expectedPort, uint16_t mappedPort) {
	pollfd pending{server.fd, POLLIN, 0};
	assert(poll(&pending, 1, 18000) == 1);
	std::array<unsigned char, 2048> request{};
	sockaddr_storage source{};
	socklen_t len = sizeof(source);
	auto size = recvfrom(server.fd, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&source), &len);
	assert(size >= 20 && request[0] == 0 && request[1] == 1 && request[4] == 0x21 && request[5] == 0x12);
	auto port = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in *>(&source)->sin_port :
	                                   reinterpret_cast<sockaddr_in6 *>(&source)->sin6_port);
	assert(port == expectedPort);
	std::vector<unsigned char> response(request.begin(), request.begin() + 20);
	response[0] = 1;
	response[1] = 1;
	response[2] = 0;
	response[3] = family == AF_INET ? 12 : 24;
	uint16_t xport = mappedPort ^ 0x2112;
	response.insert(response.end(), {0, 0x20, 0, static_cast<unsigned char>(family == AF_INET ? 8 : 20),
	                                0, static_cast<unsigned char>(family == AF_INET ? 1 : 2),
	                                static_cast<unsigned char>(xport >> 8), static_cast<unsigned char>(xport)});
	std::array<unsigned char, 16> mapped{};
	const char *address = family == AF_INET ? "198.51.100.10" : "2001:db8::10";
	assert(inet_pton(family, address, mapped.data()) == 1);
	for (int i = 0; i < (family == AF_INET ? 4 : 16); ++i) response.push_back(mapped[i] ^ request[4 + i]);
	assert(sendto(server.fd, response.data(), response.size(), 0, reinterpret_cast<sockaddr *>(&source), len) ==
	       static_cast<ssize_t>(response.size()));
}

static rtcStunBinding observed(int monitor, uint64_t responses) {
	rtcStunBinding value{};
	for (int i = 0; i < 400; ++i) {
		assert(rtcGetStunUdpMuxBinding(monitor, 0, &value) == RTC_ERR_SUCCESS);
		if (value.successfulResponses >= responses) return value;
		std::this_thread::sleep_for(5ms);
	}
	throw std::runtime_error("STUN snapshot did not update");
}

static void RTC_API unexpected(int, const rtcIceUdpMuxRequest *, void *) {
	assert(!"discovery created an incoming peer");
}

int main(int argc, char **argv) {
	assert(argc == 2);
	rtcInitLogger(RTC_LOG_FATAL, nullptr);
	int family = std::strcmp(argv[1], "6") == 0 ? AF_INET6 : AF_INET;
	const char *host = family == AF_INET ? "127.0.0.1" : "::1";
	Socket server(family);
	auto port = availablePort(family);
	const auto peersBefore = rtcGetPeerConnectionCreationAttempts();
	rtcIceUdpMuxListenerConfiguration config{host, port, 0, 0};
	int listener = rtcCreateIceUdpMuxListener(&config, unexpected, nullptr);
	assert(listener > 0);
	assert(rtcCreateStunUdpMuxMonitor(nullptr) == RTC_ERR_INVALID);
	assert(rtcCreateIceUdpMuxStunMonitor(listener, nullptr, server.port) == RTC_ERR_INVALID);
	assert(rtcCreateIceUdpMuxStunMonitor(listener, host, 0) == RTC_ERR_INVALID);
	int monitor = rtcCreateIceUdpMuxStunMonitor(listener, host, server.port);
	assert(monitor > 0);
	rtcStunBinding value{};
	assert(rtcGetStunUdpMuxBinding(monitor, 0, nullptr) == RTC_ERR_INVALID);
	assert(rtcGetStunUdpMuxBinding(monitor, 1, &value) == RTC_ERR_NOT_AVAIL);
	assert(rtcGetStunUdpMuxBinding(monitor, 0, &value) == RTC_ERR_SUCCESS);
	assert(value.lastSuccessAgeMs == UINT64_MAX && value.mappedAddress[0] == 0);
	respond(server, family, port, 40000);
	value = observed(monitor, 1);
	assert(value.mappingRevision == 1 && value.mappedPort == 40000 && value.lastSuccessAgeMs < 2000);
	assert(std::strcmp(value.serverAddress, host) == 0 && value.serverPort == server.port);
	rtcIceUdpMuxListenerStats stats{};
	assert(rtcGetIceUdpMuxListenerStats(listener, &stats) == RTC_ERR_SUCCESS);
	assert(stats.agents == 1 && stats.mappedTuples == 0 && stats.pendingRequests == 0);
	assert(rtcDeleteIceUdpMuxListener(listener) == RTC_ERR_SUCCESS);
	respond(server, family, port, 40001); // monitor retains its own socket ownership
	auto changed = observed(monitor, 2);
	assert(changed.mappingRevision == 2 && changed.mappedPort == 40001);
	assert(value.mappingRevision == 1 && value.mappedPort == 40000); // owned copy remains intact

	std::atomic<bool> stop{false};
	std::atomic<unsigned int> reads{0};
	std::thread reader([&] {
		while (!stop) {
			rtcStunBinding current{};
			int status = rtcGetStunUdpMuxBinding(monitor, 0, &current);
			assert(status == RTC_ERR_SUCCESS || status == RTC_ERR_INVALID || status == RTC_ERR_FAILURE);
			if (status == RTC_ERR_SUCCESS)
				assert(current.mappingRevision == 2 && current.mappedPort == 40001 && current.successfulResponses == 2);
			++reads;
			std::this_thread::yield();
		}
	});
	while (reads < 20) std::this_thread::yield();
	assert(rtcDeleteStunUdpMuxMonitor(monitor) == RTC_ERR_SUCCESS);
	stop = true;
	reader.join();
	assert(rtcGetStunUdpMuxBinding(monitor, 0, &changed) == RTC_ERR_INVALID);
	assert(value.mappedPort == 40000 && value.successfulResponses == 1);

	// Exercise the C++ factory and RAII ownership independently of C API handles.
	rtc::IceUdpMuxListenerConfiguration cppConfig;
	cppConfig.bindAddress = host;
	cppConfig.port = port;
	rtc::IceUdpMuxListener cppListener(cppConfig, [](auto) { assert(false); });
	auto cppMonitor = cppListener.monitorStun(host, server.port);
	cppListener.stop();
	respond(server, family, port, 41000);
	rtc::optional<rtc::StunBinding> copy;
	for (int i = 0; i < 400; ++i) {
		copy = cppMonitor->binding(0);
		if (copy && copy->successfulResponses == 1) break;
		std::this_thread::sleep_for(5ms);
	}
	assert(copy && copy->mappedPort == 41000 && copy->lastSuccessAgeMs);
	cppMonitor->stop();
	cppMonitor->stop();
	try { cppMonitor->binding(0); assert(false); } catch (const std::logic_error &) {}
	assert(copy->mappedPort == 41000 && copy->serverAddress == host);
	assert(rtcGetPeerConnectionCreationAttempts() == peersBefore);
	std::cout << "STUN C/C++ monitor " << argv[1] << ": shared socket, owned snapshots, concurrent close, zero peers PASS\n";
}
