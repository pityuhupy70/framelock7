// Portable, allocation-free clock arithmetic shared by FrameLock and its host-side tests.
#pragma once
#include <cstdint>
#include <limits>

namespace flclock {

// Computes floor(a * b / c) without forming the potentially overflowing a*b product.
// Inputs and result are non-negative; c must be positive. This form keeps the remainder
// below c and is safe for FrameLock's bounded FPS numerator.
inline bool MulDivFloor(int64_t a, int64_t b, int64_t c, int64_t& out) noexcept {
    if (a < 0 || b < 0 || c <= 0) return false;
    const uint64_t A = static_cast<uint64_t>(a);
    const uint64_t B = static_cast<uint64_t>(b);
    const uint64_t C = static_cast<uint64_t>(c);
    const uint64_t bWhole = B / C;
    const uint64_t bRem = B % C;
    const uint64_t limit = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());

    if (bWhole && A > limit / bWhole) return false;
    const uint64_t whole = A * bWhole;

    // Compute floor(A*bRem/C) with a bitwise long division. Since bRem < C and
    // the bitwise remainder remains below C, intermediate values stay bounded.
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        if (remainder > (limit - bRem) / 2) return false;
        const uint64_t v = remainder * 2 + ((A >> bit) & 1u ? bRem : 0u);
        const uint64_t digit = v / C;
        remainder = v % C;
        if (quotient > (limit - digit) / 2) return false;
        quotient = quotient * 2 + digit;
    }

    if (whole > limit - quotient) return false;
    out = static_cast<int64_t>(whole + quotient);
    return true;
}

inline bool FrameTicks(int64_t frame, int64_t qpcFrequency,
                       int64_t fpsNumerator, int64_t fpsDenominator,
                       int64_t& out) noexcept {
    if (frame < 0 || qpcFrequency <= 0 || fpsNumerator <= 0 ||
        fpsDenominator <= 0) return false;
    if (qpcFrequency > (std::numeric_limits<int64_t>::max)() / fpsDenominator)
        return false;
    const int64_t scale = qpcFrequency * fpsDenominator;
    return MulDivFloor(frame, scale, fpsNumerator, out);
}

// A locked step is a single immutable simulation timestamp. The only exception is an
// explicit user stop, where the unrecorded in-flight step is released so a blocked game
// can return to Present and unwind cleanly.
struct StepState {
    int64_t base = 0;
    int64_t step = 0;
    int64_t anchor = 0; // real QPC at step entry; diagnostic only
    bool released = false;
    int64_t releaseAt = 0;
};

inline int64_t Evaluate(const StepState& s, int64_t realQpc) noexcept {
    if (!s.released) return s.base;
    const int64_t elapsed = realQpc > s.releaseAt ? realQpc - s.releaseAt : 0;
    if (s.base > (std::numeric_limits<int64_t>::max)() - s.step) return (std::numeric_limits<int64_t>::max)();
    const int64_t next = s.base + s.step;
    if (elapsed > (std::numeric_limits<int64_t>::max)() - next) return (std::numeric_limits<int64_t>::max)();
    return next + elapsed;
}

} // namespace flclock
