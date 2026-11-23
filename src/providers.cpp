#include "providers.h"
#include <windows.h>
#include <wincred.h>
#include <winhttp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

namespace pulse {
namespace {
using namespace winrt::Windows::Data::Json;
namespace fs = std::filesystem;
constexpr wchar_t credential_name[] = L"PulseBar/DeepSeek";

bool valid_key(std::wstring_view key) {
    return !key.empty() && key.size() <= 512 && key.find(L"...") == key.npos &&
        key.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == key.npos;
}

std::optional<double> amount(const winrt::hstring& value) {
    const auto bytes = winrt::to_string(value);
    double result{};
    const auto [end, error] = std::from_chars(bytes.data(), bytes.data() + bytes.size(), result);
    if (error != std::errc{} || end != bytes.data() + bytes.size() || !std::isfinite(result)) return {};
    return result;
}

time_t parse_timestamp(const winrt::hstring& stamp) {
    if (stamp.size() < 20 || stamp.size() > 32 || stamp.back() != L'Z' || stamp[10] != L'T') return 0;
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (swscanf_s(stamp.c_str(), L"%4u-%2u-%2uT%2u:%2u:%2u", &year, &month, &day, &hour, &minute, &second) != 6 || year < 1970 || year > 9999) return 0;
    const SYSTEMTIME time{static_cast<WORD>(year), static_cast<WORD>(month), 0, static_cast<WORD>(day), static_cast<WORD>(hour), static_cast<WORD>(minute), static_cast<WORD>(second), 0};
    FILETIME utc{};
    if (!SystemTimeToFileTime(&time, &utc)) return 0;
    const ULARGE_INTEGER ticks{{utc.dwLowDateTime, utc.dwHighDateTime}};
    return static_cast<time_t>(ticks.QuadPart / 10000000ULL - 11644473600ULL);
}

std::unique_ptr<CREDENTIALW, decltype(&CredFree)> credential() {
    PCREDENTIALW item = nullptr;
    CredReadW(credential_name, CRED_TYPE_GENERIC, 0, &item);
    return {item, CredFree};
}

using Internet = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
}

std::optional<Balance> parse_balance(std::string_view json) {
    try {
        const auto root = JsonObject::Parse(winrt::to_hstring(json));
        Balance result{root.GetNamedBoolean(L"is_available"), {}};
        for (const auto& value : root.GetNamedArray(L"balance_infos")) {
            const auto item = value.GetObject();
            const auto currency = item.GetNamedString(L"currency");
            if (currency != L"CNY" && currency != L"USD") continue;
            const auto total = amount(item.GetNamedString(L"total_balance"));
            const auto granted = amount(item.GetNamedString(L"granted_balance"));
            const auto topped = amount(item.GetNamedString(L"topped_up_balance"));
            if (!total || !granted || !topped) return {};
            result.entries.push_back({std::wstring(currency), *total, *granted, *topped});
        }
        return result.entries.empty() ? std::nullopt : std::optional{std::move(result)};
    } catch (const winrt::hresult_error&) { return {}; }
}

std::optional<CodexQuota> parse_codex(std::string_view line) {
    if (line.find("\"token_count\"") == line.npos || line.find("\"rate_limits\"") == line.npos) return {};
    try {
        const auto root = JsonObject::Parse(winrt::to_hstring(line));
        if (root.GetNamedString(L"type", L"") != L"event_msg") return {};
        const auto payload = root.GetNamedObject(L"payload");
        if (payload.GetNamedString(L"type", L"") != L"token_count") return {};
        const auto limits = payload.GetNamedObject(L"rate_limits");
        const auto id_field = limits.GetNamedValue(L"limit_id", JsonValue::CreateNullValue());
        const auto id = id_field.ValueType() == JsonValueType::String ? id_field.GetString() : winrt::hstring{};
        if (!id.empty() && id != L"codex") return {};
        CodexQuota result;
        result.observed = parse_timestamp(root.GetNamedString(L"timestamp", L""));
        if (!result.observed) return {};
        for (const auto* key : {L"primary", L"secondary"}) {
            const auto value = limits.GetNamedValue(key, JsonValue::CreateNullValue());
            if (value.ValueType() != JsonValueType::Object) continue;
            const auto window = value.GetObject();
            const double duration = window.GetNamedNumber(L"window_minutes", 0);
            const double used = window.GetNamedNumber(L"used_percent", -1);
            const auto reset_value = window.GetNamedValue(L"resets_at", JsonValue::CreateNullValue());
            const double reset = reset_value.ValueType() == JsonValueType::Number ? reset_value.GetNumber() : 0;
            if (used < 0 || used > 100 || duration < 1 || duration > 525600 || reset < 0 || reset > 253402300799.0) continue;
            const QuotaWindow quota{100 - used, static_cast<int>(duration), static_cast<time_t>(reset)};
            if (duration >= 295 && duration <= 300) result.five_hour = quota;
            if (duration >= 10075 && duration <= 10080) result.week = quota;
        }
        return result.five_hour || result.week ? std::optional{result} : std::nullopt;
    } catch (const winrt::hresult_error&) { return {}; }
}

std::optional<CodexQuota> read_codex() {
    const auto environment = [](const wchar_t* key) -> fs::path {
        wchar_t path[32768];
        const auto size = GetEnvironmentVariableW(key, path, static_cast<DWORD>(std::size(path)));
        return size && size < std::size(path) ? fs::path(path) : fs::path{};
    };
    auto home = environment(L"CODEX_HOME");
    if (home.empty()) {
        home = environment(L"USERPROFILE");
        if (home.empty()) return {};
        home /= L".codex";
    }
    std::vector<std::pair<fs::file_time_type, fs::path>> candidates;
    const auto today = unix_now();
    for (int day = 0; day < 8; ++day) {
        const time_t date = today - day * 86400;
        tm local{};
        if (localtime_s(&local, &date)) continue;
        wchar_t relative[40];
        wcsftime(relative, std::size(relative), L"%Y/%m/%d", &local);
        std::error_code error;
        for (fs::directory_iterator entry(home / L"sessions" / relative, error), end; !error && entry != end; entry.increment(error)) {
            const auto file = entry->path().filename().native();
            if (!file.starts_with(L"rollout-") || !file.ends_with(L".jsonl")) continue;
            std::error_code file_error;
            if (fs::is_symlink(entry->symlink_status(file_error)) || file_error || !entry->is_regular_file(file_error)) continue;
            const auto modified = entry->last_write_time(file_error);
            if (!file_error) candidates.emplace_back(modified, entry->path());
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::optional<CodexQuota> best;
    constexpr std::streamoff tail = 384 * 1024;
    for (size_t index = 0; index < std::min<size_t>(candidates.size(), 12); ++index) {
        const auto& path = candidates[index].second;
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        const auto size = stream.tellg();
        if (size <= 0) continue;
        const auto start = std::max<std::streamoff>(0, size - tail);
        stream.seekg(start);
        std::string data(static_cast<size_t>(size - start), '\0');
        stream.read(data.data(), static_cast<std::streamsize>(data.size()));
        data.resize(static_cast<size_t>(stream.gcount()));
        auto end = data.rfind('\n');
        while (end != std::string::npos && end > 0) {
            const auto previous = data.rfind('\n', end - 1);
            if (previous == std::string::npos && start != 0) break;
            const auto first = previous == std::string::npos ? 0 : previous + 1;
            const std::string_view line(data.data() + first, end - first);
            if (line.size() <= 65536 && line.find("\"type\":\"event_msg\"") != line.npos && line.find("\"type\":\"token_count\"") != line.npos) {
                auto quota = parse_codex(line);
                if (quota && (!best || quota->observed > best->observed)) best = quota;
                if (quota) break;
            }
            end = previous;
        }
        SecureZeroMemory(data.data(), data.size());
    }
    return best;
}

bool deepseek_credential_exists() {
    auto item = credential();
    if (!item) return false;
    SecureZeroMemory(item->CredentialBlob, item->CredentialBlobSize);
    return true;
}

bool save_deepseek_key(std::wstring_view key) {
    if (!valid_key(key)) return false;
    CREDENTIALW item{};
    item.Type = CRED_TYPE_GENERIC;
    item.TargetName = const_cast<LPWSTR>(credential_name);
    item.CredentialBlobSize = static_cast<DWORD>(key.size() * sizeof(wchar_t));
    item.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(key.data()));
    item.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&item, 0) != FALSE;
}

bool remove_deepseek_key() {
    return CredDeleteW(credential_name, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
}

BalanceResult read_deepseek() {
    auto item = credential();
    if (!item) return {{}, L"尚未配置 API Key"};
    if (!item->CredentialBlobSize || item->CredentialBlobSize % sizeof(wchar_t) != 0 || item->CredentialBlobSize > 1024) {
        SecureZeroMemory(item->CredentialBlob, item->CredentialBlobSize);
        return {{}, L"保存的 API Key 格式无效"};
    }
    const std::wstring_view key(reinterpret_cast<const wchar_t*>(item->CredentialBlob), item->CredentialBlobSize / sizeof(wchar_t));
    if (!valid_key(key)) {
        SecureZeroMemory(item->CredentialBlob, item->CredentialBlobSize);
        return {{}, L"保存的 API Key 格式无效"};
    }
    std::wstring auth = L"Authorization: Bearer ";
    auth.append(key);
    auth.append(L"\r\n");
    SecureZeroMemory(item->CredentialBlob, item->CredentialBlobSize);
    const Internet session(WinHttpOpen(L"PulseBar/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    if (session) WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, 4000);
    const Internet connection(session ? WinHttpConnect(session.get(), L"api.deepseek.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr, WinHttpCloseHandle);
    const Internet request(connection ? WinHttpOpenRequest(connection.get(), L"GET", L"/user/balance", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr, WinHttpCloseHandle);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    const bool header = request && WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) &&
        WinHttpAddRequestHeaders(request.get(), auth.c_str(), static_cast<DWORD>(auth.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    SecureZeroMemory(auth.data(), auth.size() * sizeof(wchar_t));
    const bool sent = header && WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr)) return {{}, L"网络请求失败", true};
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return {{}, L"无法读取响应", true};
    if (status == 401) return {{}, L"401 · 请在编辑窗口更换完整 API Key", false, status};
    if (status == 403) return {{}, L"403 · 此 Key 无权查询余额", false, status};
    if (status != 200) return {{}, L"HTTP " + std::to_wstring(status), status == 429 || status >= 500, status};
    std::string data;
    char block[4096];
    DWORD read = 0;
    do {
        if (!WinHttpReadData(request.get(), block, sizeof(block), &read)) return {{}, L"响应读取失败", true};
        if (data.size() + read > 65536) return {{}, L"响应体过大"};
        data.append(block, read);
    } while (read);
    auto balance = parse_balance(data);
    return balance ? BalanceResult{std::move(balance), L"已同步", false, status} : BalanceResult{{}, L"余额响应格式无效", false, status};
}
}
