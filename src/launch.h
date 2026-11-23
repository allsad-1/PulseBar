#pragma once
#include <windows.h>

namespace pulse {
enum class Launch : WPARAM { status = 1, panel, hide_status, edit, both };
inline constexpr UINT launch_message = WM_APP + 4;
inline constexpr wchar_t bar_class[] = L"PulseBar.Taskbar";
inline constexpr wchar_t mutex_name[] = L"Local\\PulseBar.43D319F1";
}
