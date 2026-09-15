/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_ICE_UDP_MUX_LISTENER_H
#define RTC_ICE_UDP_MUX_LISTENER_H

#include "common.hpp"
#include "peerconnection.hpp"
#include "stunudpmuxmonitor.hpp"

namespace rtc {

namespace impl {

struct IceUdpMuxListener;

} // namespace impl

struct IceUdpMuxRequest {
	string localUfrag;
	string remoteUfrag;
	string remoteAddress;
	uint16_t remotePort;
	uint64_t id = 0; // Zero for the legacy notification-only listener.
};

struct IceUdpMuxListenerConfiguration {
	uint16_t port = 0;
	optional<string> bindAddress;
	unsigned int maxPendingRequests = 256;
	unsigned int requestTimeoutMs = 5000;
};

struct IceUdpMuxListenerStats {
	uint64_t received = 0;
	uint64_t rejected = 0;
	uint64_t notifications = 0;
	uint64_t duplicates = 0;
	unsigned int agents = 0;
	unsigned int mappedTuples = 0;
	unsigned int pendingRequests = 0;
};

// Owns an unaccepted peer. Destruction rejects its request and closes the peer;
// accept() transfers the peer to the caller. Keeps the listener implementation alive.
class RTC_CPP_EXPORT PreparedIceUdpMuxPeer final {
public:
	PreparedIceUdpMuxPeer(PreparedIceUdpMuxPeer &&other) noexcept;
	PreparedIceUdpMuxPeer &operator=(PreparedIceUdpMuxPeer &&other) noexcept;
	PreparedIceUdpMuxPeer(const PreparedIceUdpMuxPeer &) = delete;
	PreparedIceUdpMuxPeer &operator=(const PreparedIceUdpMuxPeer &) = delete;
	~PreparedIceUdpMuxPeer();
	shared_ptr<PeerConnection> peer() const;
	shared_ptr<PeerConnection> accept();

private:
	friend class IceUdpMuxListener;
	PreparedIceUdpMuxPeer(shared_ptr<impl::IceUdpMuxListener> listener, uint64_t id,
	                     shared_ptr<PeerConnection> peer);
	void reset() noexcept;
	shared_ptr<impl::IceUdpMuxListener> mListener;
	uint64_t mId = 0;
	shared_ptr<PeerConnection> mPeer;
};

class RTC_CPP_EXPORT IceUdpMuxListener final : private CheshireCat<impl::IceUdpMuxListener> {
public:
	IceUdpMuxListener(uint16_t port, optional<string> bindAddress = nullopt);
	// Retain incoming requests until accepted, rejected, or expired. The callback
	// runs outside the UDP receive lock; queue slow work on an application executor.
	IceUdpMuxListener(IceUdpMuxListenerConfiguration config,
	                  std::function<void(IceUdpMuxRequest)> callback);
	~IceUdpMuxListener();

	void stop();

	uint16_t port() const;

	void OnUnhandledStunRequest(std::function<void(IceUdpMuxRequest)> callback);

	// Verify the retained STUN request before allocating a peer. The returned peer
	// does not receive packets until accept(). Install callbacks before accepting.
	// If configuration throws after allocation, peer still belongs to the caller
	// and must be closed. A request can be prepared only once.
	void prepare(uint64_t requestId, Configuration config, Description remoteDescription,
	             LocalDescriptionInit localInit, shared_ptr<PeerConnection> &peer);
	PreparedIceUdpMuxPeer prepare(uint64_t requestId, Configuration config,
	                             Description remoteDescription, LocalDescriptionInit localInit);
	void accept(uint64_t requestId, shared_ptr<PeerConnection> peer);
	// Authenticate a new source tuple using an existing peer's ICE credentials.
	// Preserves its DTLS identity, SDP, channels and caller ownership on failure.
	void attach(uint64_t requestId, shared_ptr<PeerConnection> peer);
	void reject(uint64_t requestId);
	IceUdpMuxListenerStats stats() const;
	// The monitor inherits this listener's exact bind address/port and remains
	// caller-owned when the listener stops. Stop it separately when no longer needed.
	shared_ptr<StunUdpMuxMonitor> monitorStun(string serverHost, uint16_t serverPort = 3478);

private:
	using CheshireCat<impl::IceUdpMuxListener>::impl;
};

} // namespace rtc

#endif
