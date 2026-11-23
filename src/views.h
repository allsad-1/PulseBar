#pragma once
#include "model.h"
#include "render.h"
#include "settings.h"
#include <deque>

namespace pulse {
struct SamplePoint { time_t time; std::optional<double> cpu, gpu, ram; };
struct Data {
    Hardware hw;
    std::deque<SamplePoint> history;
    std::optional<CodexQuota> codex;
    std::optional<Balance> balance;
    time_t balance_time = 0;
    std::wstring balance_status, fps_status;
};
struct Cell {
    Metric metric;
    std::wstring primary, secondary;
    int width, tone = 0;
    bool dim = false;
    bool operator==(const Cell&) const = default;
};
struct BarPage {
    std::vector<Cell> cells;
    unsigned pages = 1;
    int width = 0;
    bool operator==(const BarPage&) const = default;
};
constexpr int bar_height = 34, details_width = 426, details_height = 477;
BarPage bar_page(const Settings& settings, const Data& data, int space, unsigned page, time_t now);
void paint_bar(Canvas& canvas, const BarPage& page, bool light, bool editing);
void paint_details(Canvas& canvas, const Settings& settings, const Data& data, time_t now);
}
