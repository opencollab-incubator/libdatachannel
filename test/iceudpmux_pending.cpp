// Incoming connections must answer the retained first request without a retry.
#include "rtc/rtc.h"
#include "rtc/iceudpmuxlistener.hpp"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
constexpr uint16_t port = 49198;
const std::string localUfrag = "incomingLocalUfrag";
const std::string remoteUfrag = "incomingRemoteUfrag";
const std::string localPassword = "incomingLocalPassword00000000";
const std::string remotePassword = "incomingRemotePassword0000000";
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void put16(std::vector<unsigned char> &data, uint16_t value) {
    data.push_back(static_cast<unsigned char>(value >> 8));
    data.push_back(static_cast<unsigned char>(value));
}
void attribute(std::vector<unsigned char> &data, uint16_t type,
               const std::vector<unsigned char> &value) {
    put16(data, type); put16(data, static_cast<uint16_t>(value.size()));
    data.insert(data.end(), value.begin(), value.end());
    while (data.size() % 4) data.push_back(0);
}
std::vector<unsigned char> request(const std::string &ufrag, unsigned char transaction,
                                   bool valid = true, const std::string &password = localPassword) {
    std::vector<unsigned char> data{0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42};
    data.resize(20, transaction);
    auto username = ufrag + ":" + remoteUfrag;
    attribute(data, 0x0006, {username.begin(), username.end()});
    attribute(data, 0x0024, {0x6e, 0x7f, 0xff, 0xff});
    attribute(data, 0x802a, {0, 0, 0, 0, 0, 0, 0, 1});
    attribute(data, 0x0025, {});
    auto length = data.size() + 24 - 20;
    data[2] = static_cast<unsigned char>(length >> 8);
    data[3] = static_cast<unsigned char>(length);
    std::array<unsigned char, 20> mac{};
    unsigned int size = 0;
    require(HMAC(EVP_sha1(), password.data(), int(password.size()), data.data(),
                 data.size(), mac.data(), &size) && size == mac.size(), "STUN MAC generation");
    if (!valid) mac[0] ^= 1;
    attribute(data, 0x0008, {mac.begin(), mac.end()});
    return data;
}
std::string remoteSdp(bool media = true) {
    std::string result = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE 0\r\na=ice-ufrag:" + remoteUfrag + "\r\na=ice-pwd:" + remotePassword +
        "\r\na=fingerprint:sha-256 ";
    for (int i=0;i<32;i++) result += i ? ":AA" : "AA";
    result += "\r\na=setup:actpass\r\n";
    if (media) result += "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "c=IN IP4 0.0.0.0\r\na=mid:0\r\na=sctp-port:5000\r\na=max-message-size:262144\r\n";
    return result;
}
struct Socket {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    Socket() {
        require(fd >= 0, "create UDP socket");
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "bind UDP socket");
    }
    ~Socket() { close(fd); }
    void send(const std::vector<unsigned char> &data) {
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        require(sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr *>(&address),
                       sizeof(address)) == static_cast<ssize_t>(data.size()), "send STUN request");
    }
    bool response(unsigned char transaction) {
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd pending{fd, POLLIN, 0};
            if (poll(&pending, 1, 100) <= 0) continue;
            std::array<unsigned char, 2048> data{};
            auto size = recv(fd, data.data(), data.size(), 0);
            if (size >= 20 && data[0] == 1 && data[1] == 1 && data[8] == transaction) return true;
        }
        return false;
    }
};
struct Requests {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<uint64_t> ids;
    std::atomic<int> callbacks{0};
    static void RTC_API incoming(int, const rtcIceUdpMuxRequest *request, void *ptr) {
        auto &self = *static_cast<Requests *>(ptr);
        require(request->id && request->localUfrag && request->remoteUfrag && request->remoteAddress,
                "request metadata");
        std::lock_guard lock(self.mutex);
        self.ids.push_back(request->id); ++self.callbacks; self.changed.notify_all();
    }
    uint64_t wait(size_t index) {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 3s, [&] { return ids.size() > index; }), "incoming callback");
        return ids[index];
    }
};
struct BlockingCallback {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    int inlineDeleteResult = 0, admissionResult = 0;
    static void RTC_API incoming(int listener, const rtcIceUdpMuxRequest *, void *ptr) {
        auto &self = *static_cast<BlockingCallback *>(ptr);
        self.inlineDeleteResult = rtcDeleteIceUdpMuxListener(listener);
        std::unique_lock lock(self.mutex);
        self.entered = true; self.changed.notify_all();
        self.changed.wait(lock, [&] { return self.released; });
        // Deletion waits for this callback, so admission must not wait for deletion
        self.admissionResult = rtcSetIceUdpMuxAdmission(listener, nullptr);
    }
};
struct ReentrantClose {
    int listener;
    uint64_t requestId;
    std::atomic<int> calls{0};
    static void RTC_API state(int, rtcState state, void *ptr) {
        if (state != RTC_CLOSED) return;
        auto &self = *static_cast<ReentrantClose *>(ptr);
        require(rtcRejectIceUdpMuxRequest(self.listener, self.requestId) == 0, "reentrant rejection");
        require(rtcDeleteIceUdpMuxListener(self.listener) == 0, "reentrant listener deletion");
        ++self.calls;
    }
};
void reentrantClose(bool stop) {
    Requests requests;
    rtcIceUdpMuxListenerConfiguration cfg{"127.0.0.1", port, 8, 5000};
    int listener = rtcCreateIceUdpMuxListener(&cfg, Requests::incoming, &requests);
    require(listener >= 0, "reentrancy listener");
    Socket client;
    client.send(request(localUfrag, 13));
    auto id = requests.wait(0);
    rtcConfiguration config{};
    rtcLocalDescriptionInit init{localUfrag.c_str(), localPassword.c_str()};
    auto sdp = remoteSdp();
    int peer = -1;
    require(rtcPrepareIceUdpMuxPeer(listener, id, &config, sdp.c_str(), &init, &peer) == 0, "prepare reentrancy peer");
    ReentrantClose context{listener, id};
    rtcSetUserPointer(peer, &context);
    rtcSetStateChangeCallback(peer, ReentrantClose::state);
    if (stop) require(rtcDeleteIceUdpMuxListener(listener) == 0, "stop with reentrant callback");
    else require(rtcRejectIceUdpMuxRequest(listener, id) == 0, "reject with reentrant callback");
    require(context.calls == 1, "closed callback completed exactly once");
    require(rtcClosePeerConnectionAndWait(peer, 5000) == 0, "reentrant peer cleanup");
    require(rtcDeletePeerConnection(peer) == 0, "reentrant handle deletion");
}
void preparedOwnership() {
    Requests requests;
    rtc::IceUdpMuxListener listener({port, "127.0.0.1", 8, 5000}, [&](rtc::IceUdpMuxRequest request) {
        rtcIceUdpMuxRequest metadata{request.id, request.localUfrag.c_str(), request.remoteUfrag.c_str(),
            request.remoteAddress.c_str(), request.remotePort};
        Requests::incoming(0, &metadata, &requests);
    });
    Socket client;
    client.send(request(localUfrag, 14));
    auto id = requests.wait(0);
    std::shared_ptr<rtc::PeerConnection> retained;
    {
        auto prepared = listener.prepare(id, {}, rtc::Description(remoteSdp(), "offer"), {localUfrag, localPassword});
        retained = prepared.peer();
        auto moved = std::move(prepared);
        require(!prepared.peer() && moved.peer() == retained, "prepared peer is move-only ownership");
    }
    require(retained->closeAndWait(5s), "scope exit tears down unaccepted peer");
    require(listener.stats().pendingRequests == 0, "scope exit rejects request");
    client.send(request(localUfrag, 15));
    id = requests.wait(1);
    auto prepared = listener.prepare(id, {}, rtc::Description(remoteSdp(), "offer"), {localUfrag, localPassword});
    auto peer = prepared.accept();
    require(!prepared.peer() && client.response(15), "accept transfers ownership and resumes request");
    require(peer->closeAsync().wait_for(5s) == std::future_status::ready, "C++ asynchronous close future");
}
void cleanup(int peer) {
    require(rtcClosePeerConnectionAndWait(peer, 5000) == 0, "complete peer teardown");
    require(rtcDeletePeerConnection(peer) == 0, "delete peer");
}
// A direct peer on the listener's port answers its own checks; unknown ufrags still go to admission.
void directPeer() {
    Requests requests;
    rtcIceUdpMuxListenerConfiguration cfg{"127.0.0.1", port, 8, 5000};
    int listener = rtcCreateIceUdpMuxListener(&cfg, Requests::incoming, &requests);
    require(listener >= 0, "direct listener");
    rtcConfiguration config{};
    config.bindAddress = "127.0.0.1";
    config.enableIceUdpMux = true;
    config.iceUdpMuxDirect = true;
    config.disableAutoNegotiation = true;
    config.portRangeBegin = config.portRangeEnd = port;
    int peer = rtcCreatePeerConnection(&config);
    require(peer >= 0, "create direct peer");
    auto sdp = remoteSdp();
    require(rtcSetRemoteDescription(peer, sdp.c_str(), "offer") == 0, "direct remote description");
    rtcLocalDescriptionInit init{localUfrag.c_str(), localPassword.c_str()};
    require(rtcSetLocalDescriptionEx(peer, "answer", &init) == 0, "direct local description");
    Socket client;
    client.send(request(localUfrag, 16));
    require(client.response(16), "direct peer answers without admission");
    rtcIceUdpMuxListenerStats stats{};
    require(rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && stats.mappedTuples == 1 &&
            requests.callbacks == 0, "direct peer maps its source natively");
    Socket unknown;
    unknown.send(request("unknownUfrag", 17));
    auto id = requests.wait(0);
    require(rtcRejectIceUdpMuxRequest(listener, id) == 0, "reject unknown ufrag");
    cleanup(peer);
    require(rtcDeleteIceUdpMuxListener(listener) == 0, "delete direct listener");
}
// Requests that no key or peer authenticates never reach the callback.
void admission() {
    const std::string context = "context", secret = "admission secret", keyed = "keyedLocalUfrag";
    std::array<unsigned char, 32> digest{};
    auto input = context + keyed;
    require(HMAC(EVP_sha256(), secret.data(), int(secret.size()),
                 reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest.data(),
                 nullptr), "derive admission password");
    std::array<unsigned char, 33> encoded{};
    EVP_EncodeBlock(encoded.data(), digest.data(), 24);
    const std::string derived(reinterpret_cast<const char *>(encoded.data()));
    Requests requests;
    rtcIceUdpMuxListenerConfiguration cfg{"127.0.0.1", port, 8, 5000};
    int listener = rtcCreateIceUdpMuxListener(&cfg, Requests::incoming, &requests);
    require(listener >= 0, "admission listener");
    rtcIceUdpMuxAdmissionKey key{"keyed", secret.data(), int(secret.size())};
    rtcIceUdpMuxAdmission settings{context.data(), int(context.size()), &key, 1};
    require(rtcSetIceUdpMuxAdmission(listener, &settings) == 0, "set admission");
    Socket forged, unknown, valid, open;
    forged.send(request(keyed, 21));
    unknown.send(request("unknownLocalUfrag", 22, true, derived));
    valid.send(request(keyed, 23, true, derived));
    auto id = requests.wait(0);
    rtcIceUdpMuxListenerStats stats{};
    require(rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && stats.rejected == 2 &&
            stats.pendingRequests == 1 && requests.callbacks == 1, "only the keyed request is held");
    rtcConfiguration config{};
    rtcLocalDescriptionInit init{keyed.c_str(), derived.c_str()};
    auto sdp = remoteSdp();
    int peer = -1;
    require(rtcPrepareIceUdpMuxPeer(listener, id, &config, sdp.c_str(), &init, &peer) == 0,
            "the application derives the same password");
    cleanup(peer);
    require(rtcSetIceUdpMuxAdmission(listener, nullptr) == 0, "clear admission");
    open.send(request("unknownLocalUfrag", 24));
    requests.wait(1);
    require(rtcDeleteIceUdpMuxListener(listener) == 0, "delete admission listener");
    require(rtcSetIceUdpMuxAdmission(listener, &settings) < 0, "admission needs a listener");
}
}

