#pragma once
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace pulse {
struct FpsResult { std::optional<double> value; std::wstring status; };
bool has_fps_permission();
std::optional<double> parse_presentmon_csv(std::string_view csv);
FpsResult sample_fps(const std::filesystem::path& executable, unsigned pid, std::stop_token stop);
}
