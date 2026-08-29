#include <gtest/gtest.h>
#include "LockFreeTripleBuffer.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

// 1. ConsumeMode::Try Non-Blocking Read/Write Test
TEST(LockFreeTripleBufferTest, TryModeNonBlockingReadWrite) {
    LockFreeTripleBuffer<int> buffer;
    int val = -1;

    // Initially, buffer has no new data; Try mode must return false immediately
    EXPECT_FALSE(buffer.consume<ConsumeMode::Try>(val));
    EXPECT_EQ(val, -1);

    // Produce an item and consume it
    buffer.produce(42);
    EXPECT_TRUE(buffer.consume<ConsumeMode::Try>(val));
    EXPECT_EQ(val, 42);

    // After consuming, buffer has no new data again
    EXPECT_FALSE(buffer.consume<ConsumeMode::Try>(val));
    EXPECT_EQ(val, 42); // Value remains unchanged

    // Overwrite test: produce multiple values without consuming
    buffer.produce(100);
    buffer.produce(200);
    buffer.produce(300);

    // Consumer receives the most recently produced data
    EXPECT_TRUE(buffer.consume<ConsumeMode::Try>(val));
    EXPECT_EQ(val, 300);

    // Immediate next consume returns false
    EXPECT_FALSE(buffer.consume<ConsumeMode::Try>(val));

    // Verify multiple alternating produce/consume cycles to rotate through all buffers
    for (int i = 1; i <= 20; ++i) {
        buffer.produce(i * 10);
        EXPECT_TRUE(buffer.consume<ConsumeMode::Try>(val));
        EXPECT_EQ(val, i * 10);
        EXPECT_FALSE(buffer.consume<ConsumeMode::Try>(val));
    }

    // Move semantics / non-trivial type test with std::string
    LockFreeTripleBuffer<std::string> strBuffer;
    std::string strVal;

    EXPECT_FALSE(strBuffer.consume<ConsumeMode::Try>(strVal));

    strBuffer.produce("LockFreeTripleBuffer C++20");
    EXPECT_TRUE(strBuffer.consume<ConsumeMode::Try>(strVal));
    EXPECT_EQ(strVal, "LockFreeTripleBuffer C++20");

    EXPECT_FALSE(strBuffer.consume<ConsumeMode::Try>(strVal));
}

// 2. ConsumeMode::Wait Thread Sleep and notify_one Wake-Up Test
TEST(LockFreeTripleBufferTest, WaitModeThreadSleepAndNotifyWakeup) {
    LockFreeTripleBuffer<int> buffer;

    std::atomic<bool> consumerReady{false};
    std::atomic<bool> consumerFinished{false};
    int consumedValue = -1;

    auto startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point wakeUpTime;

    std::jthread consumerThread([&]() {
        consumerReady.store(true, std::memory_order_release);
        // This will block and sleep until producer calls produce() -> notify_one()
        bool success = buffer.consume<ConsumeMode::Wait>(consumedValue);
        wakeUpTime = std::chrono::steady_clock::now();
        EXPECT_TRUE(success);
        consumerFinished.store(true, std::memory_order_release);
    });

    // Wait until consumer is actively running
    while (!consumerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Sleep on main thread to guarantee consumer enters wait state
    constexpr auto sleepDuration = std::chrono::milliseconds(100);
    std::this_thread::sleep_for(sleepDuration);

    // Confirm consumer is still blocked and has not finished
    EXPECT_FALSE(consumerFinished.load(std::memory_order_acquire));
    EXPECT_EQ(consumedValue, -1);

    // Produce data - this invokes _sharedState.notify_one() internally
    constexpr int expectedData = 98765;
    buffer.produce(expectedData);

    consumerThread.join();

    EXPECT_TRUE(consumerFinished.load(std::memory_order_acquire));
    EXPECT_EQ(consumedValue, expectedData);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(wakeUpTime - startTime);
    EXPECT_GE(elapsed.count(), 80); // Ensure consumer stayed asleep during the wait period
}

// 3. Multi-Threaded 500,000 Packets Monotonic Sequence and Data Integrity Stress Test
namespace {

struct Packet {
    uint64_t sequence{0};
    uint64_t payload{0};
    uint64_t checksum{0};

    static uint64_t computeChecksum(uint64_t seq, uint64_t pld) noexcept {
        // 64-bit MurmurHash3 finalizer style mixing function
        uint64_t h = (seq * 0x9E3779B97F4A7C15ULL) ^ (pld + 0x517CC1B727220A95ULL);
        h ^= (h >> 30);
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= (h >> 27);
        h *= 0x94D049BB133111EBULL;
        h ^= (h >> 31);
        return h;
    }

    static Packet create(uint64_t seq) noexcept {
        uint64_t pld = seq * 31ULL + 10007ULL;
        return Packet{seq, pld, computeChecksum(seq, pld)};
    }

    bool isValid() const noexcept {
        return checksum == computeChecksum(sequence, payload);
    }
};

} // anonymous namespace

TEST(LockFreeTripleBufferTest, MultiThreadStressTest500kPackets) {
    LockFreeTripleBuffer<Packet> buffer;

    constexpr uint64_t TOTAL_PACKETS = 500'000;
    constexpr uint64_t SENTINEL_SEQ = UINT64_MAX;

    std::atomic<bool> startSignal{false};
    uint64_t packetsConsumed = 0;
    uint64_t maxSequenceSeen = 0;
    bool integrityOk = true;
    bool monotonicityOk = true;

    // Consumer thread
    std::jthread consumerThread([&]() {
        while (!startSignal.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t lastSequence = 0;

        while (true) {
            Packet pkt{};
            bool received = buffer.consume<ConsumeMode::Wait>(pkt);
            if (!received) {
                continue;
            }

            if (!pkt.isValid()) {
                integrityOk = false;
                break;
            }

            if (pkt.sequence == SENTINEL_SEQ) {
                break;
            }

            if (pkt.sequence <= lastSequence) {
                monotonicityOk = false;
                break;
            }

            lastSequence = pkt.sequence;
            maxSequenceSeen = pkt.sequence;
            packetsConsumed++;
        }
    });

    // Producer thread
    std::jthread producerThread([&]() {
        while (!startSignal.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (uint64_t i = 1; i <= TOTAL_PACKETS; ++i) {
            buffer.produce(Packet::create(i));
        }

        // Produce termination sentinel packet
        buffer.produce(Packet{SENTINEL_SEQ, 0, Packet::computeChecksum(SENTINEL_SEQ, 0)});
    });

    // Fire start signal for both threads
    startSignal.store(true, std::memory_order_release);

    producerThread.join();
    consumerThread.join();

    EXPECT_TRUE(integrityOk) << "Data integrity violation / torn read detected!";
    EXPECT_TRUE(monotonicityOk) << "Monotonic ordering violation detected!";
    EXPECT_GT(packetsConsumed, 0u) << "Consumer did not receive any packets!";
    EXPECT_LE(maxSequenceSeen, TOTAL_PACKETS) << "Max sequence exceeded total packets!";
}

