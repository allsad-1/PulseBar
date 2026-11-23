#include "telemetry.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <string_view>

namespace pulse {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

uint64_t ticks(FILETIME time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::optional<Interfaces> interface_counters() {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) return {};
    const std::unique_ptr<MIB_IF_TABLE2, decltype(&FreeMibTable)> owner(table, FreeMibTable);
    Interfaces result;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const auto& item = table->Table[index];
        if (item.OperStatus != IfOperStatusUp || !item.InterfaceAndOperStatusFlags.HardwareInterface ||
            (item.Type != IF_TYPE_ETHERNET_CSMACD && item.Type != IF_TYPE_IEEE80211 && item.Type != IF_TYPE_WWANPP && item.Type != IF_TYPE_WWANPP2)) continue;
        result[item.InterfaceLuid.Value] = {item.InOctets, item.OutOctets};
    }
    return result;
}

class Nvml {
public:
    Nvml() : library_(LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!library_) return;
        const auto init = symbol<int (*)()>("nvmlInit_v2");
        const auto get_device = symbol<int (*)(unsigned, void**)>("nvmlDeviceGetHandleByIndex_v2");
        shutdown_ = symbol<int (*)()>("nvmlShutdown");
        initialized_ = init && shutdown_ && init() == 0;
        if (!initialized_ || !get_device || get_device(0, &device_) != 0) { device_ = nullptr; return; }
        utilization_ = symbol<int (*)(void*, Utilization*)>("nvmlDeviceGetUtilizationRates");
        temperature_ = symbol<int (*)(void*, unsigned, unsigned*)>("nvmlDeviceGetTemperature");
        memory_ = symbol<int (*)(void*, Memory*)>("nvmlDeviceGetMemoryInfo");
    }
    ~Nvml() {
        if (initialized_) shutdown_();
        if (library_) FreeLibrary(library_);
    }
    void sample(Hardware& result) const {
        if (!device_) return;
        Utilization load{};
        unsigned temperature = 0;
        Memory memory{};
        if (utilization_ && utilization_(device_, &load) == 0 && load.gpu <= 100) result.gpu = std::max(result.gpu.value_or(0), static_cast<double>(load.gpu));
        if (temperature_ && temperature_(device_, 0, &temperature) == 0 && temperature > 0 && temperature < 150) result.gpu_temperature = temperature;
        if (memory_ && memory_(device_, &memory) == 0 && memory.used <= memory.total) {
            result.vram_total = memory.total;
            result.vram_used = memory.used;
        }
    }
private:
    template<typename T> T symbol(const char* name) const {
        const auto raw = GetProcAddress(library_, name);
        static_assert(sizeof(T) == sizeof(raw));
        return std::bit_cast<T>(raw);
    }
    struct Utilization { unsigned gpu, memory; };
    struct Memory { uint64_t total, free, used; };
    HMODULE library_ = nullptr;
    bool initialized_ = false;
    void* device_ = nullptr;
    int (*shutdown_)() = nullptr;
    int (*utilization_)(void*, Utilization*) = nullptr;
    int (*temperature_)(void*, unsigned, unsigned*) = nullptr;
    int (*memory_)(void*, Memory*) = nullptr;
};

template<typename F>
bool read_pdh(PDH_HCOUNTER counter, std::vector<unsigned char>& storage, F use) {
    if (!counter) return false;
    DWORD bytes = static_cast<DWORD>(storage.size()), count = 0;
    auto status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data()));
    if (status == PDH_MORE_DATA && bytes < 4 * 1024 * 1024) {
        storage.resize(bytes);
        status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data()));
    }
    if (status != ERROR_SUCCESS) return false;
    const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
    for (DWORD index = 0; index < count; ++index) {
        const auto& item = items[index];
        if (item.FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || item.FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) use(std::wstring_view(item.szName), item.FmtValue.doubleValue);
    }
    return true;
}

ComPtr<IEnumWbemClassObject> query_wmi(const wchar_t* ns_name, const wchar_t* query_text) {
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) return {};
    const auto free_bstr = [](wchar_t* value) { SysFreeString(value); };
    using Bstr = std::unique_ptr<wchar_t, decltype(free_bstr)>;
    const Bstr ns(SysAllocString(ns_name), free_bstr);
    ComPtr<IWbemServices> services;
    if (FAILED(locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &services))) return {};
    const Bstr wql(SysAllocString(L"WQL"), free_bstr);
    const Bstr query(SysAllocString(query_text), free_bstr);
    ComPtr<IEnumWbemClassObject> rows;
    if (FAILED(services->ExecQuery(wql.get(), query.get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows))) return {};
    return rows;
}

