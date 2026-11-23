#include "fps.h"
#include "launch.h"
#include "providers.h"
#include "taskbar.h"
#include "telemetry.h"
#include "views.h"
#include <shellapi.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <windowsx.h>

namespace pulse {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr wchar_t details_class[] = L"PulseBar.Details";
constexpr UINT tray_message = WM_APP + 1, show_details_message = WM_APP + 2;
constexpr UINT menu_details = 1, menu_refresh = 2, menu_hidden = 3, menu_codex = 4, menu_key = 5, menu_forget = 6,
               menu_fps = 7, menu_stop_fps = 8, menu_thermal = 9, menu_net = 10, menu_startup = 11, menu_edit = 12,
               menu_reload = 13, menu_exit = 14, menu_deepseek = 16, menu_nvidia = 17, menu_config_file = 18, menu_metrics = 30;

struct Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    Settings settings;
    Data data;
    std::optional<TaskbarGap> gap;
    unsigned settings_version = 0, refresh_version = 0, foreground = 0, fps_pid = 0;
    std::optional<double> fps;
};

void sample_desktop(std::stop_token stop, Shared& state) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        TaskbarFinder finder;
        auto last_valid = Clock::time_point{};
        while (!stop.stop_requested()) {
            const auto gap = finder.sample();
            const auto now = Clock::now();
            std::unique_lock lock(state.mutex);
            if (gap && gap->right > gap->left) {
                state.gap = gap;
                last_valid = now;
            } else if (now - last_valid >= 2s) {
                state.gap.reset();
            }
            state.changed.wait_for(lock, stop, 650ms, [] { return false; });
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void sample_hardware(std::stop_token stop, Shared& state) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Sampler sampler;
        while (!stop.stop_requested()) {
            const auto begin = Clock::now();
            const auto foreground = foreground_process();
            std::unique_lock lock(state.mutex);
            const bool nvidia = state.settings.nvidia_sensors;
            const auto interval = std::chrono::milliseconds(state.settings.refresh_ms);
            lock.unlock();
            auto sample = sampler.sample(foreground, nvidia);
            lock.lock();
            const auto now = unix_now();
            state.foreground = foreground;
            auto& data = state.data;
            data.history.push_back({now, sample.cpu, sample.gpu, sample.ram});
            while (!data.history.empty() && (data.history.front().time < now - 60 || data.history.size() > 100)) data.history.pop_front();
            const auto current_3d = data.hw.active_3d_pid;
            sample.cpu_temperature = data.hw.cpu_temperature;
            data.hw = std::move(sample);
            if (data.hw.active_3d_pid != current_3d) state.changed.notify_all();
            state.changed.wait_until(lock, stop, begin + interval, [] { return false; });
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void sample_temperature(std::stop_token stop, Shared& state) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return;
    std::unique_lock lock(state.mutex);
    while (!stop.stop_requested()) {
        if (!state.settings.cpu_thermal) {
            state.data.hw.cpu_temperature.reset();
            state.changed.wait(lock, stop, [&] { return state.settings.cpu_thermal; });
            continue;
        }
        lock.unlock();
        const auto temperature = read_cpu_temperature();
        lock.lock();
        if (state.settings.cpu_thermal) state.data.hw.cpu_temperature = temperature;
        state.changed.wait_for(lock, stop, temperature ? 5s : 20s, [&] { return !state.settings.cpu_thermal; });
    }
    lock.unlock();
    CoUninitialize();
}

void sample_ai(std::stop_token stop, Shared& state) {
    bool initialized = false;
    unsigned settings_version = 0, refresh_version = 0;
    auto codex_at = Clock::time_point::min(), deepseek_at = codex_at, eligible = codex_at;
    unsigned backoff = 0;
    std::unique_lock lock(state.mutex);
    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        if (settings_version != state.settings_version || refresh_version != state.refresh_version) {
            settings_version = state.settings_version;
            refresh_version = state.refresh_version;
            codex_at = now;
            deepseek_at = std::max(now, eligible);
        }
        const bool codex = state.settings.codex && now >= codex_at;
        const bool deepseek = state.settings.deepseek && now >= deepseek_at;
        if (codex || deepseek) {
            lock.unlock();
            if (!initialized) { winrt::init_apartment(); initialized = true; }
            if (codex) {
                const auto result = read_codex();
                lock.lock();
                if (settings_version == state.settings_version && result && (!state.data.codex || result->observed >= state.data.codex->observed)) state.data.codex = result;
                lock.unlock();
                codex_at = Clock::now() + 50s;
            }
            std::optional<BalanceResult> result;
            if (deepseek && !stop.stop_requested()) result = read_deepseek();
            lock.lock();
            if (result) {
                eligible = Clock::now() + 8s;
                backoff = result->retry ? std::min(3U, backoff + 1) : 0;
                deepseek_at = Clock::now() + std::min(1800s, std::chrono::seconds(300U << backoff));
                if (settings_version == state.settings_version) {
                    state.data.balance_status = std::move(result->status);
                    if (result->http_status == 401 || result->http_status == 403) {
                        deepseek_at = Clock::time_point::max();
                        state.data.balance.reset();
                        state.data.balance_time = 0;
                    }
                    if (result->balance) {
                        state.data.balance = std::move(result->balance);
                        state.data.balance_time = unix_now();
                    }
                }
            }
            continue;
        }
        auto wake = Clock::now() + 5min;
        if (state.settings.codex) wake = std::min(wake, codex_at);
        if (state.settings.deepseek) wake = std::min(wake, deepseek_at);
        state.changed.wait_until(lock, stop, wake, [&] { return settings_version != state.settings_version || refresh_version != state.refresh_version; });
    }
    lock.unlock();
    if (initialized) winrt::uninit_apartment();
}

