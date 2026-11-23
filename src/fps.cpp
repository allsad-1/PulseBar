#include "fps.h"
#include <windows.h>
#include <evntrace.h>
#include <charconv>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace pulse {
namespace {
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    if (line.ends_with('\r')) line.remove_suffix(1);
    size_t start = 0;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        if (line[i] == ',' && !quoted) { result.push_back(line.substr(start, i - start)); start = i + 1; }
    }
    result.push_back(line.substr(start));
    return result;
}

}

std::optional<double> parse_presentmon_csv(std::string_view csv) {
    size_t swap = std::string::npos, duration = std::string::npos;
    std::map<std::string, std::pair<unsigned, double>> chains;
    while (!csv.empty()) {
        const auto line_end = csv.find('\n');
        const auto line = csv.substr(0, line_end);
        csv.remove_prefix(line_end == csv.npos ? csv.size() : line_end + 1);
        const auto row = fields(line);
        if (duration == std::string::npos) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (row[i] == "SwapChainAddress") swap = i;
                if (row[i] == "MsBetweenDisplayChange") duration = i;
            }
            continue;
        }
        if (swap >= row.size() || duration >= row.size()) continue;
        const auto raw = row[duration];
        double milliseconds = 0;
        const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), milliseconds);
        if (error != std::errc{} || end != raw.data() + raw.size() || !std::isfinite(milliseconds) || milliseconds < 0.1 || milliseconds > 5000) continue;
        auto& [count, total] = chains[std::string(row[swap])];
        ++count;
        total += milliseconds;
    }
    unsigned count = 1;
    std::optional<double> fps;
    for (const auto& [chain, stats] : chains) {
        if (stats.first > count) { count = stats.first; fps = 1000 * count / stats.second; }
    }
    return fps;
}

namespace {
void stop_trace(const std::wstring& name) {
    struct TraceStorage { EVENT_TRACE_PROPERTIES props; wchar_t name[96]; } storage{};
    storage.props.Wnode.BufferSize = sizeof(storage);
    storage.props.LoggerNameOffset = offsetof(TraceStorage, name);
    ControlTraceW(0, name.c_str(), &storage.props, EVENT_TRACE_CONTROL_STOP);
}
}

bool has_fps_permission() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    for (const DWORD group : {DOMAIN_ALIAS_RID_ADMINS, DOMAIN_ALIAS_RID_LOGGING_USERS}) {
        PSID sid = nullptr;
        if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, group, 0, 0, 0, 0, 0, 0, &sid)) continue;
        BOOL member = FALSE;
        CheckTokenMembership(nullptr, sid, &member);
        FreeSid(sid);
        if (member) return true;
    }
    return false;
}

FpsResult sample_fps(const std::filesystem::path& executable, unsigned pid, std::stop_token stop) {
    if (!executable.is_absolute() || executable.root_name().native().size() != 2 || executable.root_name().native()[1] != L':' ||
        GetDriveTypeW(executable.root_path().c_str()) != DRIVE_FIXED || _wcsicmp(executable.extension().c_str(), L".exe") != 0) return {{}, L"请选择本地 PresentMon 控制台 EXE"};
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inheritable, 0)) return {{}, L"无法创建 FPS 数据管道"};
    const Handle output(read, CloseHandle);
    Handle child_output(write, CloseHandle);
    if (!SetHandleInformation(output.get(), HANDLE_FLAG_INHERIT, 0)) return {{}, L"无法设置 FPS 数据管道"};
    const Handle nul(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr), CloseHandle);
    if (nul.get() == INVALID_HANDLE_VALUE) return {{}, L"无法创建 FPS 数据管道"};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul.get();
    startup.hStdOutput = child_output.get();
    startup.hStdError = child_output.get();
    PROCESS_INFORMATION child{};
    const auto name = L"PulseBar-" + std::to_wstring(GetCurrentProcessId());
    std::wstring args = L"\"" + executable.native() + L"\" --process_id " + std::to_wstring(pid) +
        L" --v1_metrics --output_stdout --no_console_stats --exclude_dropped --no_track_input --no_track_gpu --timed 3 --terminate_after_timed --terminate_on_proc_exit --session_name " + name;
    if (!CreateProcessW(executable.c_str(), args.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &child)) return {{}, L"无法启动 PresentMon"};
    const Handle process(child.hProcess, CloseHandle);
    CloseHandle(child.hThread);
    child_output.reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::string csv;
    char buffer[4096];
    bool limited = false;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0, received = 0;
        if (PeekNamedPipe(output.get(), nullptr, 0, nullptr, &available, nullptr) && available) {
            if (!ReadFile(output.get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &received, nullptr)) break;
            if (csv.size() + received > 8 * 1024 * 1024) { limited = true; break; }
            csv.append(buffer, received);
        } else if (WaitForSingleObject(process.get(), 60) == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(output.get(), nullptr, 0, nullptr, &available, nullptr) || !available) break;
        }
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process.get(), &code) || code == STILL_ACTIVE) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 500);
        stop_trace(name);
        return {{}, limited ? L"帧率样本过大" : L"帧率采集已停止"};
    }
    if (code != 0) return {{}, csv.find("access denied") != csv.npos ? L"权限不足 · 请在编辑窗口查看权限说明" : L"PresentMon 采集失败 · 请检查控制台版本"};
    const auto fps = parse_presentmon_csv(csv);
    return {fps, fps ? L"PresentMon · 前台应用" : L"等待前台画面呈现"};
}
}
