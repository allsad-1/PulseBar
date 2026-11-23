#include "render.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <array>

namespace pulse {
using Microsoft::WRL::ComPtr;

struct Canvas::Impl {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> write;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::array<ComPtr<IDWriteTextFormat>, 5> fonts;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ original = nullptr;
    int width = 0, height = 0;
    bool writing = false;

    void reset_target() { brush.Reset(); target.Reset(); }
    ~Impl() {
        reset_target();
        if (bmp) { SelectObject(dc, original); DeleteObject(bmp); }
        if (dc) DeleteDC(dc);
    }
};

Canvas::Canvas() : impl_(std::make_unique<Impl>()) {
    auto& c = *impl_;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, c.d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(c.write.GetAddressOf()));
    if (!c.write) return;
    const auto add = [&](Font font, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
        auto& format = c.fonts[static_cast<size_t>(font)];
        if (SUCCEEDED(c.write->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format))) {
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };
    add(Font::label, L"Segoe UI", 10, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    add(Font::value, L"Consolas", 12, DWRITE_FONT_WEIGHT_BOLD);
    add(Font::body, L"Segoe UI", 13, DWRITE_FONT_WEIGHT_NORMAL);
    add(Font::title, L"Segoe UI", 22, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    add(Font::compact, L"Consolas", 10, DWRITE_FONT_WEIGHT_NORMAL);
}
Canvas::~Canvas() = default;

bool Canvas::begin(int width, int height, UINT dpi) {
    auto& c = *impl_;
    if (!c.d2d || !c.write || width <= 0 || height <= 0) return false;
    if (!c.dc) c.dc = CreateCompatibleDC(nullptr);
    if (!c.dc) return false;
    if (!c.bmp || c.width != width || c.height != height) {
        BITMAPINFO info{};
        auto& header = info.bmiHeader;
        header.biSize = sizeof(header);
        header.biWidth = width;
        header.biHeight = -height;
        header.biPlanes = 1;
        header.biBitCount = 32;
        void* pixels = nullptr;
        const auto bmp = CreateDIBSection(c.dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bmp) return false;
        const auto old = SelectObject(c.dc, bmp);
        if (!old || old == HGDI_ERROR) { DeleteObject(bmp); return false; }
        if (c.bmp) DeleteObject(c.bmp);
        else c.original = old;
        c.bmp = bmp;
        c.width = width;
        c.height = height;
    }
    if (!c.target) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(c.d2d->CreateDCRenderTarget(&props, &c.target))) return false;
        if (FAILED(c.target->CreateSolidColorBrush(color(0), &c.brush))) { c.reset_target(); return false; }
    }
    const RECT bounds{0, 0, width, height};
    if (FAILED(c.target->BindDC(c.dc, &bounds))) { c.reset_target(); return false; }
    c.target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    c.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    c.target->BeginDraw();
    c.target->Clear(D2D1::ColorF(0, 0.0f));
    c.writing = true;
    return true;
}

void Canvas::rect(float x, float y, float w, float h, float radius, D2D1_COLOR_F value) {
    auto& c = *impl_;
    c.brush->SetColor(value);
    c.target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), c.brush.Get());
}
void Canvas::outline(float x, float y, float w, float h, float radius, D2D1_COLOR_F value) {
    auto& c = *impl_;
    c.brush->SetColor(value);
    c.target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), c.brush.Get());
}
void Canvas::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F value, float stroke) {
    auto& c = *impl_;
    c.brush->SetColor(value);
    c.target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), c.brush.Get(), stroke);
}
void Canvas::text(std::wstring_view value, Font font, float x, float y, float w, float h, D2D1_COLOR_F ink, Align align) {
    auto& c = *impl_;
    auto* format = c.fonts[static_cast<size_t>(font)].Get();
    if (!format || value.empty() || w <= 0 || h <= 0) return;
    format->SetTextAlignment(align == Align::middle ? DWRITE_TEXT_ALIGNMENT_CENTER : align == Align::right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    c.brush->SetColor(ink);
    c.target->DrawText(value.data(), static_cast<UINT32>(value.size()), format, D2D1::RectF(x, y, x + w, y + h), c.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool Canvas::present(HWND window, int x, int y, BYTE opacity) {
    auto& c = *impl_;
    if (!c.writing) return false;
    c.writing = false;
    if (FAILED(c.target->EndDraw())) { c.reset_target(); return false; }
    POINT at{x, y}, origin{};
    SIZE size{c.width, c.height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    return UpdateLayeredWindow(window, nullptr, &at, &size, c.dc, &origin, 0, &blend, ULW_ALPHA) != FALSE;
}
}
