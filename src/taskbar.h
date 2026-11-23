#pragma once
#include <windows.h>
#include <memory>
#include <optional>

namespace pulse {
struct TaskbarGap {
    RECT bar{};
    int left = 0, right = 0;
    UINT dpi = 96;
};
struct DisplayBounds { HMONITOR handle; RECT screen, work; };
std::optional<DisplayBounds> display_at(POINT point);
class TaskbarFinder {
public:
    TaskbarFinder();
    ~TaskbarFinder();
    std::optional<TaskbarGap> sample();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
unsigned foreground_process();
}