int main() {
    try {
        rtcInitLogger(RTC_LOG_ERROR, nullptr);
        Requests requests;
        rtcIceUdpMuxListenerConfiguration listenerConfig{"127.0.0.1", port, 8, 5000};
        int listener = rtcCreateIceUdpMuxListener(&listenerConfig, Requests::incoming, &requests);
        require(listener >= 0, "create listener");
        Socket client;
        auto first = request(localUfrag, 1);
        client.send(first);
        auto id = requests.wait(0);
        std::this_thread::sleep_for(50ms); // Acceptance is asynchronous; the client sends no retry.
        rtcConfiguration config{};
        rtcLocalDescriptionInit init{localUfrag.c_str(), localPassword.c_str()};
        auto sdp = remoteSdp();
        int peer = -1;
        require(rtcPrepareIceUdpMuxPeer(listener, id, &config, sdp.c_str(), &init, &peer) == 0 && peer >= 0,
                "prepare retained request");
        rtcIceUdpMuxListenerStats stats{};
        require(rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && stats.agents == 0 && stats.mappedTuples == 0,
                "prepared peer cannot receive before wrapper setup");
        require(rtcAcceptIceUdpMuxPeer(listener, id, peer) == 0, "accept prepared peer");
        require(client.response(1), "first request answered without a client retransmission");
        client.send(first);
        require(client.response(1), "established STUN checks remain native");
        require(requests.callbacks == 1, "no second callback after acceptance");
        require(rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && stats.mappedTuples == 1,
                "only authenticated tuple assigned");
        // A second authenticated source reuses the same peer and its identity.
        Socket alternate;
        alternate.send(request(localUfrag, 11));
        auto alternateId = requests.wait(1);
        require(rtcAttachIceUdpMuxPeer(listener, alternateId, peer) == 0, "attach existing peer");
        require(alternate.response(11), "second tuple receives its first response");
        require(rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && stats.agents == 1 && stats.mappedTuples == 2,
                "one agent serves both authenticated tuples");
        Socket badAlternate;
        badAlternate.send(request(localUfrag, 12, false));
        auto badAlternateId = requests.wait(2);
        require(rtcAttachIceUdpMuxPeer(listener, badAlternateId, peer) < 0, "reject forged alternate tuple");
        rtcRejectIceUdpMuxRequest(listener, badAlternateId);
        require(rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && stats.agents == 1 && stats.mappedTuples == 2,
                "failed attachment retains original peer and mappings");
        // Existing tuple still answers the original signed request.
        client.send(first);
        require(client.response(1), "old tuple remains usable after rejected attachment");
        cleanup(peer);

        Socket forged;
        forged.send(request("forgedUfrag", 2, false));
        auto bad = requests.wait(3);
        auto creations = rtcGetPeerConnectionCreationAttempts();
        rtcLocalDescriptionInit badInit{"forgedUfrag", localPassword.c_str()};
        require(rtcPrepareIceUdpMuxPeer(listener, bad, &config, sdp.c_str(), &badInit, &peer) < 0 && peer == -1,
                "forged STUN rejected before construction");
        require(rtcGetPeerConnectionCreationAttempts() == creations, "authentication failure creates no peer");

        Socket duplicate;
        auto repeated = request("duplicateUfrag", 3);
        duplicate.send(repeated);
        auto repeatedId = requests.wait(4);
        for (int i=0;i<20;i++) duplicate.send(repeated);
        std::this_thread::sleep_for(50ms);
        require(requests.callbacks == 5, "pending duplicates share one notification");
        require(rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && stats.duplicates >= 20,
                "duplicates counted natively");
        require(rtcRejectIceUdpMuxRequest(listener, repeatedId) == 0, "reject pending request");

        Socket broken;
        broken.send(request("brokenSdpUfrag", 4));
        auto brokenId = requests.wait(5);
        rtcLocalDescriptionInit brokenInit{"brokenSdpUfrag", localPassword.c_str()};
        auto brokenSdp = remoteSdp(false);
        require(rtcPrepareIceUdpMuxPeer(listener, brokenId, &config, brokenSdp.c_str(), &brokenInit, &peer) < 0 && peer >= 0,
                "configuration failure returns ownership of allocated peer");
        cleanup(peer);
        require(rtcDeleteIceUdpMuxListener(listener) == 0, "delete listener");

        BlockingCallback blocking;
        listener = rtcCreateIceUdpMuxListener(&listenerConfig, BlockingCallback::incoming, &blocking);
        require(listener >= 0, "replace listener");
        Socket waiting;
        waiting.send(request("closingUfrag", 5));
        {
            std::unique_lock lock(blocking.mutex);
            require(blocking.changed.wait_for(lock, 3s, [&] { return blocking.entered; }), "callback entered");
        }
        require(blocking.inlineDeleteResult < 0, "inline listener deletion fails without deadlocking");
        auto firstDelete = std::async(std::launch::async, [listener] { return rtcDeleteIceUdpMuxListener(listener); });
        require(firstDelete.wait_for(30ms) == std::future_status::timeout, "deletion waits for callback");
        auto secondDelete = std::async(std::launch::async, [listener] { return rtcDeleteIceUdpMuxListener(listener); });
        require(secondDelete.wait_for(30ms) == std::future_status::timeout, "concurrent deletion also waits");
        // Admission fails only once a deletion has marked the listener stopped, which stats reports
        auto stopping = std::chrono::steady_clock::now() + 3s;
        while (rtcGetIceUdpMuxListenerStats(listener, &stats) == 0 && std::chrono::steady_clock::now() < stopping)
            std::this_thread::sleep_for(1ms);
        require(rtcGetIceUdpMuxListenerStats(listener, &stats) < 0, "deletion marks the listener stopped");
        {
            std::lock_guard lock(blocking.mutex);
            blocking.released = true; blocking.changed.notify_all();
        }
        require(firstDelete.get() == 0 && secondDelete.get() == 0, "concurrent deletes complete safely");
        require(blocking.admissionResult < 0, "admission from a callback fails while deletion waits");
        std::cout << "incoming ICE PASS retained-first-request=true duplicate-callbacks=0 invalid-stun-creations=0 failure-ownership=true\n";
        reentrantClose(false);
        reentrantClose(true);
        preparedOwnership();
        directPeer();
        admission();
        std::cout << "incoming ICE PASS reentrantReject=true reentrantStop=true existingPeerAttachment=true directPeer=true admission=true\n";
        rtcCleanup();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
