#include "views.h"
#include <algorithm>

namespace pulse {
namespace {
struct Palette {
    D2D1_COLOR_F bg, tile, border, text, muted, track, accent, green, amber, red, violet;
};
Palette palette(bool light) {
    if (light) return {color(0xf9fcfb, .98f), color(0xecf4f0), color(0xc8d9d3), color(0x0c241e), color(0x456257), color(0xe0e9e5), color(0x006b56), color(0x087344), color(0x8d5300), color(0xb32135), color(0x573eb4)};
    return {color(0x111c21, .97f), color(0x203036), color(0x384d52), color(0xf1f9f6), color(0x9fb5b5), color(0x26363b), color(0x7ee9cd), color(0x69e59f), color(0xf9bd62), color(0xff8292), color(0xc6afff)};
}
std::wstring percent(std::optional<double> value) { return value ? number(*value) + L"%" : L"—"; }
std::wstring gib(uint64_t bytes) { return number(static_cast<double>(bytes) / 1073741824, 1); }
std::wstring memory(uint64_t used, uint64_t total) { return total ? gib(used) + L" / " + gib(total) + L" GiB" : L"—"; }

std::wstring speed(double bytes, bool megabits) {
    if (megabits) {
        const double value = bytes / 125000;
        return value >= 100 ? number(value) : number(value, 1);
    }
    const double value = bytes / 1000;
    return value >= 1000 ? number(value / 1000, value < 10000 ? 1 : 0) + L"M" : number(value) + L"K";
}
int temperature_tone(double temperature, const Settings& settings) {
    return temperature >= settings.temp_critical ? 3 : temperature >= settings.temp_warn ? 2 : 1;
}
int low_tone(double remaining, double warn, double critical) { return remaining < critical ? 3 : remaining < warn ? 2 : 1; }
D2D1_COLOR_F tone(const Palette& p, int value) {
    return value == 3 ? p.red : value == 2 ? p.amber : value == 1 ? p.green : p.muted;
}
bool expired(const QuotaWindow& quota, time_t now) { return quota.resets > 0 && quota.resets <= now; }
bool stale(time_t observed, time_t now) { return observed > 0 && now - observed >= 900; }

std::wstring local_time(time_t time) {
    tm date{};
    if (time <= 0 || localtime_s(&date, &time)) return L"—";
    wchar_t text[32]{};
    wcsftime(text, std::size(text), L"%m/%d %H:%M:%S", &date);
    return text;
}

const BalanceEntry& preferred_balance(const Balance& balance) {
    const auto cny = std::find_if(balance.entries.begin(), balance.entries.end(), [](const BalanceEntry& entry) { return entry.currency == L"CNY"; });
    return cny != balance.entries.end() ? *cny : balance.entries.front();
}
const wchar_t* currency_symbol(const BalanceEntry& entry) { return entry.currency == L"CNY" ? L"¥" : L"$"; }
std::wstring short_balance(const BalanceEntry& entry) {
    if (std::abs(entry.total) >= 1000000) return currency_symbol(entry) + number(entry.total / 1000000, 1) + L"M";
    if (std::abs(entry.total) >= 10000) return currency_symbol(entry) + number(entry.total / 1000, 1) + L"k";
    return currency_symbol(entry) + number(entry.total, 2);
}

std::optional<Cell> make_cell(Metric item, const Settings& s, const Data& d, time_t now, bool compact) {
    const auto& h = d.hw;
    Cell cell{item, {}, {}, 0};
    switch (item) {
    case Metric::cpu:
    case Metric::gpu: {
        const bool cpu = item == Metric::cpu;
        const auto temp = cpu ? h.cpu_temperature : h.gpu_temperature;
        cell.primary = percent(cpu ? h.cpu : h.gpu);
        cell.width = 72;
        if (temp) { cell.secondary = number(*temp) + L"°C"; cell.tone = temperature_tone(*temp, s); cell.width = 111; }
        break;
    }
    case Metric::ram:
        cell.primary = h.memory_total ? (compact ? gib(h.memory_used) + L" GiB" : gib(h.memory_used) + L"/" + gib(h.memory_total) + L" GiB") : L"—";
        cell.width = compact ? 107 : 150;
        break;
    case Metric::network:
        cell.primary = h.network ? L"↑" + speed(h.network->up, s.megabits) : L"↑—";
        cell.secondary = h.network ? L"↓" + speed(h.network->down, s.megabits) : L"↓—";
        cell.width = compact ? 75 : 125;
        break;
    case Metric::fps:
        if (!h.fps) return {};
        cell.primary = number(*h.fps); cell.width = 74;
        break;
    case Metric::codex: {
        if (!s.codex) return {};
        cell.primary = L"—"; cell.width = 77;
        if (!d.codex) break;
        const auto& limit = d.codex->five_hour ? d.codex->five_hour : d.codex->week;
        if (!limit || expired(*limit, now)) break;
        cell.primary = percent(limit->remaining);
        cell.secondary = limit->resets ? reset_text(limit->resets, now) : L"";
        cell.tone = low_tone(limit->remaining, 20, 5);
        cell.dim = stale(d.codex->observed, now);
        if (!compact && !cell.secondary.empty()) cell.width = 128;
        else cell.secondary.clear();
        break;
    }
    case Metric::deepseek: {
        if (!s.deepseek) return {};
        cell.primary = L"—"; cell.width = 94;
        if (!d.balance || d.balance->entries.empty()) break;
        const auto& balance = preferred_balance(*d.balance);
        cell.primary = short_balance(balance);
        cell.tone = d.balance->available ? low_tone(balance.total, s.balance_warn, s.balance_critical) : 3;
        cell.dim = stale(d.balance_time, now);
        break;
    }
    default: return {};
    }
    return cell;
}

void graph(Canvas& c, const Palette& p, const Data& d, time_t now) {
    c.rect(18, 137, 390, 80, 9, p.tile);
    c.text(L"过去 60 秒", Font::body, 29, 141, 125, 23, p.muted);
    c.text(L"CPU", Font::label, 277, 141, 28, 23, p.accent);
    c.text(L"GPU", Font::label, 319, 141, 28, 23, p.violet);
    c.text(L"RAM", Font::label, 360, 141, 32, 23, p.amber);
    for (float y : {171.0f, 188.0f, 205.0f}) c.line(29, y, 397, y, p.border);
    const auto draw = [&](auto accessor, D2D1_COLOR_F ink) {
        std::optional<float> px, py;
        for (const auto& point : d.history) {
            if (point.time < now - 60) continue;
            const auto value = accessor(point);
            if (!value) { px.reset(); py.reset(); continue; }
            const float x = 29 + static_cast<float>(std::clamp<time_t>(point.time - now + 60, 0, 60)) / 60 * 368;
            const float y = 205 - static_cast<float>(std::clamp(*value, 0.0, 100.0)) * .34f;
            if (px && py) c.line(*px, *py, x, y, ink, 1.6f);
            else c.rect(x - 1, y - 1, 2, 2, 1, ink);
            px = x; py = y;
        }
    };
    draw([](const SamplePoint& n) { return n.ram; }, p.amber);
    draw([](const SamplePoint& n) { return n.gpu; }, p.violet);
    draw([](const SamplePoint& n) { return n.cpu; }, p.accent);
}
}

BarPage bar_page(const Settings& s, const Data& d, int space, unsigned index, time_t now) {
    constexpr int frame = 10, gap = 2, tabs = 15;
    auto collect = [&](bool compact) {
        std::vector<Cell> cells;
        for (auto metric : s.order) if (s.visible[static_cast<size_t>(metric)]) {
            if (auto cell = make_cell(metric, s, d, now, compact)) cells.push_back(std::move(*cell));
        }
        return cells;
    };
    auto width = [&](const std::vector<Cell>& cells) {
        int pixels = cells.empty() ? 0 : frame - gap;
        for (const auto& cell : cells) pixels += cell.width + gap;
        return pixels;
    };
    auto cells = collect(false);
    if (const int w = width(cells); w <= space) return {std::move(cells), 1, w};
    cells = collect(true);
    if (const int w = width(cells); w <= space) return {std::move(cells), 1, w};
    std::vector<BarPage> pages(1);
    for (auto& cell : cells) {
        if (frame + tabs + cell.width > space) continue;
        auto* page = &pages.back();
        if (page->width && page->width + gap + cell.width > space) page = &pages.emplace_back();
        page->width += cell.width + (page->width ? gap : frame + tabs);
        page->cells.push_back(std::move(cell));
    }
    const auto count = static_cast<unsigned>(pages.size());
    auto page = std::move(pages[index % count]);
    page.pages = count;
    if (count == 1 && page.width) page.width -= tabs;
    return page;
}

void paint_bar(Canvas& c, const BarPage& page, bool light, bool editing) {
    const auto p = palette(light);
    const auto w = static_cast<float>(page.width);
    c.rect(.5f, 2.5f, w - 1, 30.5f, 14, color(0, .20f));
    c.rect(.5f, 1, w - 1, 30, 14, p.bg);
    c.outline(.5f, 1, w - 1, 30, 14, editing ? p.accent : p.border);
    float x = 5;
    for (const auto& item : page.cells) {
        const auto cw = static_cast<float>(item.width);
        c.rect(x, 4.5f, cw, 23.0f, 9, p.tile);
        const float label = item.metric == Metric::deepseek ? 17.0f : 25.0f;
        c.text(metric_names[static_cast<size_t>(item.metric)], Font::label, x + 8, 5, label, 22, p.muted);
        const float value_x = x + 8 + label;
        const float available = cw - 14 - label;
        if (item.metric == Metric::network) {
            if (item.width == 75) {
                c.text(item.primary, Font::compact, value_x, 5, available, 11, p.violet, Align::right);
                c.text(item.secondary, Font::compact, value_x, 15, available, 11, p.accent, Align::right);
            } else {
                c.text(item.primary, Font::value, value_x, 5, 44, 22, p.violet, Align::right);
                c.text(item.secondary, Font::value, value_x + 46, 5, available - 46, 22, p.accent, Align::right);
            }
        } else {
            const bool quota = item.metric == Metric::codex || item.metric == Metric::deepseek;
            const auto value_color = item.dim ? p.muted : quota && item.tone ? tone(p, item.tone) : p.text;
            if (item.secondary.empty()) c.text(item.primary, Font::value, value_x, 5, available, 22, value_color, Align::right);
            else {
                c.text(item.primary, Font::value, value_x, 5, 31, 22, value_color, Align::right);
                c.text(item.secondary, Font::compact, value_x + 32, 5, available - 32, 22, quota ? p.muted : tone(p, item.tone), Align::right);
            }
        }
        x += cw + 2;
    }
    if (page.pages > 1) c.text(L"·", Font::body, x, 5, 13, 22, p.accent, Align::middle);
}

void paint_details(Canvas& c, const Settings& s, const Data& d, time_t now) {
    const auto p = palette(s.light);
    c.rect(2, 4, 422, 471, 16, color(0, .26f));
    auto background = p.bg;
    background.a = 1.0f;
    c.rect(1, 1, 424, 470, 16, background);
    c.outline(1, 1, 424, 470, 16, p.border);
    c.rect(18, 23, 7, 7, 3, p.accent);
    c.text(L"PulseBar", Font::title, 34, 11, 160, 34, p.text);
    c.rect(282, 14, 99, 29, 7, p.tile);
    c.text(L"刷新额度", Font::body, 282, 14, 99, 29, p.accent, Align::middle);
    c.text(L"×", Font::title, 388, 13, 24, 30, p.muted, Align::middle);

    const auto& h = d.hw;
    const auto tile = [&](float x, const wchar_t* name, std::wstring value, const std::wstring& detail, D2D1_COLOR_F ink) {
        c.rect(x, 54, 126, 74, 9, p.tile);
        c.text(name, Font::label, x + 10, 60, 106, 17, p.muted);
        c.text(value, Font::title, x + 10, 76, 106, 28, ink);
        c.text(detail, Font::compact, x + 10, 103, 108, 18, p.muted);
    };
    const auto cpu_note = h.cpu_temperature ? L"温度 " + number(*h.cpu_temperature) + L" °C" : s.cpu_thermal ? L"CPU 温度暂无数据" : L"温度已关闭";
    const auto gpu_note = h.gpu_temperature ? L"NVIDIA " + number(*h.gpu_temperature) + L" °C" : L"GPU Engine";
    tile(18, L"CPU", percent(h.cpu), cpu_note, p.accent);
    tile(150, L"GPU", percent(h.gpu), gpu_note, p.violet);
    const auto ram_now = h.memory_total ? gib(h.memory_used) + L" GiB" : L"—";
    const auto ram_total = h.memory_total ? L"/ " + gib(h.memory_total) + L" GiB · " + percent(h.ram) : L"";
    tile(282, L"RAM", ram_now, ram_total, p.amber);
    graph(c, p, d, now);

    c.text(L"显存  " + memory(h.vram_used, h.vram_total), Font::body, 19, 218, 215, 23, p.text);
    c.text(L"FPS  " + (h.fps ? number(*h.fps) : L"—") + L"  ·  " + (d.fps_status.empty() ? L"未启用" : d.fps_status), Font::body, 19, 240, 388, 22, p.muted);
    if (h.network) {
        const std::wstring unit = s.megabits ? L" Mbps" : L"B/s";
        c.text(L"↑ " + speed(h.network->up, s.megabits) + unit + L"    ↓ " + speed(h.network->down, s.megabits) + unit, Font::body, 185, 218, 222, 23, p.muted, Align::right);
    }
    const size_t core_count = std::min<size_t>(h.cores.size(), 48);
    for (size_t i = 0; i < core_count; ++i) {
        const float x = 19 + static_cast<float>(i) * 7;
        c.rect(x, 269, 4, 16, 2, p.track);
        const float active = static_cast<float>(h.cores[i] / 100.0) * 16;
        if (active >= .75f) c.rect(x, 285 - active, 4, active, 1.5f, p.accent);
    }
    if (core_count) c.text(L"逻辑核心", Font::body, 357, 263, 51, 27, p.muted, Align::right);

    c.line(18, 295, 408, 295, p.border);
    c.text(L"Codex", Font::body, 19, 300, 130, 23, p.text);
    const auto codex_state = !s.codex ? L"托盘菜单中开启" : !d.codex ? L"等待本机限额记录" : (stale(d.codex->observed, now) ? L"上次同步  " : L"已同步  ") + local_time(d.codex->observed);
    c.text(codex_state, Font::body, 141, 300, 266, 23, p.muted, Align::right);
    const auto quota_row = [&](const wchar_t* label, const std::optional<QuotaWindow>& quota, float y) {
        c.text(label, Font::body, 19, y, 68, 20, p.muted);
        c.rect(87, y + 7, 166, 6, 3, p.track);
        if (!quota || expired(*quota, now)) {
            c.text(quota ? L"待同步" : L"—", Font::body, 267, y, 140, 20, p.muted, Align::right);
            return;
        }
        const auto ink = d.codex && stale(d.codex->observed, now) ? p.muted : tone(p, low_tone(quota->remaining, 20, 5));
        const auto amount = static_cast<float>(quota->remaining) * 1.66f;
        if (amount >= 2) c.rect(87, y + 7, amount, 6, 3, ink);
        c.text(percent(quota->remaining) + L"  ·  " + reset_text(quota->resets, now), Font::body, 257, y, 150, 20, ink, Align::right);
    };
    quota_row(L"5 小时", d.codex ? d.codex->five_hour : std::nullopt, 325);
    quota_row(L"7 天", d.codex ? d.codex->week : std::nullopt, 349);

    c.line(18, 379, 408, 379, p.border);
    c.text(L"DeepSeek", Font::body, 19, 384, 145, 22, p.text);
    auto state = s.deepseek ? d.balance_status : L"托盘菜单中配置";
    if (state.empty()) state = L"等待同步";
    if (d.balance_time) state += L"  " + local_time(d.balance_time);
    c.text(state, Font::body, 120, 384, 287, 22, p.muted, Align::right);
    if (d.balance) {
        float y = 408;
        for (const auto& entry : d.balance->entries) {
            if (y >= 452) break;
            const auto ink = stale(d.balance_time, now) ? p.muted : tone(p, d.balance->available ? low_tone(entry.total, s.balance_warn, s.balance_critical) : 3);
            c.text(entry.currency + L"  " + currency_symbol(entry) + number(entry.total, 2), Font::body, 19, y, 157, 23, ink);
            c.text(L"赠金 " + number(entry.granted, 2) + L"  ·  充值 " + number(entry.topped_up, 2), Font::body, 176, y, 231, 23, p.muted, Align::right);
            y += 23;
        }
    }
}
}
