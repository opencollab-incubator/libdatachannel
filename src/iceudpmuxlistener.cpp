/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "iceudpmuxlistener.hpp"

#include "impl/iceudpmuxlistener.hpp"

namespace rtc {

PreparedIceUdpMuxPeer::PreparedIceUdpMuxPeer(shared_ptr<impl::IceUdpMuxListener> listener,
                                             uint64_t id, shared_ptr<PeerConnection> peer)
    : mListener(std::move(listener)), mId(id), mPeer(std::move(peer)) {}

PreparedIceUdpMuxPeer::PreparedIceUdpMuxPeer(PreparedIceUdpMuxPeer &&other) noexcept {
	*this = std::move(other);
}

PreparedIceUdpMuxPeer &PreparedIceUdpMuxPeer::operator=(PreparedIceUdpMuxPeer &&other) noexcept {
	if (this != &other) {
		reset();
		mListener = std::move(other.mListener);
		mId = std::exchange(other.mId, 0);
		mPeer = std::move(other.mPeer);
	}
	return *this;
}

PreparedIceUdpMuxPeer::~PreparedIceUdpMuxPeer() { reset(); }

void PreparedIceUdpMuxPeer::reset() noexcept {
	auto peer = std::move(mPeer);
	auto listener = std::move(mListener);
	auto id = std::exchange(mId, 0);
	if (listener && id) { try { listener->reject(id); } catch (...) {} }
	if (peer) { try { peer->closeAsync(); } catch (...) {} }
}

shared_ptr<PeerConnection> PreparedIceUdpMuxPeer::peer() const { return mPeer; }

shared_ptr<PeerConnection> PreparedIceUdpMuxPeer::accept() {
	if (!mListener || !mPeer) throw std::logic_error("Prepared peer has already been accepted or moved");
	mListener->accept(mId, mPeer);
	mId = 0;
	mListener.reset();
	return std::move(mPeer);
}

IceUdpMuxListener::IceUdpMuxListener(uint16_t port, optional<string> bindAddress)
    : CheshireCat<impl::IceUdpMuxListener>(port, std::move(bindAddress)) {}

IceUdpMuxListener::IceUdpMuxListener(IceUdpMuxListenerConfiguration config,
                                     std::function<void(IceUdpMuxRequest)> callback)
    : CheshireCat<impl::IceUdpMuxListener>(std::move(config), std::move(callback)) {}

IceUdpMuxListener::~IceUdpMuxListener() {}

void IceUdpMuxListener::stop() { impl()->stop(); }

uint16_t IceUdpMuxListener::port() const { return impl()->port; }

void IceUdpMuxListener::OnUnhandledStunRequest(std::function<void(IceUdpMuxRequest)> callback) {
	impl()->unhandledStunRequestCallback = callback;
}

void IceUdpMuxListener::prepare(uint64_t requestId, Configuration config,
                                Description remoteDescription, LocalDescriptionInit localInit,
                                shared_ptr<PeerConnection> &peer) {
	impl()->prepare(requestId, std::move(config), std::move(remoteDescription),
	                std::move(localInit), peer);
}

PreparedIceUdpMuxPeer IceUdpMuxListener::prepare(uint64_t requestId, Configuration config,
                                                Description remoteDescription, LocalDescriptionInit localInit) {
	shared_ptr<PeerConnection> peer;
	impl()->prepare(requestId, std::move(config), std::move(remoteDescription), std::move(localInit), peer);
	return PreparedIceUdpMuxPeer(impl(), requestId, std::move(peer));
}

void IceUdpMuxListener::accept(uint64_t requestId, shared_ptr<PeerConnection> peer) {
	impl()->accept(requestId, std::move(peer));
}

void IceUdpMuxListener::reject(uint64_t requestId) { impl()->reject(requestId); }

void IceUdpMuxListener::attach(uint64_t requestId, shared_ptr<PeerConnection> peer) {
	impl()->attach(requestId, std::move(peer));
}

IceUdpMuxListenerStats IceUdpMuxListener::stats() const { return impl()->stats(); }

shared_ptr<StunUdpMuxMonitor> IceUdpMuxListener::monitorStun(string serverHost, uint16_t serverPort) {
	return impl()->monitorStun(std::move(serverHost), serverPort);
}

} // namespace rtc
