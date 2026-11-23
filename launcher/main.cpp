#include <windows.h>
#include <shellapi.h>
#include <filesystem>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    constexpr const wchar_t* actions[] = {L"--panel", L"--show-status", L"--hide-status", L"--edit"};
    static_assert(PULSE_LAUNCHER >= 0 && PULSE_LAUNCHER < 4);
    wchar_t self[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (!length || length >= std::size(self)) return 1;
    const auto directory = std::filesystem::path(self).parent_path();
    const auto program = directory / L"PulseBar.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", program.c_str(), actions[PULSE_LAUNCHER], directory.c_str(), SW_SHOWNORMAL)) > 32) return 0;
    MessageBoxW(nullptr, L"请将这个启动器与 PulseBar.exe 放在同一目录。", L"PulseBar", MB_OK | MB_ICONWARNING);
    return 1;
}
