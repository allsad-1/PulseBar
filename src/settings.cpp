#include "settings.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

namespace pulse {
namespace {
constexpr wchar_t section[] = L"PulseBar";
constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t run_name[] = L"PulseBar";

std::wstring read_string(const std::filesystem::path& path, const wchar_t* key, const wchar_t* fallback) {
    std::wstring out(32768, L'\0');
    out.resize(GetPrivateProfileStringW(section, key, fallback, out.data(), static_cast<DWORD>(out.size()), path.c_str()));
    return out;
}

std::optional<POINT> read_position(const std::filesystem::path& path, const wchar_t* key) {
    POINT position{};
    wchar_t trailing = 0;
    if (swscanf_s(read_string(path, key, L"").c_str(), L"%ld,%ld %lc", &position.x, &position.y, &trailing, 1U) != 2 ||
        position.x < -50000 || position.x > 50000 || position.y < -50000 || position.y > 50000) return {};
    return position;
}

bool ensure_settings(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code error;
    if (std::filesystem::exists(path, error)) return !error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    constexpr wchar_t initial[]{L"\uFEFF[PulseBar]\r\norder=CPU,GPU,RAM,NET,FPS,CDX,DS\r\nrefresh_ms=1000\r\noffset=0\r\nopacity=100\r\ntheme=system\r\nnet_mbps=1\r\ntemp_warn=70\r\ntemp_critical=85\r\nbalance_warn=20\r\nbalance_critical=5\r\ncodex=0\r\ndeepseek=0\r\n"};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
    DWORD written = 0;
    constexpr DWORD size = sizeof(initial) - sizeof(wchar_t);
    const bool ok = WriteFile(file, initial, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

std::filesystem::path bundled_presentmon() {
    wchar_t executable[32768]{};
    const auto count = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!count || count >= std::size(executable)) return {};
    const auto path = std::filesystem::path(executable).parent_path() / L"vendor" / L"PresentMon.exe";
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path : std::filesystem::path{};
}

INT_PTR CALLBACK key_dialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        SendDlgItemMessageW(dialog, 100, EM_SETLIMITTEXT, 512, 0);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
        auto& key = *reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, DWLP_USER));
        wchar_t buffer[513]{};
        const int size = GetDlgItemTextW(dialog, 100, buffer, static_cast<int>(std::size(buffer)));
        if (!size) return TRUE;
        key.assign(buffer, size);
        SecureZeroMemory(buffer, sizeof(buffer));
        SetDlgItemTextW(dialog, 100, L"");
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(wparam) == IDCANCEL) || message == WM_CLOSE) {
        SetDlgItemTextW(dialog, 100, L"");
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
}

std::filesystem::path settings_path() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) return {};
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> local(raw, CoTaskMemFree);
    return std::filesystem::path(raw) / L"PulseBar" / L"settings.ini";
}

Settings load_settings() {
    Settings result;
    const auto path = settings_path();
    if (path.empty()) return result;
    const auto read = [&](const wchar_t* key, int fallback, int minimum, int maximum) {
        const auto value = static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
        return std::clamp(value, minimum, maximum);
    };
    for (size_t i = 0; i < metric_count; ++i) result.visible[i] = read((L"show_" + std::wstring(metric_names[i])).c_str(), 1, 0, 1) != 0;
    std::wstringstream order(read_string(path, L"order", L""));
    std::wstring name;
    std::array<bool, metric_count> seen{};
    size_t output = 0;
    while (std::getline(order, name, L',')) {
        const auto index = std::find_if(metric_names.begin(), metric_names.end(), [&](const auto* label) { return _wcsicmp(label, name.c_str()) == 0; });
        if (index == metric_names.end()) continue;
        const auto id = static_cast<size_t>(index - metric_names.begin());
        if (!seen[id]) { result.order[output++] = static_cast<Metric>(id); seen[id] = true; }
    }
    for (size_t i = 0; i < metric_count; ++i) if (!seen[i]) result.order[output++] = static_cast<Metric>(i);
    result.codex = read(L"codex", 0, 0, 1) != 0;
    result.deepseek = read(L"deepseek", 0, 0, 1) != 0;
    result.cpu_thermal = read(L"cpu_temperature", 1, 0, 1) != 0;
    result.nvidia_sensors = read(L"nvidia_sensors", 0, 0, 1) != 0;
    result.megabits = read(L"net_mbps", 1, 0, 1) != 0;
    result.refresh_ms = read(L"refresh_ms", 1000, 750, 5000);
    result.offset = read(L"offset", 0, -10000, 0);
    result.opacity = read(L"opacity", 100, 45, 100);
    result.temp_warn = read(L"temp_warn", 70, 0, 120);
    result.temp_critical = read(L"temp_critical", 85, result.temp_warn, 125);
    result.balance_warn = read(L"balance_warn", 20, 0, 100000);
    result.balance_critical = read(L"balance_critical", 5, 0, result.balance_warn);
    result.presentmon = read_string(path, L"presentmon", L"");
    if (result.presentmon.empty()) result.presentmon = bundled_presentmon();
    result.fps = read(L"fps_enabled", 1, 0, 1) != 0;
    result.panel_position = read_position(path, L"panel_position");
    result.status_position = read_position(path, L"status_position");
    const auto theme = read_string(path, L"theme", L"system");
    DWORD light = 0, bytes = sizeof(light);
    if (theme == L"system") RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    result.light = theme == L"light" || light != 0;
    return result;
}

bool write_setting(const wchar_t* key, const wchar_t* value) {
    const auto path = settings_path();
    return ensure_settings(path) && WritePrivateProfileStringW(section, key, value, path.c_str());
}

bool open_settings_file(HWND owner) {
    const auto path = settings_path();
    return ensure_settings(path) && reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", L"notepad.exe", (L"\"" + path.native() + L"\"").c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

bool runs_at_logon() {
    return RegGetValueW(HKEY_CURRENT_USER, run_key, run_name, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool set_runs_at_logon(bool enabled) {
    if (!enabled) {
        const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, run_name);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    wchar_t path[32768]{};
    const auto size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!size || size >= std::size(path)) return false;
    const auto command = L"\"" + std::wstring(path, size) + L"\"";
    return RegSetKeyValueW(HKEY_CURRENT_USER, run_key, run_name, REG_SZ, command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::optional<std::wstring> prompt_key(HWND owner, HINSTANCE instance) {
    std::wstring key;
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(101), owner, key_dialog, reinterpret_cast<LPARAM>(&key));
    if (result == IDOK) return key;
    return {};
}

std::optional<std::wstring> select_presentmon(HWND owner) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"PresentMon 控制台程序 (*.exe)\0*.exe\0";
    dialog.lpstrTitle = L"选择官方 PresentMon 控制台 EXE";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::optional{std::wstring(path)} : std::nullopt;
}
}
