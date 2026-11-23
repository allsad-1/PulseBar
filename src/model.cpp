#include "model.h"
#include <algorithm>
#include <cwchar>

namespace pulse {
std::optional<double> cpu_percent(CpuTicks before, CpuTicks after) {
    if (after.idle < before.idle || after.kernel < before.kernel || after.user < before.user) return {};
    const double total = static_cast<double>(after.kernel - before.kernel) + static_cast<double>(after.user - before.user);
    const auto idle = static_cast<double>(after.idle - before.idle);
    if (total <= 0 || idle > total) return {};
    return 100 * (total - idle) / total;
}

std::optional<Speed> network_speed(const Interfaces& before, const Interfaces& after, double seconds) {
    if (seconds <= 0) return {};
    Speed speed{};
    bool comparable = false;
    for (const auto& [id, value] : after) {
        const auto old = before.find(id);
        if (old == before.end() || old->second.down > value.down || old->second.up > value.up) continue;
        comparable = true;
        speed.down += static_cast<double>(value.down - old->second.down) / seconds;
        speed.up += static_cast<double>(value.up - old->second.up) / seconds;
    }
    return comparable ? std::optional{speed} : std::nullopt;
}

std::optional<int> anchor_x(int left, int right, int width, int offset) {
    const long long last = static_cast<long long>(right) - width;
    if (width <= 0 || last < left) return {};
    return static_cast<int>(std::clamp(last + offset, static_cast<long long>(left), last));
}

int clamp_screen_axis(int requested, int lower, int upper, int size) {
    return std::clamp(requested, lower, std::max(lower, upper - size));
}

std::wstring reset_text(time_t reset, time_t now) {
    if (reset <= 0) return L"—";
    if (reset <= now) return L"待同步";
    const auto minutes = (reset - now + 59) / 60;
    wchar_t out[40];
    if (minutes >= 60) swprintf_s(out, L"%lldh %02lldm", static_cast<long long>(minutes / 60), static_cast<long long>(minutes % 60));
    else swprintf_s(out, L"%lldm", static_cast<long long>(minutes));
    return out;
}

std::wstring number(double value, int decimals) {
    wchar_t out[40];
    swprintf_s(out, L"%.*f", decimals, value);
    return out;
}

time_t unix_now() { return std::time(nullptr); }
}
