// The ring is the only lock-free thing in this repository, so it is tested against a real second
// thread rather than against a sequence of calls on one. A single-threaded test would pass on an
// implementation with no memory ordering at all.

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "hotpath/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("capacity rounds up to a power of two", "[ring]") {
    CHECK(hotpath::SpscRing<int>(1).capacity() == 2);
    CHECK(hotpath::SpscRing<int>(2).capacity() == 2);
    CHECK(hotpath::SpscRing<int>(3).capacity() == 4);
    CHECK(hotpath::SpscRing<int>(1000).capacity() == 1024);
    CHECK(hotpath::SpscRing<int>(1024).capacity() == 1024);
}

TEST_CASE("a full ring refuses the push instead of overwriting", "[ring]") {
    hotpath::SpscRing<int> ring(4);

    for (int i = 0; i < 4; ++i) {
        CHECK(ring.try_push(i));
    }
    CHECK(ring.size() == 4);
    CHECK_FALSE(ring.try_push(99));

    int value = -1;
    REQUIRE(ring.try_pop(value));
    CHECK(value == 0);
    CHECK(ring.try_push(99));
    CHECK_FALSE(ring.try_push(100));
}

TEST_CASE("an empty ring reports the pop as a failure", "[ring]") {
    hotpath::SpscRing<int> ring(4);

    int value = -1;
    CHECK_FALSE(ring.try_pop(value));
    CHECK(value == -1);
    CHECK(ring.size() == 0);
}

TEST_CASE("values come back in the order they went in", "[ring]") {
    hotpath::SpscRing<int> ring(8);

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 6; ++i) {
            REQUIRE(ring.try_push((round * 100) + i));
        }
        for (int i = 0; i < 6; ++i) {
            int value = -1;
            REQUIRE(ring.try_pop(value));
            CHECK(value == (round * 100) + i);
        }
    }
}

// The index arithmetic is `head - tail` on unsigned counters that are never reduced modulo the
// capacity, so the wrap it has to survive is the counters' own overflow rather than the ring's.
// This is the cheap half of that: enough pushes to wrap the slots many times over.
TEST_CASE("the ring wraps its slots without losing an element", "[ring]") {
    hotpath::SpscRing<std::uint32_t> ring(4);

    for (std::uint32_t i = 0; i < 10'000; ++i) {
        REQUIRE(ring.try_push(i));
        std::uint32_t value = 0;
        REQUIRE(ring.try_pop(value));
        REQUIRE(value == i);
    }
}

TEST_CASE("one producer and one consumer on two threads lose nothing", "[ring]") {
    constexpr std::uint64_t kItems = 200'000;
    hotpath::SpscRing<std::uint64_t> ring(64);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            while (!ring.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t received = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t value = 0;
    while (received < kItems) {
        if (!ring.try_pop(value)) {
            std::this_thread::yield();
            continue;
        }
        if (value != received) {
            ++out_of_order;
        }
        ++received;
    }
    producer.join();

    CHECK(received == kItems);
    CHECK(out_of_order == 0);
    CHECK(ring.size() == 0);
}

TEST_CASE("a ring holding a struct copies every field", "[ring]") {
    struct Slot {
        std::int64_t stamp;
        double duration;
        std::array<char, 8> tag;
    };

    hotpath::SpscRing<Slot> ring(2);
    Slot pushed{};
    pushed.stamp = 1'787'290'000'123;
    pushed.duration = 0.00425;
    pushed.tag[0] = 'w';
    pushed.tag[1] = 'r';
    REQUIRE(ring.try_push(pushed));

    Slot popped{};
    REQUIRE(ring.try_pop(popped));
    CHECK(popped.stamp == pushed.stamp);
    CHECK(popped.duration == pushed.duration);
    CHECK(popped.tag[0] == 'w');
    CHECK(popped.tag[1] == 'r');
}
