/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_ICE_UDP_MUX_LISTENER_H
#define RTC_IMPL_ICE_UDP_MUX_LISTENER_H

#include "common.hpp"

#include "rtc/iceudpmuxlistener.hpp"

#if !USE_NICE
#include <juice/juice.h>
#endif

#include <atomic>
#include <chrono>
#include <list>
#include <unordered_map>

namespace rtc::impl {

struct IceUdpMuxListener final {
	IceUdpMuxListener(uint16_t port, optional<string> bindAddress = nullopt);
	IceUdpMuxListener(IceUdpMuxListenerConfiguration config,
	                  std::function<void(IceUdpMuxRequest)> callback);
	~IceUdpMuxListener();

	void stop();
	void prepare(uint64_t requestId, Configuration config, Description remoteDescription,
	             LocalDescriptionInit localInit, shared_ptr<rtc::PeerConnection> &peer);
	void accept(uint64_t requestId, shared_ptr<rtc::PeerConnection> peer);
	void attach(uint64_t requestId, shared_ptr<rtc::PeerConnection> peer);
	void reject(uint64_t requestId);
	IceUdpMuxListenerStats stats() const;
	shared_ptr<rtc::StunUdpMuxMonitor> monitorStun(string serverHost, uint16_t serverPort);

	const uint16_t port;
	const optional<string> bindAddress;
	synchronized_callback<IceUdpMuxRequest> unhandledStunRequestCallback;

private:
#if !USE_NICE
	static void UnhandledStunRequestCallback(const juice_mux_binding_request *info, void *user_ptr);
	static void PendingRequestCallback(const juice_mux_pending_request_t *info, void *user_ptr);
#endif
	struct Request {
		IceUdpMuxRequest metadata;
		std::chrono::steady_clock::time_point expiresAt;
		weak_ptr<rtc::PeerConnection> peer;
		bool prepared = false;
		std::list<uint64_t>::iterator expiry;
	};
	void removeExpiredRequests(); // Takes its own lock; never closes peers under it.
	void eraseRequest(std::unordered_map<uint64_t, Request>::iterator it);

	std::atomic<bool> mStopped{false};
	std::mutex mStopMutex;
	bool mPendingMode = false;
	unsigned int mMaxPendingRequests = 256;
	unsigned int mRequestTimeoutMs = 5000;
	std::mutex mRequestsMutex;
	std::unordered_map<uint64_t, Request> mRequests;
	std::list<uint64_t> mExpiry;
};

}

#endif
