/**
 * Copyright (c) 2025 Alex Potsides
 * Copyright (c) 2025 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "iceudpmuxlistener.hpp"
#include "internals.hpp"
#include "peerconnection.hpp"

namespace rtc::impl {

#if !USE_NICE
namespace {
thread_local const IceUdpMuxListener *CallbackListener = nullptr;
IceUdpMuxRequest copyRequest(const juice_mux_binding_request_t &info, uint64_t id = 0) {
	return {info.local_ufrag, info.remote_ufrag, info.address, info.port, id};
}
}

void IceUdpMuxListener::UnhandledStunRequestCallback(const juice_mux_binding_request *info,
                                                     void *user_ptr) {
	auto listener = static_cast<IceUdpMuxListener *>(user_ptr);
	if (listener) {
		auto previous = std::exchange(CallbackListener, listener);
		scope_guard reset([previous] { CallbackListener = previous; });
		listener->unhandledStunRequestCallback(copyRequest(*info));
	}
}

void IceUdpMuxListener::PendingRequestCallback(const juice_mux_pending_request_t *info,
                                               void *user_ptr) {
	auto listener = static_cast<IceUdpMuxListener *>(user_ptr);
	if (!listener || listener->mStopped)
		return;
	auto previous = std::exchange(CallbackListener, listener);
	scope_guard reset([previous] { CallbackListener = previous; });

	try {
		auto metadata = copyRequest(info->binding, info->request_id);
		listener->removeExpiredRequests();
		{
			std::lock_guard lock(listener->mRequestsMutex);
			if (listener->mStopped || listener->mRequests.size() >= listener->mMaxPendingRequests) {
				juice_mux_reject_request(listener->bindAddress ? listener->bindAddress->c_str() : nullptr,
				                         listener->port, metadata.id);
				return;
			}
			listener->mExpiry.push_back(metadata.id);
			auto expiry = std::prev(listener->mExpiry.end());
			try {
				listener->mRequests.emplace(metadata.id, Request{metadata,
				    std::chrono::steady_clock::now() + std::chrono::milliseconds(listener->mRequestTimeoutMs),
				    {}, false, expiry});
			} catch (...) { listener->mExpiry.erase(expiry); throw; }
		}
		if (!listener->unhandledStunRequestCallback(metadata))
			listener->reject(metadata.id);
	} catch (const std::exception &e) {
		PLOG_WARNING << "Incoming ICE request callback failed: " << e.what();
		try { listener->reject(info->request_id); } catch (...) {}
	} catch (...) {
		try { listener->reject(info->request_id); } catch (...) {}
	}
}
#endif

IceUdpMuxListener::IceUdpMuxListener(uint16_t port, optional<string> bindAddress)
    : port(port), bindAddress(std::move(bindAddress)) {
#if !USE_NICE
	if (juice_mux_listen(this->bindAddress ? this->bindAddress->c_str() : nullptr, port,
	                     UnhandledStunRequestCallback, this) < 0)
		throw std::runtime_error("Failed to register ICE UDP mux listener");
#else
	PLOG_WARNING << "ICE UDP mux is not available with libnice";
#endif
}

IceUdpMuxListener::IceUdpMuxListener(IceUdpMuxListenerConfiguration config,
                                     std::function<void(IceUdpMuxRequest)> callback)
    : port(config.port), bindAddress(std::move(config.bindAddress)),
      unhandledStunRequestCallback(std::move(callback)), mPendingMode(true),
      mMaxPendingRequests(config.maxPendingRequests ? config.maxPendingRequests : 256),
      mRequestTimeoutMs(config.requestTimeoutMs ? config.requestTimeoutMs : 5000) {
	if (!port || mMaxPendingRequests > 4096 || mRequestTimeoutMs > 30000)
		throw std::invalid_argument("Invalid ICE UDP mux listener limits");
#if !USE_NICE
	juice_mux_pending_config_t nativeConfig{mMaxPendingRequests, mRequestTimeoutMs};
	if (juice_mux_listen_pending(bindAddress ? bindAddress->c_str() : nullptr, port,
	                             &nativeConfig, PendingRequestCallback, this) < 0)
		throw std::runtime_error("Failed to register pending ICE UDP mux listener");
#else
	throw std::runtime_error("ICE UDP mux requires libjuice");
#endif
}

IceUdpMuxListener::~IceUdpMuxListener() {
	try { stop(); } catch (const std::exception &e) { PLOG_ERROR << e.what(); }
}

std::unique_lock<std::mutex> IceUdpMuxListener::lockStop() {
	std::unique_lock lock(mStopMutex, std::defer_lock);
#if !USE_NICE
	// Stop waits for this listener's callbacks, so one of them cannot outlive it and must not wait
	if (CallbackListener == this)
		return lock;
#endif
	lock.lock();
	return lock;
}

shared_ptr<rtc::StunUdpMuxMonitor> IceUdpMuxListener::monitorStun(string serverHost,
                                                               uint16_t serverPort) {
	auto lock = lockStop();
	if (mStopped)
		throw std::logic_error("ICE UDP mux listener is stopped");
	return std::make_shared<rtc::StunUdpMuxMonitor>(
	    StunUdpMuxMonitorConfiguration{bindAddress, port, std::move(serverHost), serverPort});
}

void IceUdpMuxListener::stop() {
#if !USE_NICE
	if (CallbackListener == this)
		throw std::logic_error("Stop the ICE UDP mux listener from outside its callback");
#endif
	std::vector<shared_ptr<rtc::PeerConnection>> peers;
	{
		std::lock_guard stopLock(mStopMutex);
		if (mStopped.exchange(true)) return;
#if !USE_NICE
		const char *address = bindAddress ? bindAddress->c_str() : nullptr;
		int result = mPendingMode ? juice_mux_listen_pending(address, port, nullptr, nullptr, nullptr)
		                          : juice_mux_listen(address, port, nullptr, nullptr);
		if (result < 0) {
			mStopped = false;
			throw std::runtime_error("Failed to unregister ICE UDP mux listener");
		}
#endif
		std::lock_guard lock(mRequestsMutex);
		for (auto &[id, request] : mRequests)
			if (auto peer = request.peer.lock()) peers.push_back(std::move(peer));
		mRequests.clear();
		mExpiry.clear();
	}
	// Closing can synchronously invoke arbitrary peer callbacks, including stop().
	for (auto &peer : peers) peer->close();
}

void IceUdpMuxListener::eraseRequest(std::unordered_map<uint64_t, Request>::iterator it) {
	mExpiry.erase(it->second.expiry);
	mRequests.erase(it);
}

void IceUdpMuxListener::removeExpiredRequests() {
	std::vector<shared_ptr<rtc::PeerConnection>> peers;
	{
		std::lock_guard lock(mRequestsMutex);
		auto now = std::chrono::steady_clock::now();
		while (!mExpiry.empty()) {
			auto it = mRequests.find(mExpiry.front());
			if (it->second.expiresAt > now) break;
			if (auto peer = it->second.peer.lock()) peers.push_back(std::move(peer));
			eraseRequest(it);
		}
	}
	// Expiry is also checked by the receive-thread notification callback.
	for (auto &peer : peers) peer->closeAsync();
}

void IceUdpMuxListener::prepare(uint64_t requestId, Configuration config,
                                Description remoteDescription, LocalDescriptionInit localInit,
                                shared_ptr<rtc::PeerConnection> &peer) {
	if (peer)
		throw std::invalid_argument("Peer output must be empty");
#if !USE_NICE
	try {
		removeExpiredRequests();
		{
			std::lock_guard lock(mRequestsMutex);
			auto it = mRequests.find(requestId);
			if (!mPendingMode || mStopped || it == mRequests.end() || it->second.prepared)
				throw std::invalid_argument("Incoming ICE request is no longer available");
			const auto &metadata = it->second.metadata;
			if (!localInit.iceUfrag || *localInit.iceUfrag != metadata.localUfrag ||
			    !localInit.icePwd || localInit.icePwd->size() < 22 || localInit.icePwd->size() > 256 ||
			    remoteDescription.iceUfrag() != metadata.remoteUfrag || !remoteDescription.icePwd() ||
			    remoteDescription.icePwd()->size() < 22 || remoteDescription.icePwd()->size() > 256 ||
			    !remoteDescription.fingerprint() || !remoteDescription.fingerprint()->isValid() ||
			    remoteDescription.type() != Description::Type::Offer ||
			    config.disableFingerprintVerification)
				throw std::invalid_argument("Incoming ICE credentials or remote fingerprint are invalid");
			if (juice_mux_verify_request(bindAddress ? bindAddress->c_str() : nullptr, port,
			                             requestId, localInit.icePwd->c_str()) < 0)
				throw std::invalid_argument("Incoming STUN authentication failed");
			it->second.prepared = true;
			config.enableIceUdpMux = true;
			config.iceUdpMuxDirect = false;
			config.bindAddress = bindAddress;
			config.portRangeBegin = port;
			config.portRangeEnd = port;
			config.disableAutoNegotiation = true;
			config.disableAutoGathering = true;
		}
		// Certificate import and construction must not stall the receive callback's mutex.
		peer = std::make_shared<rtc::PeerConnection>(std::move(config));
		{
			std::lock_guard lock(mRequestsMutex);
			auto it = mRequests.find(requestId);
			if (mStopped || it == mRequests.end() || it->second.expiresAt <= std::chrono::steady_clock::now())
				throw std::runtime_error("Incoming ICE request cancelled during construction");
			it->second.peer = peer;
		}
		// Preserve ownership in the output even when either operation throws.
		peer->setRemoteDescription(std::move(remoteDescription));
		peer->setLocalDescription(Description::Type::Answer, std::move(localInit));
	} catch (...) {
		try { reject(requestId); } catch (...) {}
		if (peer) peer->close();
		throw;
	}
#else
	(void)requestId; (void)config; (void)remoteDescription; (void)localInit;
	throw std::runtime_error("ICE UDP mux requires libjuice");
#endif
}

void IceUdpMuxListener::accept(uint64_t requestId, shared_ptr<rtc::PeerConnection> peer) {
	removeExpiredRequests();
	{
		std::lock_guard lock(mRequestsMutex);
		auto it = mRequests.find(requestId);
		if (mStopped || it == mRequests.end() || !peer || it->second.peer.lock() != peer ||
		    peer->state() == rtc::PeerConnection::State::Closed)
			throw std::invalid_argument("Incoming ICE request is no longer available");
	}
	peer->gatherLocalCandidates();
	auto transport = peer->impl()->getIceTransport();
	if (!transport) throw std::runtime_error("Incoming peer has no ICE transport");
	std::lock_guard lock(mRequestsMutex);
	auto it = mRequests.find(requestId);
	if (mStopped || it == mRequests.end() || it->second.peer.lock() != peer)
		throw std::invalid_argument("Incoming ICE request is no longer available");
	transport->acceptUdpMuxRequest(bindAddress, port, requestId);
	eraseRequest(it);
}

void IceUdpMuxListener::attach(uint64_t requestId, shared_ptr<rtc::PeerConnection> peer) {
#if !USE_NICE
	removeExpiredRequests();
	if (!peer || peer->state() == rtc::PeerConnection::State::Closed ||
	    peer->config()->disableFingerprintVerification)
		throw std::invalid_argument("A live authenticated peer is required");
	if (peer->config()->iceUdpMuxDirect)
		throw std::invalid_argument("A direct ICE UDP mux peer cannot be attached");
	auto local = peer->localDescription();
	auto remote = peer->remoteDescription();
	auto transport = peer->impl()->getIceTransport();
	if (!local || !local->icePwd() || !remote || !transport ||
	    !remote->fingerprint() || !remote->fingerprint()->isValid())
		throw std::invalid_argument("Peer has no configured ICE transport or remote fingerprint");
	std::lock_guard lock(mRequestsMutex);
	auto it = mRequests.find(requestId);
	if (mStopped || it == mRequests.end() || it->second.prepared ||
	    local->iceUfrag() != it->second.metadata.localUfrag ||
	    remote->iceUfrag() != it->second.metadata.remoteUfrag)
		throw std::invalid_argument("Incoming ICE request does not match this peer");
	// Authentication uses the existing peer's password; no identity or SDP is replaced.
	if (juice_mux_verify_request(bindAddress ? bindAddress->c_str() : nullptr, port,
	                             requestId, local->icePwd()->c_str()) < 0)
		throw std::invalid_argument("Incoming STUN authentication failed");
	transport->acceptUdpMuxRequest(bindAddress, port, requestId);
	eraseRequest(it);
#else
	(void)requestId; (void)peer;
	throw std::runtime_error("ICE UDP mux requires libjuice");
#endif
}

void IceUdpMuxListener::reject(uint64_t requestId) {
	shared_ptr<rtc::PeerConnection> peer;
	{
		std::lock_guard lock(mRequestsMutex);
		auto it = mRequests.find(requestId);
		if (it != mRequests.end()) {
			peer = it->second.peer.lock();
			eraseRequest(it);
		}
#if !USE_NICE
		if (mPendingMode && !mStopped)
			juice_mux_reject_request(bindAddress ? bindAddress->c_str() : nullptr, port, requestId);
#endif
	}
	if (peer) peer->close();
}

void IceUdpMuxListener::setAdmission(const optional<IceUdpMuxAdmission> &admission) {
#if !USE_NICE
	std::vector<juice_mux_admission_key_t> keys;
	juice_mux_admission_t native{};
	if (admission) {
		for (const auto &key : admission->keys)
			keys.push_back({key.ufragPrefix.c_str(), key.secret.data(), key.secret.size()});
		native = {admission->context.data(), admission->context.size(), keys.data(),
		          static_cast<int>(keys.size())};
	}
	auto lock = lockStop(); // Stop clears the native admission.
	if (!mPendingMode || mStopped)
		throw std::logic_error("ICE UDP mux admission requires a running pending listener");
	int result = juice_mux_set_admission(bindAddress ? bindAddress->c_str() : nullptr, port,
	                                     admission ? &native : nullptr);
	if (result == JUICE_ERR_INVALID) throw std::invalid_argument("Invalid ICE UDP mux admission");
	if (result < 0) throw std::runtime_error("Failed to set ICE UDP mux admission");
#else
	(void)admission;
	throw std::runtime_error("ICE UDP mux requires libjuice");
#endif
}

IceUdpMuxListenerStats IceUdpMuxListener::stats() const {
#if !USE_NICE
	juice_mux_stats_t result{};
	if (mStopped || juice_mux_get_stats(bindAddress ? bindAddress->c_str() : nullptr,
	                                    port, &result) < 0)
		throw std::runtime_error("ICE UDP mux listener is stopped");
	return {result.received, result.rejected, result.notifications, result.duplicates,
	        static_cast<unsigned int>(result.agents), static_cast<unsigned int>(result.mapped_tuples),
	        static_cast<unsigned int>(result.pending)};
#else
	throw std::runtime_error("ICE UDP mux requires libjuice");
#endif
}

void SetIceUdpMuxProxyProtocol(uint16_t port, const optional<string> &bindAddress,
                               const std::vector<string> &proxies) {
#if !USE_NICE
	if (!port)
		throw std::invalid_argument("ICE UDP mux port is required");
	std::vector<const char *> entries;
	entries.reserve(proxies.size());
	for (const auto &proxy : proxies)
		entries.push_back(proxy.c_str());
	int result = juice_mux_set_proxy_protocol(bindAddress ? bindAddress->c_str() : nullptr, port,
	                                          entries.data(), entries.size());
	if (result == JUICE_ERR_INVALID)
		throw std::invalid_argument("Invalid PROXY protocol relay");
	if (result < 0)
		throw std::runtime_error("Failed to set PROXY protocol relays");
#else
	(void)port;
	(void)bindAddress;
	(void)proxies;
	throw std::runtime_error("ICE UDP mux requires libjuice");
#endif
}

optional<string> GetIceUdpMuxProxyRelay(uint16_t port, const optional<string> &bindAddress,
                                        const string &clientAddress, uint16_t clientPort) {
#if !USE_NICE
	if (!port || !clientPort)
		throw std::invalid_argument("ICE UDP mux and client ports are required");
	char relay[JUICE_MAX_ADDRESS_STRING_LEN];
	int result = juice_mux_get_proxy_relay(bindAddress ? bindAddress->c_str() : nullptr, port,
	                                       clientAddress.c_str(), clientPort, relay, sizeof(relay));
	if (result == JUICE_ERR_NOT_AVAIL)
		return nullopt;
	if (result == JUICE_ERR_INVALID)
		throw std::invalid_argument("Invalid PROXY protocol client address");
	if (result < 0)
		throw std::runtime_error("Failed to look up a PROXY protocol relay");
	return string(relay);
#else
	(void)port;
	(void)bindAddress;
	(void)clientAddress;
	(void)clientPort;
	throw std::runtime_error("ICE UDP mux requires libjuice");
#endif
}

} // namespace rtc::impl