void sample_frames(std::stop_token stop, Shared& state) {
    const bool permission = has_fps_permission();
    std::unique_lock lock(state.mutex);
    while (!stop.stop_requested()) {
        const auto exe = state.settings.presentmon;
        const bool enabled = state.settings.fps;
        const auto pid = state.data.hw.active_3d_pid;
        if (!enabled || exe.empty() || !permission || !pid) {
            state.fps.reset();
            state.fps_pid = 0;
            state.data.fps_status = !enabled ? L"已关闭 · 在编辑窗口开启" : exe.empty() ? L"待配置 · 在编辑窗口选择 PresentMon" :
                !permission ? L"缺少采集权限 · 见编辑窗口" : L"已就绪 · 切到有 3D 画面的前台应用";
            state.changed.wait(lock, stop, [&] { return state.settings.fps != enabled || state.settings.presentmon != exe || state.data.hw.active_3d_pid != pid; });
            continue;
        }
        if (state.fps_pid != pid) state.data.fps_status = L"正在采集前台应用";
        lock.unlock();
        const auto result = sample_fps(exe, pid, stop);
        lock.lock();
        if (!state.settings.fps || state.settings.presentmon != exe) continue;
        state.data.fps_status = result.status;
        state.fps_pid = pid;
        state.fps = result.value;
        state.changed.wait_for(lock, stop, result.value ? 350ms : 10s, [&] { return !state.settings.fps || state.settings.presentmon != exe || state.data.hw.active_3d_pid != pid; });
    }
}

void populate_demo(Shared& state) {
    const auto now = unix_now();
    auto& s = state.settings;
    s.codex = s.deepseek = true;
    auto& d = state.data;
    d.hw.cpu = 15; d.hw.cpu_temperature = 68; d.hw.gpu = 21; d.hw.gpu_temperature = 51; d.hw.ram = 60.5;
    d.hw.memory_total = 34359738368ULL; d.hw.memory_used = 20787682356ULL;
    d.hw.vram_total = 8589934592ULL; d.hw.vram_used = 2684354560ULL;
    d.hw.fps = 60; d.hw.network = Speed{600000, 25000};
    d.hw.cores = {15, 23, 14, 63, 22, 74, 9, 45, 17, 28, 4, 12, 25, 42, 12, 64};
    d.codex = CodexQuota{QuotaWindow{82, 300, now + 8000}, QuotaWindow{39, 10080, now + 217800}, now};
    d.balance = Balance{true, {{L"CNY", 48.5, 8.5, 40}, {L"USD", 12.28, 2.28, 10}}};
    d.balance_time = now; d.balance_status = L"演示余额"; d.fps_status = L"演示数据";
    for (int i = 0; i <= 60; ++i) {
        const auto t = static_cast<double>(i);
        d.history.push_back({now - 60 + i, 19 + 8 * std::sin(t * .37) + 8 * std::cos(t * .8), 35 + 25 * std::sin(t * .19) + 9 * std::sin(t * .8), 59 + 2 * std::sin(t * .22)});
    }
}

int write_probe(const wchar_t* path) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    {
        Sampler sampler;
        TaskbarFinder finder;
        sampler.sample(0, true);
        std::this_thread::sleep_for(1200ms);
        const auto h = sampler.sample(0, true);
        const auto gap = finder.sample();
        std::ofstream out{std::filesystem::path(path)};
        if (!out) return 3;
        const auto field = [&](const char* name, std::optional<double> value) {
            out << name << '=';
            if (value) out << *value;
            else out << "unavailable";
            out << '\n';
        };
        field("cpu", h.cpu); field("cpu_c", read_cpu_temperature()); field("gpu", h.gpu); field("gpu_c", h.gpu_temperature); field("ram", h.ram);
        field("upload_Bps", h.network ? std::optional{h.network->up} : std::nullopt);
        field("download_Bps", h.network ? std::optional{h.network->down} : std::nullopt);
        out << "logical_cores=" << h.cores.size() << "\nvram_bytes=" << h.vram_total << '\n';
        if (gap) out << "taskbar_gap=" << gap->left << ',' << gap->right << "\ndpi=" << gap->dpi << '\n';
        else out << "taskbar_gap=unavailable\n";
        if (!out) return 3;
    }
    CoUninitialize();
    return 0;
}