std::optional<double> cpu_thermal_zone() {
    const auto rows = query_wmi(L"ROOT\\WMI", L"SELECT InstanceName, CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    if (!rows) return {};
    for (unsigned index = 0; index < 16; ++index) {
        ULONG count = 0;
        ComPtr<IWbemClassObject> row;
        if (rows->Next(100, 1, row.GetAddressOf(), &count) != WBEM_S_NO_ERROR || !count) break;
        VARIANT name{}, value{};
        row->Get(L"InstanceName", 0, &name, nullptr, nullptr);
        row->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr);
        std::wstring label = name.vt == VT_BSTR && name.bstrVal ? name.bstrVal : L"";
        std::transform(label.begin(), label.end(), label.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
        const double raw = value.vt == VT_I4 ? static_cast<double>(value.lVal) : value.vt == VT_UI4 ? static_cast<double>(value.ulVal) : 0;
        const double celsius = raw / 10.0 - 273.15;
        VariantClear(&name);
        VariantClear(&value);
        if (label.find(L"CPU") != label.npos && celsius > 0 && celsius < 125) return celsius;
    }
    return {};
}

std::optional<double> libre_cpu_package() {
    const auto rows = query_wmi(L"ROOT\\LibreHardwareMonitor", L"SELECT Name, Identifier, Value FROM Sensor WHERE SensorType = 'Temperature'");
    if (!rows) return {};
    for (unsigned index = 0; index < 256; ++index) {
        ULONG count = 0;
        ComPtr<IWbemClassObject> row;
        if (rows->Next(250, 1, row.GetAddressOf(), &count) != WBEM_S_NO_ERROR || !count) break;
        VARIANT name{}, id{}, value{};
        row->Get(L"Name", 0, &name, nullptr, nullptr);
        row->Get(L"Identifier", 0, &id, nullptr, nullptr);
        row->Get(L"Value", 0, &value, nullptr, nullptr);
        const std::wstring_view label = name.vt == VT_BSTR && name.bstrVal ? name.bstrVal : L"";
        const std::wstring_view sensor = id.vt == VT_BSTR && id.bstrVal ? id.bstrVal : L"";
        const bool cpu_package = (sensor.starts_with(L"/intelcpu/") || sensor.starts_with(L"/amdcpu/")) &&
            (label == L"CPU Package" || label == L"Core (Tctl/Tdie)" || label == L"CPU (Tctl/Tdie)" || label == L"CPU Die (average)");
        const double temperature = value.vt == VT_R4 ? value.fltVal : value.vt == VT_R8 ? value.dblVal : -100;
        VariantClear(&name);
        VariantClear(&id);
        VariantClear(&value);
        if (cpu_package && temperature > 0 && temperature < 125) return temperature;
    }
    return {};
}
}

std::optional<double> read_cpu_temperature() {
    if (const auto firmware = cpu_thermal_zone()) return firmware;
    return libre_cpu_package();
}

struct Sampler::Impl {
    std::optional<CpuTicks> cpu;
    std::optional<Interfaces> network;
    Clock::time_point last_network{};
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpu = nullptr, cores = nullptr;
    std::vector<unsigned char> pdh_buffer;
    std::optional<Nvml> nvml;

    Impl() {
        if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu);
            PdhAddEnglishCounterW(query, L"\\Processor Information(*)\\% Processor Time", 0, &cores);
        }
    }
    ~Impl() { if (query) PdhCloseQuery(query); }
};

Sampler::Sampler() : impl_(std::make_unique<Impl>()) {}
Sampler::~Sampler() = default;

Hardware Sampler::sample(unsigned foreground_pid, bool read_nvidia) {
    auto& impl = *impl_;
    Hardware result;
    const auto now = Clock::now();
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const CpuTicks current{ticks(idle), ticks(kernel), ticks(user)};
        if (impl.cpu) result.cpu = cpu_percent(*impl.cpu, current);
        impl.cpu = current;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys) {
        result.memory_total = memory.ullTotalPhys;
        result.memory_used = memory.ullTotalPhys - memory.ullAvailPhys;
        result.ram = 100.0 * static_cast<double>(result.memory_used) / static_cast<double>(result.memory_total);
    }
    auto network = interface_counters();
    if (network && impl.network) result.network = network_speed(*impl.network, *network, std::chrono::duration<double>(now - impl.last_network).count());
    impl.network = std::move(network);
    impl.last_network = now;
    if (impl.query && PdhCollectQueryData(impl.query) == ERROR_SUCCESS) {
        std::map<std::wstring, double, std::less<>> engines;
        const auto focus = L"pid_" + std::to_wstring(foreground_pid) + L"_";
        const bool gpu_available = read_pdh(impl.gpu, impl.pdh_buffer, [&](std::wstring_view name, double value) {
            const auto engine = name.find(L"_luid_");
            if (engine == name.npos || value < 0) return;
            auto physical = name.substr(engine);
            if (const auto suffix = physical.find(L'#'); suffix != physical.npos) physical = physical.substr(0, suffix);
            engines[std::wstring(physical)] += value;
            if (foreground_pid && value >= 0.25 && name.starts_with(focus) && physical.ends_with(L"_engtype_3D")) result.active_3d_pid = foreground_pid;
        });
        if (gpu_available) result.gpu = 0;
        for (const auto& [name, value] : engines) result.gpu = std::max(result.gpu.value_or(0), std::min(100.0, value));
        read_pdh(impl.cores, impl.pdh_buffer, [&](std::wstring_view name, double value) {
            if (!name.ends_with(L"_Total")) result.cores.push_back(std::clamp(value, 0.0, 100.0));
        });
    }
    if (read_nvidia) {
        if (!impl.nvml) impl.nvml.emplace();
        impl.nvml->sample(result);
    } else if (impl.nvml) {
        impl.nvml.reset();
    }
    return result;
}
}
