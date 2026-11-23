#pragma once
#include "model.h"
#include <windows.h>
#include <filesystem>

namespace pulse {
struct Settings {
    std::array<bool, metric_count> visible{true, true, true, true, true, true, true};
    std::array<Metric, metric_count> order{Metric::cpu, Metric::gpu, Metric::ram, Metric::network, Metric::fps, Metric::codex, Metric::deepseek};
    bool codex = false, deepseek = false, cpu_thermal = true, nvidia_sensors = false, megabits = true, light = false, fps = true;
    int refresh_ms = 1000, offset = 0, opacity = 100, temp_warn = 70, temp_critical = 85;
    int balance_warn = 20, balance_critical = 5;
    std::filesystem::path presentmon;
    std::optional<POINT> panel_position, status_position;
};
std::filesystem::path settings_path();
Settings load_settings();
bool write_setting(const wchar_t* key, const wchar_t* value);
bool open_settings_file(HWND owner);
bool runs_at_logon();
bool set_runs_at_logon(bool enabled);
std::optional<std::wstring> prompt_key(HWND owner, HINSTANCE instance);
std::optional<std::wstring> select_presentmon(HWND owner);
}
