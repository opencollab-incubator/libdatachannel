#include "rtc/rtc.hpp"
#include "test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace rtc;
using namespace std;

TestResult test_sctp_stats() {
	InitLogger(LogLevel::Debug);

	PeerConnection pc1;
	PeerConnection pc2;

	pc1.onLocalDescription([&pc2](Description sdp) { pc2.setRemoteDescription(string(sdp)); });
	pc1.onLocalCandidate([&pc2](Candidate candidate) { pc2.addRemoteCandidate(string(candidate)); });
	pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(string(sdp)); });
	pc2.onLocalCandidate([&pc1](Candidate candidate) { pc1.addRemoteCandidate(string(candidate)); });

	const size_t messageSize = 16 * 1024;
	const int messageCount = 32;
	atomic<int> received = 0;
	shared_ptr<DataChannel> dc2;
	pc2.onDataChannel([&dc2, &received](shared_ptr<DataChannel> dc) {
		dc->onMessage([&received](variant<binary, string>) { ++received; });
		std::atomic_store(&dc2, dc);
	});

	auto dc1 = pc1.createDataChannel("stats");

	int attempts = 10;
	while ((!std::atomic_load(&dc2) || !dc1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected || !dc1->isOpen())
		return TestResult(false, "PeerConnection is not connected");

	auto idle = pc1.sctpStats();
	if (!idle)
		return TestResult(false, "A connected association reports no SCTP stats");

	cout << "Idle: sendQueueBytes=" << idle->sendQueueBytes
	     << " sendBufferSize=" << idle->sendBufferSize
	     << " receiveQueueBytes=" << idle->receiveQueueBytes << endl;

	if (idle->sendBufferSize < 1024 * 1024)
		return TestResult(false, "Send buffer size is smaller than the 1 MiB default");

	// Delayed SACKs keep at least the first message queued
	binary payload(messageSize, byte(0x42));
	uint32_t mostQueued = 0;
	for (int i = 0; i < messageCount; ++i) {
		dc1->send(payload);
		if (auto stats = pc1.sctpStats())
			mostQueued = std::max(mostQueued, stats->sendQueueBytes);
	}

	cout << "Most queued while sending: " << mostQueued << " bytes" << endl;
	if (mostQueued < messageSize)
		return TestResult(false, "Send queue never held a whole unacknowledged message");

	attempts = 10;
	while (received < messageCount && attempts--)
		this_thread::sleep_for(1s);

	if (received < messageCount)
		return TestResult(false, "Not every message arrived");

	uint32_t remaining = 0;
	attempts = 20;
	do {
		auto stats = pc1.sctpStats();
		if (!stats)
			return TestResult(false, "SCTP stats vanished while connected");
		remaining = stats->sendQueueBytes;
		if (remaining == 0)
			break;
		this_thread::sleep_for(100ms);
	} while (attempts--);

	if (remaining != 0)
		return TestResult(false, "Send queue did not drain once everything was acknowledged");

	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}
