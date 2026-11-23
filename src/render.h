#pragma once
#include <windows.h>
#include <d2d1.h>
#include <string_view>
#include <memory>

namespace pulse {
enum class Font { label, value, body, title, compact };
enum class Align { left, middle, right };

class Canvas {
public:
    Canvas();
    ~Canvas();
    bool begin(int width, int height, UINT dpi);
    void rect(float x, float y, float w, float h, float radius, D2D1_COLOR_F color);
    void outline(float x, float y, float w, float h, float radius, D2D1_COLOR_F color);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float stroke = 1);
    void text(std::wstring_view value, Font font, float x, float y, float w, float h, D2D1_COLOR_F color, Align align = Align::left);
    bool present(HWND window, int x, int y, BYTE opacity = 255);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline D2D1_COLOR_F color(UINT32 rgb, float alpha = 1) { return D2D1::ColorF(rgb, alpha); }
}