int write_balance_probe(const wchar_t* path) {
    winrt::init_apartment();
    const auto result = read_deepseek();
    winrt::uninit_apartment();
    std::ofstream output{std::filesystem::path(path)};
    output << "http_status=" << result.http_status << "\nvalid_balance=" << (result.balance ? 1 : 0) << '\n';
    return output ? 0 : 3;
}

class App {
public:
    explicit App(HINSTANCE instance, bool demo) : instance_(instance), demo_(demo) {}
    int run(Launch action);
private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static INT_PTR CALLBACK edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void update();
    void add_tray(bool add);
    void show_menu();
    void command(UINT id);
    void reload(bool clear_balance = false);
    void launch(Launch action);
    void open_edit();
    void close_edit();
    void set_details(bool open);
    void drag_bar();
    UINT display_dpi(const DisplayBounds& display);
    void update_bar(const Settings& settings, const Data& data, const std::optional<TaskbarGap>& gap, time_t now);
    void update_details(const Settings& settings, const Data& data, time_t now);
    void close_if_idle();
    void refresh();
    void change(const wchar_t* key, bool value);
    void warning(const wchar_t* text) { MessageBoxW(bar_, text, L"PulseBar", MB_OK | MB_ICONWARNING); }

    HINSTANCE instance_;
    bool demo_;
    HWND bar_ = nullptr, detail_ = nullptr, edit_ = nullptr, dpi_probe_ = nullptr;
    UINT taskbar_created_ = 0;
    bool details_open_ = false, status_open_ = false, have_surface_ = false, detail_moving_ = false;
    bool bar_dragging_ = false, drag_moved_ = false, edit_position_changed_ = false;
    UINT dpi_ = 96, detail_dpi_ = 96;
    std::optional<POINT> detail_position_;
    std::optional<POINT> edit_position_;
    POINT drag_start_{}, drag_cursor_{}, drag_grip_{};
    HWND last_foreground_ = nullptr;
    Clock::time_point next_page_{};
    unsigned page_ = 0;
    BYTE last_opacity_ = 255;
    bool last_light_ = false;
    RECT last_rect_{};
    BarPage drawn_;
    Canvas bar_canvas_;
    std::unique_ptr<Canvas> detail_canvas_;
    Shared state_;
};

