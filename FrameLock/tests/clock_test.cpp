// Host-side deterministic tests for FrameLock's exact integer clock contract.
// These tests do NOT emulate a Windows game, GPU, driver, or file system. They test
// the portable timing and frame-commit state machine used by the Windows DLL.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include "../src/hook/clock_math.h"
#include "../src/hook/fps_parse.h"
#include "../src/hook/present_gate.h"

namespace {
int failures = 0;

void Check(bool condition, const char* label, int64_t a = 0, int64_t b = 0) {
    if (!condition) {
        ++failures;
        if (failures <= 30) std::printf("FAIL: %s (actual=%lld expected=%lld)\n", label,
            static_cast<long long>(a), static_cast<long long>(b));
    }
}

void TestMulDivSmallExhaustive() {
    for (int64_t a = 0; a <= 250; ++a) {
        for (int64_t b = 0; b <= 250; ++b) {
            for (int64_t c = 1; c <= 50; ++c) {
                int64_t got = -1;
                const bool ok = flclock::MulDivFloor(a, b, c, got);
                const int64_t expected = (a * b) / c; // product is safely bounded here
                Check(ok && got == expected, "MulDivFloor exhaustive small-domain", got, expected);
            }
        }
    }
    int64_t out = 0;
    Check(!flclock::MulDivFloor(-1, 1, 1, out), "MulDivFloor rejects negative input");
    Check(!flclock::MulDivFloor(1, 1, 0, out), "MulDivFloor rejects zero divisor");
    Check(!flclock::MulDivFloor((std::numeric_limits<int64_t>::max)(), 2, 1, out),
          "MulDivFloor rejects overflow");
}

// Independent quotient/remainder recurrence: no call to FrameTicks for the expected values.
void TestFrameSchedule(int64_t num, int64_t den, int64_t freq, int64_t hours) {
    const int64_t scale = freq * den; // inputs in this test are intentionally within int64 bounds
    const int64_t q = scale / num;
    const int64_t rem = scale % num;
    const int64_t frames = (hours * 3600 * num) / den;
    int64_t expected = 0;
    int64_t carry = 0;
    int64_t minStep = (std::numeric_limits<int64_t>::max)();
    int64_t maxStep = 0;
    int64_t prev = -1;
    for (int64_t n = 0; n <= frames; ++n) {
        int64_t got = -1;
        const bool ok = flclock::FrameTicks(n, freq, num, den, got);
        Check(ok, "FrameTicks must succeed in the tested range", n, frames);
        Check(got == expected, "FrameTicks equals independent rational recurrence", got, expected);
        if (prev >= 0) {
            const int64_t step = got - prev;
            minStep = std::min(minStep, step);
            maxStep = std::max(maxStep, step);
            Check(step > 0, "frame boundaries strictly increase", step, 1);
        }
        prev = got;
        if (n != frames) {
            expected += q;
            carry += rem;
            if (carry >= num) { ++expected; carry -= num; }
        }
    }
    Check(maxStep - minStep <= 1, "integer QPC frame interval jitter is at most one tick", maxStep - minStep, 1);
    std::printf("schedule %6lld/%-5lld qpc=%-10lld %2lld h: %lld boundaries, interval %lld..%lld ticks\n",
        static_cast<long long>(num), static_cast<long long>(den), static_cast<long long>(freq),
        static_cast<long long>(hours), static_cast<long long>(frames + 1),
        static_cast<long long>(minStep), static_cast<long long>(maxStep));
}


void TestWholePresentTransactionIsSerialized() {
    // Exercise the exact gate class used by dllmain.cpp. The critical section represents
    // StepPre/capture+save -> original Present -> StepPost/clock advance as one transaction.
    flpresent::SequenceGate gate;
    std::atomic<int> active{ 0 };
    std::atomic<int> overlaps{ 0 };
    std::vector<std::thread> presenters;
    presenters.reserve(16);
    for (int t = 0; t < 16; ++t) {
        presenters.emplace_back([&] {
            for (int i = 0; i < 500; ++i) {
                auto transaction = gate.Enter();
                if (active.fetch_add(1, std::memory_order_acq_rel) != 0)
                    overlaps.fetch_add(1, std::memory_order_relaxed);
                // Yield at each boundary to force competing threads to contend while the
                // gate is held, just as other swapchains/render threads can do in a game.
                std::this_thread::yield(); // StepPre / capture+commit
                std::this_thread::yield(); // original Present
                std::this_thread::yield(); // StepPost / clock advance
                active.fetch_sub(1, std::memory_order_acq_rel);
            }
        });
    }
    for (auto& t : presenters) t.join();
    Check(overlaps.load() == 0, "whole Present transaction never overlaps across threads", overlaps.load(), 0);
    Check(active.load() == 0, "Present transaction gate is fully released", active.load(), 0);
}

void TestControlThreadTryGate() {
    flpresent::SequenceGate gate;
    auto present = gate.Enter();
    auto stopAttempt = gate.TryEnter();
    Check(!stopAttempt.owns_lock(), "stop monitor never blocks behind an active Present transaction", stopAttempt.owns_lock(), 0);
    present.unlock();
    auto afterPresent = gate.TryEnter();
    Check(afterPresent.owns_lock(), "stop monitor can enter after Present transaction completes", afterPresent.owns_lock(), 1);
}

void TestImmutableStepReads() {
    constexpr int64_t base = 918273645;
    constexpr int64_t step = 166666;
    const flclock::StepState state{ base, step, 100, false, 0 };
    std::atomic<int> mismatches{ 0 };
    std::vector<std::thread> readers;
    readers.reserve(48);
    for (int t = 0; t < 48; ++t) {
        readers.emplace_back([&, t] {
            std::mt19937_64 rng(0xF10C10CULL + static_cast<uint64_t>(t));
            std::uniform_int_distribution<int64_t> delay(0, 60LL * 60LL * 10000000LL);
            for (int i = 0; i < 10000; ++i) {
                const int64_t real = 100 + delay(rng);
                if (flclock::Evaluate(state, real) != base) mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& reader : readers) reader.join();
    Check(mismatches.load() == 0, "48 concurrent readers see one immutable timestamp", mismatches.load(), 0);

    const flclock::StepState released{ base, step, 100, true, 500 };
    Check(flclock::Evaluate(released, 500) == base + step, "explicit stop releases next boundary", flclock::Evaluate(released, 500), base + step);
    Check(flclock::Evaluate(released, 1000) == base + step + 500, "released clock follows real elapsed time", flclock::Evaluate(released, 1000), base + step + 500);
}

void TestFpsParsing() {
    auto expect = [](const wchar_t* text, int64_t n, int64_t d, const char* label) {
        int64_t gotN = 0, gotD = 0;
        const bool ok = flfps::Parse(text, gotN, gotD);
        Check(ok && gotN == n && gotD == d, label, gotN, n);
    };
    auto reject = [](const wchar_t* text, const char* label) {
        int64_t n = 0, d = 0;
        Check(!flfps::Parse(text, n, d), label, n, 0);
    };
    expect(L"59.94", 2997, 50, "decimal FPS preserves base-10 value 59.94");
    expect(L"60000/1001", 60000, 1001, "explicit broadcast rational preserved");
    expect(L"23.976", 2997, 125, "decimal 23.976 is not silently rewritten to 24000/1001");
    expect(L"  60  ", 60, 1, "integer FPS permits surrounding whitespace");
    expect(L" 30000 / 1001 ", 30000, 1001, "rational FPS permits surrounding whitespace");
    reject(nullptr, "null FPS rejected");
    reject(L"", "empty FPS rejected");
    reject(L"0", "zero FPS rejected");
    reject(L"0.5", "sub-one FPS rejected");
    reject(L"1001", "FPS above 1000 rejected");
    reject(L"59.9400001", "more than six decimal places rejected");
    reject(L"59.94junk", "trailing non-whitespace rejected");
    reject(L"-60", "negative FPS rejected");
    reject(L"60/0", "zero rational denominator rejected");
    reject(L"1/1001", "rational FPS below one rejected");
}

struct SimulatedRecorder {
    int64_t nextFrame = 0;
    int64_t virtualStep = 0;
    bool stopped = false;
    bool CommitFrame(bool captureOk, bool encodeOk, bool flushAndRenameOk, bool anotherFrameExpected = true) {
        if (stopped || !captureOk || !encodeOk || !flushAndRenameOk) {
            stopped = true;
            return false;
        }
        ++nextFrame;
        // The clock advances only to prepare a subsequent frame. The final committed
        // image keeps its own boundary; no extra unrecorded simulation interval is added.
        if (anotherFrameExpected) ++virtualStep;
        return true;
    }
};

void TestCommitGateAndNoGap() {
    for (int failPoint = 0; failPoint < 3; ++failPoint) {
        SimulatedRecorder r;
        Check(r.CommitFrame(true, true, true), "first complete frame commits", r.nextFrame, 1);
        const int64_t saved = r.nextFrame;
        const int64_t step = r.virtualStep;
        const bool captured = failPoint != 0;
        const bool encoded = failPoint != 1;
        const bool flushed = failPoint != 2;
        Check(!r.CommitFrame(captured, encoded, flushed), "each injected failure rejects frame", 0, 1);
        Check(r.stopped, "failure permanently stops the take", r.stopped, 1);
        Check(r.nextFrame == saved, "failure does not consume a frame number", r.nextFrame, saved);
        Check(r.virtualStep == step, "failure does not advance the simulation clock", r.virtualStep, step);
        Check(!r.CommitFrame(true, true, true), "stopped take cannot silently resume");
    }

    SimulatedRecorder r;
    for (int64_t i = 0; i < 100000; ++i)
        Check(r.CommitFrame(true, true, true), "successful frames commit sequentially");
    Check(r.nextFrame == 100000, "long commit sequence has exact frame count", r.nextFrame, 100000);
    Check(r.virtualStep == r.nextFrame, "one saved image corresponds to one simulation step", r.virtualStep, r.nextFrame);

    SimulatedRecorder finalFrame;
    finalFrame.virtualStep = 1; // the one startup warm-up boundary before frame_000000
    for (int64_t i = 0; i < 123; ++i) {
        const bool anotherFrameExpected = i != 122;
        Check(finalFrame.CommitFrame(true, true, true, anotherFrameExpected), "123-frame take commits in order");
    }
    Check(finalFrame.nextFrame == 123, "final-frame test committed exactly 123 images", finalFrame.nextFrame, 123);
    Check(finalFrame.virtualStep == 123, "final frame does not advance to an unrecorded 124th step", finalFrame.virtualStep, 123);

    // If F9 is pressed just after StepPost already advanced for the next frame, one
    // pending boundary may exist at stop; it is discarded and never becomes an image.
    SimulatedRecorder lateStop;
    lateStop.virtualStep = 1; // startup warm-up
    for (int64_t i = 0; i < 123; ++i)
        Check(lateStop.CommitFrame(true, true, true), "late-stop take commits in order");
    Check(lateStop.nextFrame == 123 && lateStop.virtualStep == 124,
          "late user stop permits only one pending uncommitted boundary", lateStop.virtualStep, 124);
    Check(lateStop.virtualStep - lateStop.nextFrame == 1,
          "pending boundary is not counted as an exported frame", lateStop.virtualStep - lateStop.nextFrame, 1);
}

void TestSlowCommitDoesNotAdvanceClock() {
    SimulatedRecorder r;
    std::mutex mx;
    std::condition_variable cv;
    bool writerEntered = false;
    bool allowCommit = false;
    bool committed = false;
    std::thread writer([&] {
        std::unique_lock<std::mutex> lk(mx);
        writerEntered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return allowCommit; }); // models a slow disk/encoder commit barrier
        lk.unlock();
        committed = r.CommitFrame(true, true, true);
    });
    {
        std::unique_lock<std::mutex> lk(mx);
        cv.wait(lk, [&] { return writerEntered; });
    }
    Check(r.nextFrame == 0 && r.virtualStep == 0, "slow storage leaves frame index and virtual clock unchanged", r.virtualStep, 0);
    {
        std::lock_guard<std::mutex> lk(mx);
        allowCommit = true;
    }
    cv.notify_all();
    writer.join();
    Check(committed && r.nextFrame == 1 && r.virtualStep == 1,
          "clock advances exactly once after slow commit completes", r.virtualStep, 1);
}

void TestOverflowAndInvalidRates() {
    int64_t out = 0;
    Check(!flclock::FrameTicks(-1, 10000000, 60, 1, out), "negative frame index rejected");
    Check(!flclock::FrameTicks(1, 0, 60, 1, out), "zero QPC frequency rejected");
    Check(!flclock::FrameTicks(1, 10000000, 0, 1, out), "zero FPS numerator rejected");
    Check(!flclock::FrameTicks(1, (std::numeric_limits<int64_t>::max)(), 60, 2, out),
          "frequency denominator multiplication overflow rejected");
    Check(!flclock::FrameTicks((std::numeric_limits<int64_t>::max)(), 10000000, 1, 1, out),
          "frame boundary overflow rejected");
}
}

int main() {
    TestWholePresentTransactionIsSerialized();
    TestControlThreadTryGate();
    TestMulDivSmallExhaustive();
    const int64_t rates[][2] = {
        {24,1},{25,1},{30,1},{50,1},{60,1},{120,1},{144,1},{240,1},
        {24000,1001},{30000,1001},{60000,1001}
    };
    for (const auto& rate : rates) TestFrameSchedule(rate[0], rate[1], 10000000, 10);
    TestFrameSchedule(60000, 1001, 3579545, 10);
    TestFrameSchedule(144, 1, 2531835, 10);
    TestFrameSchedule(24000, 1001, 1000000000, 10);
    TestImmutableStepReads();
    TestFpsParsing();
    TestCommitGateAndNoGap();
    TestSlowCommitDoesNotAdvanceClock();
    TestOverflowAndInvalidRates();
    if (failures) {
        std::printf("\nRESULT: %d FAILURES\n", failures);
        return 1;
    }
    std::printf("\nRESULT: ALL CHECKS PASSED (portable clock math + simulated commit-gate tests only)\n");
    return 0;
}
