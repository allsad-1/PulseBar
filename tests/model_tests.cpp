#include "model.h"
#include "providers.h"
#include "fps.h"
#include <winrt/base.h>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool success, const char* name) {
    if (!success) { std::cerr << name << '\n'; ++failures; }
}
}

int main() {
    using namespace pulse;
    winrt::init_apartment();
    check(!cpu_percent({100, 150, 100}, {99, 160, 120}), "CPU rejects reversed counters");
    check(cpu_percent({100, 150, 100}, {140, 220, 130}) == 60, "CPU accounts for idle within kernel time");
    const auto net = network_speed({{1, {100, 200}}, {2, {500, 600}}}, {{1, {120, 240}}, {2, {4, 6}}, {3, {900, 1000}}}, 2);
    check(net && net->down == 10 && net->up == 20, "network ignores resets and interfaces without a baseline");
    check(!network_speed({{1, {10, 10}}}, {{2, {999, 999}}}, 1), "network waits for a comparable sample");
    check(anchor_x(100, 500, 320, 100) == 180 && anchor_x(100, 500, 320, -200) == 100, "offset is clamped within taskbar gap");
    check(!anchor_x(200, 500, 320, 0), "overlay cannot overlap app icons or the tray");
    check(clamp_screen_axis(-1230, -1920, 0, 360) == -1230, "pinned position supports negative monitor coordinates");
    check(clamp_screen_axis(1050, 0, 1080, 54) == 1026, "the whole strip fits before the bottom edge");
    check(clamp_screen_axis(1200, 0, 1080, 1200) == 0, "oversize surfaces start on the visible edge");
    check(reset_text(1000, 1001) == L"待同步" && reset_text(4601, 1000) == L"1h 01m", "reset countdown handles stale and fractional minutes");
    const auto compact = parse_balance(R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"48.50","granted_balance":"8.50","topped_up_balance":"40.00"}]})");
    check(compact && compact->entries.size() == 1 && compact->entries[0].total == 48.5, "DeepSeek accepts official numeric string schema");
    check(!parse_balance(R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"4junk","granted_balance":"0","topped_up_balance":"4"}]})"), "DeepSeek rejects malformed amount");
    const auto quota = parse_codex(R"({"timestamp":"2026-06-01T05:00:00.000Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"primary":{"used_percent":16,"window_minutes":10080,"resets_at":1790000000},"secondary":null}}})");
    check(quota && !quota->five_hour && quota->week && quota->week->remaining == 84 && quota->observed == 1780290000, "Codex recognizes weekly-only plans by window length");
    check(!parse_codex(R"({"type":"response_item","payload":{"type":"token_count","rate_limits":{}}})"), "Codex ignores conversation records");
    check(!parse_codex(R"({"timestamp":"2026-06-01T05:00:00Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"other","primary":{"used_percent":16,"window_minutes":300}}}})"), "Codex ignores model-specific buckets");
    const auto mixed = parse_codex(R"({"timestamp":"2026-06-01T05:00:00Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":null,"primary":{"used_percent":18,"window_minutes":300,"resets_at":null},"secondary":{"used_percent":30,"window_minutes":10080,"resets_at":1790000000}}}})");
    check(mixed && mixed->five_hour && mixed->five_hour->remaining == 82 && mixed->five_hour->resets == 0 && mixed->week && mixed->week->remaining == 70, "Codex keeps remaining quota when a reset date is absent");
    const auto fps = parse_presentmon_csv("Application,SwapChainAddress,MsBetweenDisplayChange\r\n\"Game, Inc.\",0xA,20\r\nGame,0xB,5\r\nGame,0xA,20\r\nGame,0xB,nan\r\n");
    check(fps && std::abs(*fps - 50.0) < 0.001, "FPS uses the main swap chain and ignores non-finite timings");
    check(!parse_presentmon_csv("App,ProcessID\nGame,123\n"), "FPS rejects CSV without frame timings");
    winrt::uninit_apartment();
    return failures == 0 ? 0 : 1;
}
