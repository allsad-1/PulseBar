#include "taskbar.h"
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwchar>

namespace pulse {
namespace {
using Microsoft::WRL::ComPtr;

bool window_class(HWND window, const wchar_t* expected) {
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return wcscmp(name, expected) == 0;
}
}

struct TaskbarFinder::Impl {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationCondition> buttons;
    ComPtr<IUIAutomationCacheRequest> properties;
};

TaskbarFinder::TaskbarFinder() : impl_(std::make_unique<Impl>()) {
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl_->automation)))) return;
    ComPtr<IUIAutomation2> automation2;
    if (SUCCEEDED(impl_->automation.As(&automation2))) {
        automation2->put_ConnectionTimeout(300);
        automation2->put_TransactionTimeout(350);
    }
    VARIANT control{};
    control.vt = VT_I4;
    control.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> button, toggle, list;
    impl_->automation->CreatePropertyCondition(UIA_ControlTypePropertyId, control, &button);
    control.lVal = UIA_CheckBoxControlTypeId;
    impl_->automation->CreatePropertyCondition(UIA_ControlTypePropertyId, control, &toggle);
    control.lVal = UIA_ListItemControlTypeId;
    impl_->automation->CreatePropertyCondition(UIA_ControlTypePropertyId, control, &list);
    if (button && toggle && list) {
        IUIAutomationCondition* conditions[]{button.Get(), toggle.Get(), list.Get()};
        impl_->automation->CreateOrConditionFromNativeArray(conditions, static_cast<int>(std::size(conditions)), &impl_->buttons);
    }
    if (SUCCEEDED(impl_->automation->CreateCacheRequest(&impl_->properties))) {
        impl_->properties->put_AutomationElementMode(AutomationElementMode_None);
        for (const PROPERTYID key : {UIA_IsOffscreenPropertyId, UIA_BoundingRectanglePropertyId, UIA_AutomationIdPropertyId, UIA_ClassNamePropertyId}) {
            if (FAILED(impl_->properties->AddProperty(key))) { impl_->properties.Reset(); break; }
        }
    }
}
TaskbarFinder::~TaskbarFinder() = default;

std::optional<TaskbarGap> TaskbarFinder::sample() {
    const auto bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!bar || !IsWindowVisible(bar) || !impl_->buttons || !impl_->properties) return {};
    TaskbarGap result;
    result.dpi = GetDpiForWindow(bar);
    if (!result.dpi || !GetWindowRect(bar, &result.bar)) return {};
    const auto& r = result.bar;
    const int h = r.bottom - r.top;
    if (h < MulDiv(18, result.dpi, 96) || r.right - r.left <= 2 * h) return {};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONULL), &monitor)) return {};
    RECT clipped{};
    if (!IntersectRect(&clipped, &monitor.rcMonitor, &r) || clipped.bottom - clipped.top < h * 3 / 4) return {};

    RECT tray_rect{};
    const auto tray = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray || !IsWindowVisible(tray) || !GetWindowRect(tray, &tray_rect) || tray_rect.left <= r.left || tray_rect.right > r.right) return {};
    result.right = tray_rect.left;
    result.left = r.left;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(impl_->automation->ElementFromHandle(bar, &root))) return {};
    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, impl_->buttons.Get(), impl_->properties.Get(), &elements))) return {};
    int count = 0;
    if (FAILED(elements->get_Length(&count))) return {};
    bool start_button = false;
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> item;
        if (FAILED(elements->GetElement(index, &item))) continue;
        BOOL offscreen = TRUE;
        RECT bounds{};
        if (FAILED(item->get_CachedIsOffscreen(&offscreen)) || offscreen || FAILED(item->get_CachedBoundingRectangle(&bounds))) continue;
        if (bounds.right <= bounds.left || bounds.left < r.left || bounds.right > r.right || bounds.bottom <= r.top || bounds.top >= r.bottom) continue;
        BSTR id = nullptr, kind = nullptr;
        item->get_CachedAutomationId(&id);
        item->get_CachedClassName(&kind);
        start_button = start_button || (id && wcscmp(id, L"StartButton") == 0);
        const bool system_tray = kind && wcsncmp(kind, L"SystemTray.", 11) == 0;
        if (system_tray) result.right = std::min(result.right, static_cast<int>(bounds.left));
        else if (bounds.left < tray_rect.left) result.left = std::max(result.left, static_cast<int>(bounds.right));
        SysFreeString(id);
        SysFreeString(kind);
    }
    const int margin = MulDiv(9, result.dpi, 96);
    result.left += margin;
    result.right -= margin;
    if (!start_button) result.left = result.right;
    return result;
}

std::optional<DisplayBounds> display_at(POINT point) {
    const auto display = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(display, &monitor)) return {};
    return DisplayBounds{display, monitor.rcMonitor, monitor.rcWork};
}

unsigned foreground_process() {
    DWORD pid = 0;
    const auto front = GetForegroundWindow();
    if (!front || front == GetShellWindow() || window_class(front, L"Shell_TrayWnd") || window_class(front, L"Progman") || window_class(front, L"WorkerW")) return 0;
    GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId() ? 0 : pid;
}
}