int App::run(Launch action) {
    WNDCLASSEXW window{};
    window.cbSize = sizeof(window);
    window.hInstance = instance_;
    window.lpfnWndProc = window_proc;
    window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window.lpszClassName = bar_class;
    if (!RegisterClassExW(&window)) return 2;
    window.lpszClassName = details_class;
    if (!RegisterClassExW(&window)) return 2;
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    const auto ex = WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    bar_ = CreateWindowExW(ex | WS_EX_TRANSPARENT, bar_class, L"PulseBar", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
    if (!bar_) return 3;
    detail_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, details_class, L"PulseBar · 详情", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
    if (!detail_) { DestroyWindow(bar_); return 3; }
    state_.settings = demo_ ? Settings{} : load_settings();
    if (demo_) populate_demo(state_);
    detail_position_ = state_.settings.panel_position;
    add_tray(true);
    SetTimer(bar_, 1, 350, nullptr);
    std::jthread placement([&](std::stop_token stop) { sample_desktop(stop, state_); });
    std::jthread hardware, ai, fps, temperature;
    if (!demo_) {
        hardware = std::jthread([&](std::stop_token stop) { sample_hardware(stop, state_); });
        ai = std::jthread([&](std::stop_token stop) { sample_ai(stop, state_); });
        fps = std::jthread([&](std::stop_token stop) { sample_frames(stop, state_); });
        temperature = std::jthread([&](std::stop_token stop) { sample_temperature(stop, state_); });
    }
    launch(action);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (edit_ && IsDialogMessageW(edit_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    fps.request_stop(); ai.request_stop(); temperature.request_stop(); hardware.request_stop(); placement.request_stop();
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto info = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
    }
    const auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->dispatch(window, message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT App::dispatch(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == taskbar_created_ && message) {
        if (window == bar_) { add_tray(true); have_surface_ = false; }
        return 0;
    }
    switch (message) {
    case WM_NCHITTEST:
        if (window == bar_ && !edit_) return HTTRANSPARENT;
        if (window == detail_) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(detail_, &point);
            const int x = MulDiv(point.x, 96, detail_dpi_), y = MulDiv(point.y, 96, detail_dpi_);
            if (x >= 0 && x < 275 && y >= 0 && y < 48) return HTCAPTION;
        }
        return DefWindowProcW(window, message, wp, lp);
    case WM_MOUSEACTIVATE: return window == bar_ && !edit_ ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_TIMER: if (window == bar_) update(); return 0;
    case WM_DPICHANGED:
        if (window == detail_ && detail_moving_) {
            const auto& r = *reinterpret_cast<RECT*>(lp);
            SetWindowPos(detail_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        have_surface_ = false;
        return 0;
    case WM_DISPLAYCHANGE: have_surface_ = false; return 0;
    case WM_SETTINGCHANGE: if (window == bar_ && !demo_) reload(); return 0;
    case WM_ENTERSIZEMOVE: if (window == detail_) detail_moving_ = true; return 0;
    case WM_EXITSIZEMOVE:
        if (window == detail_) {
            detail_moving_ = false;
            RECT r{};
            if (GetWindowRect(detail_, &r)) {
                detail_position_ = POINT{r.left, r.top};
                const auto position = std::to_wstring(r.left) + L"," + std::to_wstring(r.top);
                if (!demo_ && !write_setting(L"panel_position", position.c_str())) warning(L"无法保存大面板位置。");
            }
            update();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (window == bar_ && edit_) {
            const POINT grip{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            drag_start_ = grip;
            if (ClientToScreen(bar_, &drag_start_)) {
                drag_grip_ = {MulDiv(grip.x, 96, dpi_), MulDiv(grip.y, 96, dpi_)};
                drag_moved_ = false;
                bar_dragging_ = true;
                SetCapture(bar_);
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (window == bar_ && bar_dragging_ && (wp & MK_LBUTTON)) drag_bar();
        return 0;
    case WM_CAPTURECHANGED: if (window == bar_) bar_dragging_ = false; return 0;
    case WM_LBUTTONUP:
        if (window == bar_) {
            if (bar_dragging_) { drag_bar(); ReleaseCapture(); }
        } else if (window == detail_) {
            const int x = MulDiv(GET_X_LPARAM(lp), 96, detail_dpi_), y = MulDiv(GET_Y_LPARAM(lp), 96, detail_dpi_);
            if (y >= 12 && y <= 45 && x >= 386) set_details(false);
            else if (y >= 12 && y <= 45 && x >= 282 && x <= 381) refresh();
        }
        return 0;
    case tray_message:
        if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_RBUTTONUP) show_menu();
        else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT || LOWORD(lp) == WM_LBUTTONUP) set_details(!details_open_);
        return 0;
    case launch_message: if (window == bar_) launch(static_cast<Launch>(wp)); return 1;
    case WM_APP + 5: return 2;
    case show_details_message: set_details(true); return 0;
    case WM_CLOSE:
        if (window == detail_) set_details(false);
        else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (window == bar_) {
            if (edit_) { DestroyWindow(edit_); edit_ = nullptr; }
            if (detail_) { DestroyWindow(detail_); detail_ = nullptr; }
            if (dpi_probe_) { DestroyWindow(dpi_probe_); dpi_probe_ = nullptr; }
            KillTimer(bar_, 1);
            add_tray(false);
            PostQuitMessage(0);
        }
        return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}

void App::drag_bar() {
    if (!GetCursorPos(&drag_cursor_)) return;
    if (!drag_moved_ && std::abs(drag_cursor_.x - drag_start_.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi_) &&
        std::abs(drag_cursor_.y - drag_start_.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) return;
    drag_moved_ = true;
    update();
}

void App::launch(Launch action) {
    switch (action) {
    case Launch::status: status_open_ = true; update(); break;
    case Launch::panel: set_details(true); break;
    case Launch::hide_status: status_open_ = false; if (edit_) close_edit(); else { update(); close_if_idle(); } break;
    case Launch::edit: open_edit(); break;
    case Launch::both: status_open_ = true; set_details(true); break;
    }
}

void App::set_details(bool open) {
    details_open_ = open;
    update();
    if (open && IsWindowVisible(detail_)) SetForegroundWindow(detail_);
    close_if_idle();
}

void App::close_if_idle() {
    if (!status_open_ && !details_open_ && !edit_) DestroyWindow(bar_);
}

void App::open_edit() {
    if (!edit_) {
        edit_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(103), nullptr, edit_proc, reinterpret_cast<LPARAM>(this));
        if (!edit_) { warning(L"无法打开编辑窗口。"); close_if_idle(); return; }
        const auto ex = GetWindowLongPtrW(bar_, GWL_EXSTYLE);
        SetWindowLongPtrW(bar_, GWL_EXSTYLE, ex & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE));
        SetWindowPos(bar_, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        have_surface_ = false;
    }
    status_open_ = true;
    SetWindowPos(edit_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
    SetForegroundWindow(edit_);
    update();
}

void App::close_edit() {
    if (!edit_) return;
    if (bar_dragging_) ReleaseCapture();
    if (edit_position_changed_) {
        const auto coordinates = edit_position_ ? std::to_wstring(edit_position_->x) + L"," + std::to_wstring(edit_position_->y) : std::wstring{};
        if (!demo_ && !write_setting(L"status_position", edit_position_ ? coordinates.c_str() : nullptr)) {
            warning(L"无法保存状态栏位置。");
            return;
        }
        const std::lock_guard lock(state_.mutex);
        state_.settings.status_position = edit_position_;
    }
    edit_position_changed_ = false;
    edit_position_.reset();
    DestroyWindow(edit_);
    edit_ = nullptr;
    const auto ex = GetWindowLongPtrW(bar_, GWL_EXSTYLE);
    SetWindowLongPtrW(bar_, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowPos(bar_, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    have_surface_ = false;
    update();
    close_if_idle();
}

INT_PTR CALLBACK App::edit_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const auto app = reinterpret_cast<App*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        const std::lock_guard lock(app->state_.mutex);
        CheckDlgButton(window, 201, app->state_.settings.fps ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, 202, app->state_.settings.cpu_thermal ? BST_CHECKED : BST_UNCHECKED);
        for (const int id : {201, 202, 203, 204, 205}) EnableWindow(GetDlgItem(window, id), !app->demo_);
        return TRUE;
    }
    const auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!app) return FALSE;
    if (message == WM_CLOSE) { app->close_edit(); return TRUE; }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(wp)) {
    case IDOK: case IDCANCEL: app->close_edit(); break;
    case 201: app->change(L"fps_enabled", IsDlgButtonChecked(window, 201) == BST_CHECKED); break;
    case 202: app->change(L"cpu_temperature", IsDlgButtonChecked(window, 202) == BST_CHECKED); break;
    case 203: app->command(menu_key); break;
    case 204: app->command(menu_fps); break;
    case 205: app->command(menu_config_file); break;
    case 206: app->edit_position_changed_ = true; app->edit_position_.reset(); app->update(); break;
    case 207:
        MessageBoxW(window, L"PresentMon 在前台应用有 3D 活动时采集 FPS。\n\n若提示权限不足，请由 Windows 管理员将当前账号加入 Performance Log Users（性能日志用户）组，然后注销并重新登录。", L"FPS 采集权限", MB_OK | MB_ICONINFORMATION);
        break;
    default: return FALSE;
    }
    return TRUE;
}

void App::update() {
    Settings settings;
    Data data;
    std::optional<TaskbarGap> gap;
    {
        const std::lock_guard lock(state_.mutex);
        settings = state_.settings;
        gap = state_.gap;
        data.hw = state_.data.hw;
        data.codex = state_.data.codex;
        data.balance = state_.data.balance;
        data.balance_time = state_.data.balance_time;
        if (!demo_) data.hw.fps = state_.fps_pid && state_.fps_pid == state_.foreground ? state_.fps : std::nullopt;
        if (details_open_) {
            data.history = state_.data.history;
            data.balance_status = state_.data.balance_status;
            data.fps_status = state_.data.fps_status;
        }
    }
    const auto now = unix_now();
    update_bar(settings, data, gap, now);
    update_details(settings, data, now);
}

UINT App::display_dpi(const DisplayBounds& display) {
    if (MonitorFromWindow(bar_, MONITOR_DEFAULTTONULL) == display.handle) {
        if (const auto dpi = GetDpiForWindow(bar_)) return dpi;
    }
    const auto x = display.screen.left, y = display.screen.top;
    if (!dpi_probe_) {
        dpi_probe_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", nullptr, WS_POPUP, x, y, 1, 1, nullptr, nullptr, instance_, nullptr);
    } else if (MonitorFromWindow(dpi_probe_, MONITOR_DEFAULTTONULL) != display.handle) {
        SetWindowPos(dpi_probe_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (const auto dpi = GetDpiForWindow(dpi_probe_)) return dpi;
    return dpi_;
}

void App::update_bar(const Settings& settings, const Data& data, const std::optional<TaskbarGap>& gap, time_t now) {
    if (!status_open_) { ShowWindow(bar_, SW_HIDE); return; }
    const auto time = Clock::now();
    if (!bar_dragging_ && time >= next_page_) {
        if (drawn_.pages > 1) ++page_;
        next_page_ = time + 6s;
    }
    const auto pin = edit_position_changed_ ? edit_position_ : settings.status_position;
    const bool moving = bar_dragging_ && drag_moved_;
    UINT dpi = gap ? gap->dpi : 96;
    const auto scale = [&](int dip) { return MulDiv(dip, dpi, 96); };
    BarPage page;
    RECT position{};
    if (!pin && !moving && gap) {
        page = bar_page(settings, data, MulDiv(gap->right - gap->left, 96, dpi), page_, now);
        const int width = scale(page.width), height = scale(bar_height);
        if (const auto x = anchor_x(gap->left, gap->right, width, scale(settings.offset)); x && gap->bar.bottom - gap->bar.top >= height) {
            const auto y = gap->bar.top + (gap->bar.bottom - gap->bar.top - height) / 2;
            position = {*x, y, *x + width, y + height};
        }
    }
    if (IsRectEmpty(&position)) {
        const auto display = display_at(moving ? drag_cursor_ : pin.value_or(POINT{}));
        if (!display) return;
        dpi = display_dpi(*display);
        const auto bounds = pin || moving ? display->screen : display->work;
        page = bar_page(settings, data, MulDiv(bounds.right - bounds.left, 96, dpi), page_, now);
        if (!page.width) { ShowWindow(bar_, SW_HIDE); return; }
        const int width = scale(page.width), height = scale(bar_height);
        auto target = pin.value_or(POINT{bounds.right - width - scale(12), bounds.bottom - height - scale(12)});
        if (moving) {
            target = {drag_cursor_.x - std::min(scale(drag_grip_.x), width - 1), drag_cursor_.y - std::min(scale(drag_grip_.y), height - 1)};
        }
        const int x = clamp_screen_axis(target.x, bounds.left, bounds.right, width);
        const int y = clamp_screen_axis(target.y, bounds.top, bounds.bottom, height);
        position = {x, y, x + width, y + height};
        if (moving) { edit_position_ = POINT{x, y}; edit_position_changed_ = true; }
    }
    const BYTE opacity = static_cast<BYTE>(255 * settings.opacity / 100);
    const int width = position.right - position.left, height = position.bottom - position.top;
    const bool dirty = !have_surface_ || page != drawn_ || dpi_ != dpi || opacity != last_opacity_ || settings.light != last_light_;
    if (dirty) {
        if (!bar_canvas_.begin(width, height, dpi)) return;
        paint_bar(bar_canvas_, page, settings.light, edit_ != nullptr);
        have_surface_ = bar_canvas_.present(bar_, position.left, position.top, opacity);
        if (!have_surface_) return;
        drawn_ = std::move(page);
        last_opacity_ = opacity;
        last_light_ = settings.light;
    }
    const auto foreground = GetForegroundWindow();
    if (!IsWindowVisible(bar_) || !EqualRect(&position, &last_rect_) || foreground != last_foreground_) {
        if (!SetWindowPos(bar_, HWND_TOPMOST, position.left, position.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW)) return;
    }
    last_foreground_ = foreground;
    last_rect_ = position;
    dpi_ = dpi;
}

void App::update_details(const Settings& settings, const Data& data, time_t now) {
    if (!details_open_) {
        if (IsWindowVisible(detail_)) ShowWindow(detail_, SW_HIDE);
        detail_canvas_.reset();
        return;
    }
    if (detail_moving_) return;
    MONITORINFO mon{};
    mon.cbSize = sizeof(mon);
    const POINT at = detail_position_.value_or(POINT{0, 0});
    if (!GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mon)) return;
    const bool visible = IsWindowVisible(detail_) != FALSE;
    if (!visible) SetWindowPos(detail_, nullptr, mon.rcWork.left, mon.rcWork.top, 1, 1, SWP_NOZORDER | SWP_NOACTIVATE);
    detail_dpi_ = GetDpiForWindow(detail_);
    if (!detail_dpi_) detail_dpi_ = 96;
    const auto scale = [&](int dip) { return MulDiv(dip, detail_dpi_, 96); };
    const int w = scale(details_width), h = scale(details_height);
    const int max_x = std::max(static_cast<int>(mon.rcWork.left), static_cast<int>(mon.rcWork.right) - w);
    const int max_y = std::max(static_cast<int>(mon.rcWork.top), static_cast<int>(mon.rcWork.bottom) - h);
    const int dx = std::clamp(detail_position_ ? static_cast<int>(at.x) : max_x - scale(30), static_cast<int>(mon.rcWork.left), max_x);
    const int dy = std::clamp(detail_position_ ? static_cast<int>(at.y) : static_cast<int>(mon.rcWork.top) + scale(55), static_cast<int>(mon.rcWork.top), max_y);
    if (!detail_canvas_) detail_canvas_ = std::make_unique<Canvas>();
    if (detail_canvas_->begin(w, h, detail_dpi_)) {
        paint_details(*detail_canvas_, settings, data, now);
        if (detail_canvas_->present(detail_, dx, dy) && !visible) SetWindowPos(detail_, HWND_TOP, dx, dy, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void App::add_tray(bool add) {
    NOTIFYICONDATAW item{};
    item.cbSize = sizeof(item);
    item.hWnd = bar_;
    item.uID = 1;
    if (!add) { Shell_NotifyIconW(NIM_DELETE, &item); return; }
    item.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    item.uCallbackMessage = tray_message;
    item.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(102));
    wcscpy_s(item.szTip, demo_ ? L"PulseBar · 演示" : L"PulseBar · 点击查看详情");
    if (Shell_NotifyIconW(NIM_ADD, &item)) { item.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &item); }
}

void App::show_menu() {
    Settings settings;
    { const std::lock_guard lock(state_.mutex); settings = state_.settings; }
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    const auto add = [&](UINT id, const wchar_t* name, bool checked = false, bool enable = true) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED) | (enable ? MF_ENABLED : MF_GRAYED), id, name);
    };
    add(menu_details, details_open_ ? L"关闭详情" : L"查看详情");
    add(menu_refresh, L"刷新额度", false, !demo_);
    add(menu_hidden, status_open_ ? L"关闭状态栏" : L"启动状态栏");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto modules = CreatePopupMenu();
    for (UINT i = 0; i < metric_count; ++i) AppendMenuW(modules, MF_STRING | (settings.visible[i] ? MF_CHECKED : MF_UNCHECKED), menu_metrics + i, metric_names[i]);
    AppendMenuW(menu, MF_POPUP | (demo_ ? MF_GRAYED : MF_ENABLED), reinterpret_cast<UINT_PTR>(modules), L"显示项目");
    add(menu_codex, L"读取本机 Codex 限额记录", settings.codex, !demo_);
    add(menu_key, L"配置 DeepSeek API Key…", false, !demo_);
    add(menu_deepseek, L"查询 DeepSeek 余额", settings.deepseek, !demo_);
    add(menu_forget, L"删除 DeepSeek API Key", false, !demo_ && deepseek_credential_exists());
    add(menu_fps, L"选择 PresentMon 控制台 EXE…", !settings.presentmon.empty(), !demo_);
    add(menu_stop_fps, L"自动采集 FPS", settings.fps, !demo_);
    add(menu_thermal, L"读取 CPU 温度", settings.cpu_thermal, !demo_);
    add(menu_nvidia, L"读取 NVIDIA 温度和显存", settings.nvidia_sensors, !demo_);
    add(menu_net, L"网速使用 Mbps", settings.megabits, !demo_);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(menu_startup, L"登录 Windows 时启动", runs_at_logon(), !demo_);
    add(menu_edit, L"编辑状态栏位置…");
    add(menu_config_file, L"编辑配置文件…", false, !demo_);
    add(menu_reload, L"重新读取设置", false, !demo_);
    add(menu_exit, L"退出 PulseBar");
    POINT at{};
    GetCursorPos(&at);
    SetForegroundWindow(bar_);
    const auto id = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, at.x, at.y, bar_, nullptr));
    PostMessageW(bar_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (id) command(id);
}

void App::command(UINT id) {
    if (id == menu_exit) { DestroyWindow(bar_); return; }
    if (id == menu_details) { set_details(!details_open_); return; }
    if (id == menu_edit) { open_edit(); return; }
    if (id == menu_hidden) { launch(status_open_ ? Launch::hide_status : Launch::status); return; }
    if (demo_) return;
    Settings settings;
    { const std::lock_guard lock(state_.mutex); settings = state_.settings; }
    switch (id) {
    case menu_refresh: refresh(); break;
    case menu_codex: change(L"codex", !settings.codex); break;
    case menu_key:
        if (auto key = prompt_key(edit_ ? edit_ : bar_, instance_)) {
            auto value = std::wstring_view(*key);
            const auto first = value.find_first_not_of(L" \t\r\n");
            if (first != value.npos) value = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
            if (value.starts_with(L"Bearer ")) value.remove_prefix(7);
            const bool stored = save_deepseek_key(value);
            SecureZeroMemory(key->data(), key->size() * sizeof(wchar_t));
            if (stored && write_setting(L"deepseek", L"1")) reload(true);
            else warning(L"无法启用 DeepSeek。请检查 API Key 格式和 Windows 凭据管理器。");
        }
        break;
    case menu_deepseek:
        if (!settings.deepseek && !deepseek_credential_exists()) command(menu_key);
        else change(L"deepseek", !settings.deepseek);
        break;
    case menu_forget:
        if (remove_deepseek_key() && write_setting(L"deepseek", L"0")) reload(true);
        else warning(L"无法删除 DeepSeek 凭据或保存设置。");
        break;
    case menu_fps:
        if (const auto path = select_presentmon(edit_ ? edit_ : bar_)) {
            if (write_setting(L"presentmon", path->c_str()) && write_setting(L"fps_enabled", L"1")) reload();
            else warning(L"无法保存 PresentMon 路径。");
        }
        break;
    case menu_stop_fps: change(L"fps_enabled", !settings.fps); break;
    case menu_thermal: change(L"cpu_temperature", !settings.cpu_thermal); break;
    case menu_nvidia: change(L"nvidia_sensors", !settings.nvidia_sensors); break;
    case menu_net: change(L"net_mbps", !settings.megabits); break;
    case menu_startup: if (!set_runs_at_logon(!runs_at_logon())) warning(L"无法修改登录启动设置。"); break;
    case menu_config_file: if (!open_settings_file(bar_)) warning(L"无法打开设置文件。"); break;
    case menu_reload: reload(); break;
    default:
        if (id >= menu_metrics && id < menu_metrics + metric_count) {
            const auto index = id - menu_metrics;
            const auto key = L"show_" + std::wstring(metric_names[index]);
            change(key.c_str(), !settings.visible[index]);
        }
    }
}

void App::change(const wchar_t* key, bool value) {
    if (write_setting(key, value ? L"1" : L"0")) reload();
    else warning(L"无法保存设置。");
}

void App::reload(bool clear_balance) {
    const auto settings = load_settings();
    {
        const std::lock_guard lock(state_.mutex);
        if (settings.codex != state_.settings.codex || settings.deepseek != state_.settings.deepseek || clear_balance) ++state_.settings_version;
        if (settings.fps != state_.settings.fps || settings.presentmon != state_.settings.presentmon) { state_.fps.reset(); state_.fps_pid = 0; }
        state_.settings = settings;
        if (!settings.codex) state_.data.codex.reset();
        if (clear_balance || !settings.deepseek) { state_.data.balance.reset(); state_.data.balance_time = 0; state_.data.balance_status.clear(); }
        state_.changed.notify_all();
    }
    if (edit_) {
        CheckDlgButton(edit_, 201, settings.fps ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(edit_, 202, settings.cpu_thermal ? BST_CHECKED : BST_UNCHECKED);
    }
    update();
}

void App::refresh() {
    if (demo_) return;
    const std::lock_guard lock(state_.mutex);
    ++state_.refresh_version;
    state_.changed.notify_all();
}
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace pulse;
    int argc = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> args(CommandLineToArgvW(GetCommandLineW(), &argc), LocalFree);
    bool demo = false;
    auto action = Launch::status;
    for (int i = 1; args && i < argc; ++i) {
        const std::wstring_view arg(args.get()[i]);
        if (arg == L"--probe" && i + 1 < argc) return write_probe(args.get()[i + 1]);
        if (arg == L"--probe-deepseek" && i + 1 < argc) return write_balance_probe(args.get()[i + 1]);
        if (arg == L"--demo") demo = true;
        if (arg == L"--show-status") action = Launch::status;
        if (arg == L"--panel") action = Launch::panel;
        if (arg == L"--hide-status") action = Launch::hide_status;
        if (arg == L"--edit") action = Launch::edit;
        if (arg == L"--details") action = Launch::both;
    }
    std::unique_ptr<void, decltype(&CloseHandle)> mutex(nullptr, CloseHandle);
    const auto deadline = Clock::now() + 3s;
    for (;;) {
        const auto handle = CreateMutexW(nullptr, FALSE, mutex_name);
        const auto status = GetLastError();
        if (!handle) return 1;
        mutex.reset(handle);
        if (status != ERROR_ALREADY_EXISTS) break;
        if (const auto existing = FindWindowW(bar_class, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(existing, WM_APP + 5, 0, 0, SMTO_ABORTIFHUNG, 500, &result) && result == 2) {
                return PostMessageW(existing, launch_message, static_cast<WPARAM>(action), 0) ? 0 : 1;
            }
            MessageBoxW(nullptr, L"请先在托盘退出旧版 PulseBar，再启动新版。", L"PulseBar", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
        mutex.reset();
        if (Clock::now() >= deadline) return 1;
        std::this_thread::sleep_for(60ms);
    }
    if (action == Launch::hide_status) return 0;
    const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    { App app(instance, demo); result = app.run(action); }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return result;
}
