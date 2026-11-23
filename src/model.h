#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulse {
struct CpuTicks { uint64_t idle, kernel, user; };
struct Traffic { uint64_t down, up; };
using Interfaces = std::map<uint64_t, Traffic>;
struct Speed { double down, up; };

struct Hardware {
    std::optional<double> cpu, ram, gpu, gpu_temperature, cpu_temperature, fps;
    std::optional<Speed> network;
    uint64_t memory_total = 0, memory_used = 0, vram_total = 0, vram_used = 0;
    std::vector<double> cores;
    unsigned active_3d_pid = 0;
};

struct QuotaWindow { double remaining; int minutes; time_t resets; };
struct CodexQuota {
    std::optional<QuotaWindow> five_hour, week;
    time_t observed = 0;
};
struct BalanceEntry { std::wstring currency; double total, granted, topped_up; };
struct Balance { bool available; std::vector<BalanceEntry> entries; };

enum class Metric { cpu, gpu, ram, network, fps, codex, deepseek, count };
constexpr auto metric_count = static_cast<size_t>(Metric::count);
inline constexpr std::array metric_names{L"CPU", L"GPU", L"RAM", L"NET", L"FPS", L"CDX", L"DS"};

std::optional<double> cpu_percent(CpuTicks before, CpuTicks after);
std::optional<Speed> network_speed(const Interfaces& before, const Interfaces& after, double seconds);
std::optional<int> anchor_x(int left, int right, int width, int offset);
int clamp_screen_axis(int requested, int lower, int upper, int size);
std::wstring reset_text(time_t reset, time_t now);
std::wstring number(double value, int decimals = 0);
time_t unix_now();
}
