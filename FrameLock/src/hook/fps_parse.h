// Exact, portable parsing of FrameLock's FPS text field.
// Decimal input is preserved as a base-10 rational; broadcast rationals must be explicit.
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <limits>

namespace flfps {

inline bool Parse(const wchar_t* text, int64_t& outNum, int64_t& outDen) noexcept {
    if (!text) return false;
    while (*text && std::iswspace(*text)) ++text;
    if (!*text) return false;

    auto reduce = [](long long n, long long d, int64_t& num, int64_t& den) noexcept -> bool {
        if (n <= 0 || d <= 0) return false;
        const double rate = static_cast<double>(n) / static_cast<double>(d);
        if (!std::isfinite(rate) || rate < 1.0 || rate > 1000.0) return false;
        long long a = n, b = d;
        while (b) { long long t = a % b; a = b; b = t; }
        if (a <= 0) return false;
        n /= a; d /= a;
        if (n > 1000000000LL || d > 1000000LL) return false;
        num = n; den = d;
        return true;
    };

    if (std::wcschr(text, L'/')) {
        wchar_t* endNum = nullptr;
        errno = 0;
        const long long n = std::wcstoll(text, &endNum, 10);
        if (errno || endNum == text) return false;
        while (*endNum && std::iswspace(*endNum)) ++endNum;
        if (*endNum != L'/') return false;
        const wchar_t* denStart = endNum + 1;
        while (*denStart && std::iswspace(*denStart)) ++denStart;
        wchar_t* endDen = nullptr;
        errno = 0;
        const long long d = std::wcstoll(denStart, &endDen, 10);
        if (errno || endDen == denStart) return false;
        while (*endDen && std::iswspace(*endDen)) ++endDen;
        if (*endDen) return false;
        return reduce(n, d, outNum, outDen);
    }

    const wchar_t* p = text;
    long long whole = 0, frac = 0, scale = 1;
    bool haveWhole = false, haveFrac = false;
    while (*p >= L'0' && *p <= L'9') {
        haveWhole = true;
        if (whole > 1000) return false;
        whole = whole * 10 + (*p++ - L'0');
    }
    if (*p == L'.') {
        ++p;
        while (*p >= L'0' && *p <= L'9') {
            if (scale >= 1000000) return false;
            haveFrac = true;
            frac = frac * 10 + (*p++ - L'0');
            scale *= 10;
        }
        if (!haveFrac) return false;
    }
    while (*p && std::iswspace(*p)) ++p;
    if (*p || (!haveWhole && !haveFrac)) return false;
    if (!haveWhole) whole = 0;
    if (whole > 1000 || whole * scale > ((std::numeric_limits<long long>::max)() - frac)) return false;
    return reduce(whole * scale + frac, scale, outNum, outDen);
}

} // namespace flfps
