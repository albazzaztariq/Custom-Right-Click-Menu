// menu_window.cpp
// Owner-drawn menu — modern Win11-style look.
// Rounded corners, off-white background, Segoe UI typography,
// per-item rounded highlight, right-side chevron for submenus.
//
// Submenus cascade on hover. Each level enters its own modal message
// loop. Outside-clicks dismiss the current level.

#include "menu_window.h"
#include "diag.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <wincodec.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <cmath>
#include <cstdint>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_ROUND 2
#endif

namespace menu_window {

namespace {

constexpr wchar_t kClass[]       = L"RCMenuApiWindow";
constexpr int     kItemHeight    = 40;
constexpr int     kIconW         = 28;     // colored placeholder square
constexpr int     kIconGap       = 12;
constexpr int     kPadLeft       = 16;
constexpr int     kPadRight      = 16;
constexpr int     kChevronW      = 20;
constexpr int     kFrameRadius   = 36;
constexpr int     kItemRadius    = 6;
constexpr int     kFramePadV     = 8;
constexpr int     kFramePadH     = 8;
// First/last separator rows are shorter than a normal item row so the
// top item and bottom item don't appear "stranded" against the
// rounded frame corners. Middle separators keep full row height
// because they're surrounded by items on both sides and the symmetry
// hides the 30 px gap. See row_height_at().
constexpr int     kEdgeSepHeight = 16;
constexpr int     kMinWidth      = 260;

// Colors (Win11-ish)
constexpr COLORREF kBg           = RGB(0xFF, 0xFF, 0xFF);  // box pure white
constexpr COLORREF kBorder       = RGB(0x00, 0x00, 0x80);  // navy
constexpr COLORREF kText         = RGB(0x1A, 0x1A, 0x1A);
constexpr COLORREF kTextMuted    = RGB(0x6A, 0x6A, 0x6A);
constexpr COLORREF kShortcut     = RGB(0x8B, 0x8B, 0x8B);
constexpr COLORREF kSepLine      = RGB(0x00, 0x00, 0x80);  // navy
constexpr COLORREF kHighlight    = RGB(0xE6, 0xE6, 0xE2);  // hover dims box

struct WindowData {
    const MenuNode* node               = nullptr;
    int             highlighted        = -1;
    int             submenu_opened_for = -1;
    int             spawning_item      = -1;   // index in owner that opened us (-1 = top-level)
    int             result             = -1;
    bool            done               = false;
    HFONT           font_text          = nullptr;
    HFONT           font_chevron       = nullptr;
    float           dpi_scale          = 1.0f;
    // Polled-mouse outside-click dismissal. WM_LBUTTONDOWN doesn't
    // reach us when the click lands on a foreign-process window
    // (WM_MOUSEACTIVATE activates that window first), so we poll
    // GetAsyncKeyState on a 25 ms timer instead.
    bool            prev_any_btn_down  = true;
};

// Per-thread scale factor for the current paint. Set at the top of
// show_impl() based on the monitor DPI; all pixel constants and font
// sizes go through S()/Sf() so the visual appearance is identical
// regardless of which host process (hook_test = DPI-Unaware, explorer
// = Per-Monitor v2) loads us. Position is left in physical pixels so
// the popup lands exactly at the cursor.
static thread_local float g_dpi_scale = 1.0f;
static inline int   S(int v)   { return (int)(v * g_dpi_scale + 0.5f); }
static inline float Sf(float v){ return v * g_dpi_scale; }

// Per-row height. Normal items + middle separators get kItemHeight;
// the first and last separator rows get kEdgeSepHeight so the top
// and bottom items sit visibly closer to their adjacent separator
// (the rounded frame corners exaggerate that gap otherwise).
static inline int row_height_at(const MenuNode& root, int i) {
    if (!root.children[i].is_separator()) return S(kItemHeight);
    int n = (int)root.children.size();
    if (i == 1) return S(kEdgeSepHeight);              // first sep
    if (i == n - 2) return S(kEdgeSepHeight);          // last sep
    return S(kItemHeight);
}

// Total content height (sum of all row heights + frame padding).
static inline int compute_total_height(const MenuNode& root) {
    int total = 2 * S(kFramePadV);
    for (int i = 0; i < (int)root.children.size(); ++i) {
        total += row_height_at(root, i);
    }
    return total;
}

// Y position of the top of row `i` relative to the menu's client area.
static inline int row_top_at(const MenuNode& root, int i) {
    int y = S(kFramePadV);
    for (int k = 0; k < i; ++k) y += row_height_at(root, k);
    return y;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        r.data(), n);
    return r;
}

// GDI+ kept only for icon rendering (Graphics::DrawImage with HQ bicubic).
// Text rendering uses D2D + DirectWrite for proper variable-font axis
// support and correct premultiplied alpha output for UpdateLayeredWindow.
static ULONG_PTR                  g_gdiplus_token = 0;
static ID2D1Factory*              g_d2d_factory    = nullptr;
static IDWriteFactory5*           g_dwrite_factory = nullptr;
static IDWriteFontCollection1*    g_font_collection = nullptr;

void ensure_custom_font_loaded() {
    static bool loaded = false;
    if (loaded) return;

    // GDI+ startup for icon rendering.
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);

    // D2D factory.
    HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory), nullptr,
                                     (void**)&g_d2d_factory);
    diag::log("D2D1CreateFactory hr=0x%08x ptr=%p", hr, g_d2d_factory);

    // DirectWrite factory — IDWriteFactory5 needed for FontSetBuilder
    // and variable-font axis support.
    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                               __uuidof(IDWriteFactory5),
                               (IUnknown**)&g_dwrite_factory);
    diag::log("DWriteCreateFactory hr=0x%08x ptr=%p", hr, g_dwrite_factory);

    if (!g_dwrite_factory) { loaded = true; return; }

    // Resolve the variable-font path next to the DLL.
    HMODULE me = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&ensure_custom_font_loaded),
                         &me);
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(me, buf, MAX_PATH);
    std::wstring dir(buf);
    auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    std::wstring font_path = dir + L"Font\\Exo2-VariableFont_wght.ttf";

    IDWriteFontFile* file = nullptr;
    hr = g_dwrite_factory->CreateFontFileReference(font_path.c_str(),
                                                   nullptr, &file);
    diag::log("DWrite font file %ls hr=0x%08x", font_path.c_str(), hr);

    if (file) {
        IDWriteFontSetBuilder1* builder = nullptr;
        if (SUCCEEDED(g_dwrite_factory->CreateFontSetBuilder(&builder))
                                                                && builder) {
            builder->AddFontFile(file);
            IDWriteFontSet* set = nullptr;
            if (SUCCEEDED(builder->CreateFontSet(&set)) && set) {
                hr = g_dwrite_factory->CreateFontCollectionFromFontSet(
                        set, &g_font_collection);
                diag::log("DWrite font collection hr=0x%08x ptr=%p",
                          hr, g_font_collection);
                if (g_font_collection) {
                    UINT32 fc = g_font_collection->GetFontFamilyCount();
                    diag::log("DWrite collection has %u families", fc);
                    for (UINT32 i = 0; i < fc; ++i) {
                        IDWriteFontFamily* fam = nullptr;
                        if (SUCCEEDED(g_font_collection->GetFontFamily(i, &fam)) && fam) {
                            IDWriteLocalizedStrings* names = nullptr;
                            if (SUCCEEDED(fam->GetFamilyNames(&names)) && names) {
                                wchar_t name[128] = {};
                                names->GetString(0, name, 128);
                                diag::log("  family[%u] = %ls", i, name);
                                names->Release();
                            }
                            fam->Release();
                        }
                    }
                }
                set->Release();
            }
            builder->Release();
        }
        file->Release();
    }

    loaded = true;
}

// DirectWrite text-pass context. One per paint_contents_text call.
struct DwContext {
    ID2D1RenderTarget*       rt;
    IDWriteFactory5*         dw;
    IDWriteFontCollection1*  collection;
    ID2D1SolidColorBrush*    brush;
};

// Create a DirectWrite text format at a given size + wght axis value.
IDWriteTextFormat* make_text_format(IDWriteFactory5* dw,
                                    IDWriteFontCollection1* collection,
                                    float size_pt, float weight,
                                    int align) {
    if (!dw || !collection) return nullptr;
    float size_dip = size_pt * 96.0f / 72.0f;

    IDWriteTextFormat* fmt = nullptr;
    HRESULT hr = dw->CreateTextFormat(L"Exo 2", collection,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      size_dip, L"en-us", &fmt);
    if (FAILED(hr) || !fmt) {
        static int s_log_once = 0;
        if (++s_log_once <= 3) {
            diag::log("CreateTextFormat hr=0x%08x size=%.1f weight=%.1f",
                      hr, size_pt, weight);
        }
        return nullptr;
    }

    IDWriteTextFormat3* fmt3 = nullptr;
    if (SUCCEEDED(fmt->QueryInterface(__uuidof(IDWriteTextFormat3),
                                      (void**)&fmt3)) && fmt3) {
        DWRITE_FONT_AXIS_VALUE axes[] = {
            { DWRITE_FONT_AXIS_TAG_WEIGHT, weight }
        };
        fmt3->SetFontAxisValues(axes, 1);
        fmt3->Release();
    }

    if (align == DT_RIGHT)       fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    else if (align == DT_CENTER) fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else                         fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return fmt;
}

void draw_text_dw(DwContext& c, const wchar_t* s, int len,
                  const RECT& r, COLORREF color,
                  float size_pt, int align,
                  float weight = 400.0f) {
    if (len <= 0 || !c.rt) return;
    IDWriteTextFormat* fmt = make_text_format(c.dw, c.collection,
                                              size_pt, weight, align);
    if (!fmt) return;

    c.brush->SetColor(D2D1::ColorF(GetRValue(color) / 255.0f,
                                   GetGValue(color) / 255.0f,
                                   GetBValue(color) / 255.0f, 1.0f));

    D2D1_RECT_F rd = D2D1::RectF((float)r.left, (float)r.top,
                                 (float)r.right, (float)r.bottom);
    c.rt->DrawText(s, (UINT32)len, fmt, &rd, c.brush,
                   D2D1_DRAW_TEXT_OPTIONS_NONE,
                   DWRITE_MEASURING_MODE_NATURAL);
    fmt->Release();
}

int measure_text_dw(DwContext& c, const wchar_t* s, int len,
                    float size_pt, float weight = 400.0f) {
    if (len <= 0 || !c.dw) return 0;
    IDWriteTextFormat* fmt = make_text_format(c.dw, c.collection,
                                              size_pt, weight, DT_LEFT);
    if (!fmt) return 0;

    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = c.dw->CreateTextLayout(s, (UINT32)len, fmt,
                                        10000.0f, 100.0f, &layout);
    int width = 0;
    if (SUCCEEDED(hr) && layout) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        width = (int)std::ceil(m.widthIncludingTrailingWhitespace);
        layout->Release();
    }
    fmt->Release();
    return width;
}

HFONT make_font(int size_pt, int weight = FW_NORMAL) {
    ensure_custom_font_loaded();
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    int height = -MulDiv(size_pt, dpi, 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,
                       L"Exo 2");
}

int compute_width(const MenuNode& node, HFONT font) {
    HDC dc = GetDC(nullptr);
    HFONT old = (HFONT)SelectObject(dc, font);

    int max_w = S(kMinWidth);
    for (const auto& c : node.children) {
        auto w = widen(c.text);
        SIZE sz{};
        GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz);
        int total = sz.cx + S(kPadLeft) + S(kIconW) + S(kIconGap)
                          + S(kPadRight) + S(kChevronW);
        if (total > max_w) max_w = total;
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return max_w;
}

// Rotate through a small palette so consecutive items get different
// colors. This is a placeholder until the API gains real icon support.
COLORREF icon_color_for(int idx) {
    static const COLORREF palette[] = {
        RGB(0x95, 0x88, 0xE8),   // purple
        RGB(0xF5, 0xA9, 0x52),   // orange
        RGB(0xE5, 0x6A, 0x7A),   // red
        RGB(0x6E, 0xA8, 0xF0),   // blue
        RGB(0x77, 0xC9, 0x8A),   // green
        RGB(0xE8, 0xC5, 0x47),   // gold
    };
    return palette[idx % (int)(sizeof(palette)/sizeof(palette[0]))];
}

// ─────────────────────────────────────────────────────────────────────
// PNG icon loader. Resolves consumer paths relative to
// <dll-dir>\assets\icons\ and caches HBITMAPs per path.
// ─────────────────────────────────────────────────────────────────────

struct IconBitmap {
    // We keep BOTH a GDI HBITMAP (legacy path) and a GDI+ Bitmap.
    // GDI+ is used for actual drawing — HighQualityBicubic during
    // the 56→28 downscale gives smoother edges than AlphaBlend.
    HBITMAP          hbm = nullptr;
    Gdiplus::Bitmap* gpb = nullptr;
    int              w   = 0;
    int              h   = 0;
};

std::wstring dll_directory() {
    HMODULE me = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&dll_directory), &me);
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(me, buf, MAX_PATH);
    std::wstring p(buf);
    auto slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p;
}

std::wstring resolve_icon_path(const std::string& consumer_path) {
    if (consumer_path.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0,
                                consumer_path.data(),
                                (int)consumer_path.size(),
                                nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, consumer_path.data(),
                        (int)consumer_path.size(), w.data(), n);
    // Absolute path? Use as-is.
    if (w.size() >= 2 && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'))) {
        return w;
    }
    return dll_directory() + L"assets\\icons\\" + w;
}

IconBitmap decode_png_to_dib(const std::wstring& path) {
    IconBitmap out;

    // Ensure COM is initialized on this thread — WIC fails with
    // CO_E_NOTINITIALIZED otherwise. CoInitializeEx returns S_FALSE
    // if already initialized, or S_OK if we just did it. We only
    // CoUninitialize in the latter case.
    HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool need_couninit = (co_hr == S_OK);

    IWICImagingFactory* fac = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&fac));
    if (FAILED(hr) || !fac) {
        diag::log("icon: WIC factory failed hr=0x%08x co_hr=0x%08x for %ls",
                  hr, co_hr, path.c_str());
        if (need_couninit) ::CoUninitialize();
        return out;
    }

    IWICBitmapDecoder* dec = nullptr;
    hr = fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                        WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr) || !dec) {
        diag::log("icon: decode %ls failed hr=0x%08x", path.c_str(), hr);
        fac->Release();
        return out;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    if (SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(fac->CreateFormatConverter(&conv)) && conv) {
            if (SUCCEEDED(conv->Initialize(frame,
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeCustom))) {
                UINT W = 0, H = 0;
                conv->GetSize(&W, &H);

                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth       = (LONG)W;
                bmi.bmiHeader.biHeight      = -(LONG)H;
                bmi.bmiHeader.biPlanes      = 1;
                bmi.bmiHeader.biBitCount    = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                void* bits = nullptr;
                HDC sc = GetDC(nullptr);
                HBITMAP hbm = CreateDIBSection(sc, &bmi, DIB_RGB_COLORS,
                                               &bits, nullptr, 0);
                ReleaseDC(nullptr, sc);
                if (hbm && bits) {
                    UINT stride = W * 4;
                    if (SUCCEEDED(conv->CopyPixels(nullptr, stride,
                                                   stride * H,
                                                   (BYTE*)bits))) {
                        out.hbm = hbm;
                        out.w   = (int)W;
                        out.h   = (int)H;
                    } else {
                        DeleteObject(hbm);
                    }
                }
            }
            conv->Release();
        }
        frame->Release();
    }
    dec->Release();
    fac->Release();
    if (need_couninit) ::CoUninitialize();
    diag::log("icon: decoded %ls -> %dx%d hbm=%p",
              path.c_str(), out.w, out.h, out.hbm);
    return out;
}

IconBitmap& icon_for(const std::string& consumer_path) {
    static std::unordered_map<std::string, IconBitmap> cache;
    static IconBitmap empty;
    if (consumer_path.empty()) return empty;
    auto it = cache.find(consumer_path);
    if (it != cache.end()) return it->second;

    IconBitmap b = decode_png_to_dib(resolve_icon_path(consumer_path));

    // Also load a GDI+ Bitmap from the same file for high-quality
    // scaled blitting. GDI+ uses WIC internally so this is the same
    // decode path, just exposed through a friendlier API.
    std::wstring path = resolve_icon_path(consumer_path);
    b.gpb = Gdiplus::Bitmap::FromFile(path.c_str(), FALSE);
    if (b.gpb && b.gpb->GetLastStatus() != Gdiplus::Ok) {
        delete b.gpb;
        b.gpb = nullptr;
    }

    auto ins = cache.emplace(consumer_path, b);
    return ins.first->second;
}

void blit_icon(HDC dst, const IconBitmap& ib, RECT target) {
    if (!ib.gpb && !ib.hbm) return;

    int tw = target.right  - target.left;
    int th = target.bottom - target.top;

    if (ib.gpb) {
        // GDI+ path — HighQualityBicubic downscale from the 2x
        // source PNG to the 28x28 target. Smoother than AlphaBlend.
        Gdiplus::Graphics g(dst);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        Gdiplus::Rect dr(target.left, target.top, tw, th);
        g.DrawImage(ib.gpb, dr, 0, 0,
                    ib.gpb->GetWidth(), ib.gpb->GetHeight(),
                    Gdiplus::UnitPixel);
        return;
    }

    HDC src = CreateCompatibleDC(dst);
    HGDIOBJ old = SelectObject(src, ib.hbm);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dst, target.left, target.top, tw, th,
               src, 0, 0, ib.w, ib.h, bf);
    SelectObject(src, old);
    DeleteDC(src);
}

void fill_rounded(HDC hdc, RECT r, int radius, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN   pn = CreatePen(PS_NULL, 0, 0);
    HBRUSH oldB = (HBRUSH)SelectObject(hdc, br);
    HPEN   oldP = (HPEN)  SelectObject(hdc, pn);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(br);
    DeleteObject(pn);
}

// PASS 1: All decoration via GDI+ on a PARGB Bitmap surface, so the
// silhouette anti-aliasing aligns with the border stroke and no GDI
// alpha-zeroing happens. Bg fill, highlights, separators, icons.
void paint_contents_gdi(int W, int H, void* bits, WindowData* d) {
    if (!bits) return;

    Gdiplus::Bitmap surface(W, H, W * 4,
                            PixelFormat32bppPARGB,
                            static_cast<BYTE*>(bits));
    std::unique_ptr<Gdiplus::Graphics> g(
        Gdiplus::Graphics::FromImage(&surface));
    g->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    const int IH  = S(kItemHeight);
    const int FPV = S(kFramePadV);
    const int FPH = S(kFramePadH);
    const int PL  = S(kPadLeft);
    const int PR  = S(kPadRight);
    const int IW  = S(kIconW);
    const int IR  = S(kItemRadius);
    const int FR  = S(kFrameRadius);

    auto build_rounded_path = [](Gdiplus::GraphicsPath& p,
                                 Gdiplus::REAL x, Gdiplus::REAL y,
                                 Gdiplus::REAL w_, Gdiplus::REAL h_,
                                 Gdiplus::REAL r) {
        p.Reset();
        Gdiplus::REAL d = 2 * r;
        p.AddArc(x,         y,         d, d, 180.0f, 90.0f);
        p.AddArc(x + w_ - d, y,         d, d, 270.0f, 90.0f);
        p.AddArc(x + w_ - d, y + h_ - d, d, d,   0.0f, 90.0f);
        p.AddArc(x,         y + h_ - d, d, d,  90.0f, 90.0f);
        p.CloseFigure();
    };

    // Silhouette fill with kBg — defines the visible shape. We KEEP
    // the path alive after the fill so we can use it as a clip region
    // for everything drawn afterwards (highlights, etc.). Without the
    // clip, an item-row highlight on the top/bottom row would render
    // a 6 px-radius rectangle that overflows the menu's 36 px outer
    // rounded corner. With the clip, every subsequent FillPath gets
    // shaped to the menu silhouette automatically.
    Gdiplus::GraphicsPath frame_path;
    build_rounded_path(frame_path, 0, 0,
                       (Gdiplus::REAL)W, (Gdiplus::REAL)H,
                       (Gdiplus::REAL)FR);
    {
        Gdiplus::SolidBrush bg(Gdiplus::Color(255,
            GetRValue(kBg), GetGValue(kBg), GetBValue(kBg)));
        g->FillPath(&bg, &frame_path);
    }
    g->SetClip(&frame_path, Gdiplus::CombineModeReplace);

    // Per-item content
    Gdiplus::SolidBrush highlightBrush(Gdiplus::Color(255,
        GetRValue(kHighlight), GetGValue(kHighlight), GetBValue(kHighlight)));
    Gdiplus::Pen sepPen(Gdiplus::Color(255,
        GetRValue(kSepLine), GetGValue(kSepLine), GetBValue(kSepLine)),
        1.5f);

    int cum_y = FPV;
    for (size_t i = 0; i < d->node->children.size(); ++i) {
        const auto& child = d->node->children[i];
        int rh = row_height_at(*d->node, (int)i);
        Gdiplus::REAL itemY = (Gdiplus::REAL)cum_y;
        Gdiplus::REAL itemX = (Gdiplus::REAL)FPH;
        Gdiplus::REAL itemW = (Gdiplus::REAL)(W - 2 * FPH);
        Gdiplus::REAL itemH = (Gdiplus::REAL)rh;

        if (child.is_separator()) {
            float sy = itemY + itemH / 2.0f;
            float sx0 = itemX + (float)PL;
            float sx1 = itemX + itemW - (float)PR;
            g->DrawLine(&sepPen, sx0, sy, sx1, sy);
        } else {
            if ((int)i == d->highlighted) {
                Gdiplus::GraphicsPath hpath;
                build_rounded_path(hpath, itemX, itemY, itemW, itemH,
                                   (Gdiplus::REAL)IR);
                g->FillPath(&highlightBrush, &hpath);
            }

            int icon_cy = (rh - IW) / 2;
            Gdiplus::Rect iconR((int)itemX + PL,
                                (int)itemY + icon_cy, IW, IW);
            if (!child.icon_path.empty()) {
                const IconBitmap& ib = icon_for(child.icon_path);
                if (ib.gpb) {
                    g->DrawImage(ib.gpb, iconR, 0, 0,
                                 ib.gpb->GetWidth(), ib.gpb->GetHeight(),
                                 Gdiplus::UnitPixel);
                }
            }
        }
        cum_y += rh;
    }
}

// PASS 2: All text via D2D + DirectWrite on a WIC bitmap that wraps
// the DIB as PARGB. This is the modern path with proper alpha and
// variable-font axis support.
void paint_contents_text(int W, int H, void* dib_bits, WindowData* d) {
    if (!dib_bits || !g_d2d_factory || !g_dwrite_factory
                  || !g_font_collection) return;

    constexpr float kLabelPt    = 10.0f;
    constexpr float kShortcutPt = 9.5f;
    constexpr float kChevronPt  = 12.0f;
    constexpr float kLabelWeight    = 450.0f;
    constexpr float kShortcutWeight = 250.0f;
    constexpr float kChevronWeight  = 700.0f;

    // WIC bitmap wraps the DIB memory directly (no copy).
    HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool need_couninit = (co_hr == S_OK);

    IWICImagingFactory* wic = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic))) || !wic) {
        if (need_couninit) ::CoUninitialize();
        return;
    }

    // Private WIC bitmap (NOT memory-shared) — CreateBitmapFromMemory
    // turns out NOT to alias the caller's memory in practice (verified
    // empirically: FillRectangle on the shared bitmap didn't show up
    // in the DIB). Draw text into this private buffer, then Lock and
    // composite back onto the DIB at the end.
    IWICBitmap* wic_bm = nullptr;
    UINT stride = (UINT)W * 4;
    HRESULT hr = wic->CreateBitmap(W, H,
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapCacheOnLoad, &wic_bm);
    if (FAILED(hr) || !wic_bm) {
        diag::log("WIC CreateBitmap hr=0x%08x", hr);
        wic->Release();
        if (need_couninit) ::CoUninitialize();
        return;
    }
    // Init to fully transparent so D2D text composites correctly and
    // areas without text don't blank out the DIB.
    {
        IWICBitmapLock* il = nullptr;
        WICRect wr0{0, 0, W, H};
        if (SUCCEEDED(wic_bm->Lock(&wr0, WICBitmapLockWrite, &il)) && il) {
            UINT ls = 0, lsize = 0;
            BYTE* lp = nullptr;
            il->GetStride(&ls);
            il->GetDataPointer(&lsize, &lp);
            if (lp) memset(lp, 0, lsize);
            il->Release();
        }
    }

    // Force 96 DPI so DIPs == pixels — matches the rest of the menu's
    // pixel-based layout. Default CreateWicBitmapRenderTarget would
    // pick up system DPI and scale text off the visible area on
    // high-DPI displays.
    D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ID2D1RenderTarget* rt = nullptr;
    hr = g_d2d_factory->CreateWicBitmapRenderTarget(wic_bm, rt_props, &rt);
    if (FAILED(hr) || !rt) {
        diag::log("D2D CreateWicBitmapRenderTarget hr=0x%08x", hr);
        wic_bm->Release();
        wic->Release();
        if (need_couninit) ::CoUninitialize();
        return;
    }

    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);

    DwContext ctx{ rt, g_dwrite_factory, g_font_collection, brush };

    rt->BeginDraw();

    // Scaled pixel constants for this paint.
    const int IH = S(kItemHeight);
    const int FPV = S(kFramePadV);
    const int FPH = S(kFramePadH);
    const int PL = S(kPadLeft);
    const int PR = S(kPadRight);
    const int IW = S(kIconW);
    const int IG = S(kIconGap);
    const int CW_ = S(kChevronW);
    const float labelPt    = Sf(kLabelPt);
    const float shortcutPt = Sf(kShortcutPt);
    const float chevronPt  = Sf(kChevronPt);

    int cum_y = FPV;
    for (size_t i = 0; i < d->node->children.size(); ++i) {
        const auto& child = d->node->children[i];
        int rh = row_height_at(*d->node, (int)i);
        if (child.is_separator()) { cum_y += rh; continue; }

        RECT ir{ FPH,
                 cum_y,
                 W - FPH,
                 cum_y + rh };
        cum_y += rh;

        int icon_right = ir.left + PL + IW;

        std::wstring shortcut_w;
        int shortcut_width = 0;
        if (child.is_selection() && !child.shortcut.empty()) {
            shortcut_w = widen(child.shortcut);
            shortcut_width = measure_text_dw(ctx, shortcut_w.c_str(),
                                             (int)shortcut_w.size(),
                                             shortcutPt,
                                             kShortcutWeight);
        }

        int right_reserve = child.is_submenu() ? CW_ : shortcut_width;
        int text_right = ir.right - PR - right_reserve;
        if (text_right < icon_right + IG + S(16)) {
            text_right = icon_right + IG + S(16);
        }

        std::wstring t = widen(child.text);
        RECT tr{ icon_right + IG, ir.top,
                 text_right, ir.bottom };
        draw_text_dw(ctx, t.c_str(), (int)t.size(), tr, kText,
                     labelPt, DT_LEFT, kLabelWeight);

        if (child.is_submenu()) {
            // U+2B9E (⮞) for all submenus. Drawn in Segoe UI Symbol
            // (Exo 2 lacks this glyph), navy via kBorder.
            const wchar_t* chev = L"⮞";

            IDWriteTextFormat* cfmt = nullptr;
            float chev_dip = chevronPt * 96.0f / 72.0f;
            if (SUCCEEDED(ctx.dw->CreateTextFormat(
                    L"Segoe UI Symbol", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    chev_dip, L"en-us", &cfmt)) && cfmt) {
                cfmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                cfmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                cfmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                ctx.brush->SetColor(D2D1::ColorF(
                    GetRValue(kBorder) / 255.0f,
                    GetGValue(kBorder) / 255.0f,
                    GetBValue(kBorder) / 255.0f, 1.0f));

                D2D1_RECT_F rd = D2D1::RectF(
                    (float)(ir.right - PR - CW_), (float)ir.top,
                    (float)(ir.right - PR),       (float)ir.bottom);
                ctx.rt->DrawText(chev, 1, cfmt, &rd, ctx.brush,
                                 D2D1_DRAW_TEXT_OPTIONS_NONE,
                                 DWRITE_MEASURING_MODE_NATURAL);
                cfmt->Release();
            }
        } else if (!shortcut_w.empty()) {
            RECT ar{ ir.right - PR - shortcut_width, ir.top,
                     ir.right - PR, ir.bottom };
            draw_text_dw(ctx, shortcut_w.c_str(),
                         (int)shortcut_w.size(),
                         ar, kShortcut, shortcutPt, DT_RIGHT,
                         kShortcutWeight);
        }
    }

    HRESULT flush_hr = rt->Flush(nullptr, nullptr);
    HRESULT end_hr = rt->EndDraw();
    static int s_end_logs = 0;
    if (++s_end_logs <= 3) {
        diag::log("D2D Flush hr=0x%08x EndDraw hr=0x%08x",
                  flush_hr, end_hr);
    }

    // Composite the private WIC bitmap (containing only text) onto
    // the DIB with premultiplied source-over.
    {
        IWICBitmapLock* il = nullptr;
        WICRect wr0{0, 0, W, H};
        if (SUCCEEDED(wic_bm->Lock(&wr0, WICBitmapLockRead, &il)) && il) {
            UINT ls = 0, lsize = 0;
            BYTE* lp = nullptr;
            il->GetStride(&ls);
            il->GetDataPointer(&lsize, &lp);
            if (lp) {
                uint32_t* dst  = static_cast<uint32_t*>(dib_bits);
                int srcStridePx = (int)(ls / 4);
                for (int y = 0; y < H; ++y) {
                    uint32_t* srcRow = reinterpret_cast<uint32_t*>(lp + y * ls);
                    uint32_t* dstRow = dst + y * W;
                    for (int x = 0; x < W; ++x) {
                        uint32_t s = srcRow[x];
                        uint32_t a = (s >> 24) & 0xFF;
                        if (a == 0) continue;
                        if (a == 255) { dstRow[x] = s; continue; }
                        uint32_t d_ = dstRow[x];
                        uint32_t dR = (d_ >> 16) & 0xFF;
                        uint32_t dG = (d_ >>  8) & 0xFF;
                        uint32_t dB = (d_      ) & 0xFF;
                        uint32_t sR = (s >> 16) & 0xFF;
                        uint32_t sG = (s >>  8) & 0xFF;
                        uint32_t sB = (s      ) & 0xFF;
                        uint32_t ia = 255 - a;
                        uint32_t rR = sR + (dR * ia + 127) / 255;
                        uint32_t rG = sG + (dG * ia + 127) / 255;
                        uint32_t rB = sB + (dB * ia + 127) / 255;
                        dstRow[x] = 0xFF000000 | (rR << 16) | (rG << 8) | rB;
                        (void)srcStridePx;
                    }
                }
            }
            il->Release();
        }
    }

    if (brush)  brush->Release();
    if (rt)     rt->Release();
    if (wic_bm) wic_bm->Release();
    if (wic)    wic->Release();
    if (need_couninit) ::CoUninitialize();
}

// Apply antialiased rounded-corner alpha to a 32bpp BGRA buffer.
// 4x4 supersampling per corner pixel produces visibly smooth edges.
// Pixels OUTSIDE the rounded region get alpha 0; pixels in the
// transition band get fractional alpha; everything else stays 255.
void apply_rounded_corner_alpha(uint32_t* px, int W, int H, int r) {
    auto set_alpha = [&](int x, int y, int a) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        uint32_t p = px[y * W + x];
        px[y * W + x] = (p & 0x00FFFFFF) | ((uint32_t)a << 24);
    };

    struct Corner { int cx, cy, x0, y0, x1, y1; };
    Corner corners[4] = {
        { r,     r,     0,     0,     r,    r    },   // TL: center at (r,r)
        { W - r, r,     W - r, 0,     W,    r    },   // TR
        { r,     H - r, 0,     H - r, r,    H    },   // BL
        { W - r, H - r, W - r, H - r, W,    H    },   // BR
    };

    constexpr int SS = 4;            // supersample N per axis
    constexpr int NSAMPLES = SS * SS;
    const double r2 = (double)r * r;

    for (auto& c : corners) {
        for (int y = c.y0; y < c.y1; ++y) {
            for (int x = c.x0; x < c.x1; ++x) {
                int inside = 0;
                for (int sy = 0; sy < SS; ++sy) {
                    for (int sx = 0; sx < SS; ++sx) {
                        double px_x = x + (sx + 0.5) / SS;
                        double px_y = y + (sy + 0.5) / SS;
                        double dx = px_x - c.cx;
                        double dy = px_y - c.cy;
                        if (dx * dx + dy * dy <= r2) ++inside;
                    }
                }
                int a = (inside * 255 + NSAMPLES / 2) / NSAMPLES;
                set_alpha(x, y, a);
            }
        }
    }
}

// Force alpha=255 for all pixels NOT in a corner zone, then
// premultiply RGB by alpha for the corner zones (UpdateLayeredWindow
// expects premultiplied BGRA when ULW_ALPHA is used).
void finalize_alpha(uint32_t* px, int W, int H, int r) {
    // Step 1: bulk set alpha to 255 across the whole buffer.
    for (int i = 0; i < W * H; ++i) {
        px[i] = (px[i] & 0x00FFFFFF) | 0xFF000000;
    }
    // Step 2: corner alpha (overwrites alpha for the small corner
    // squares with antialiased values, leaves the rest at 255).
    apply_rounded_corner_alpha(px, W, H, r);

    // Step 3: premultiply only the corner pixels (others are alpha=255
    // and need no change).
    auto premul = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (x < 0 || x >= W || y < 0 || y >= H) continue;
                uint32_t p = px[y * W + x];
                uint32_t a = (p >> 24) & 0xFF;
                if (a == 255) continue;
                uint32_t R = (p >> 16) & 0xFF;
                uint32_t G = (p >>  8) & 0xFF;
                uint32_t B = (p      ) & 0xFF;
                R = R * a / 255;
                G = G * a / 255;
                B = B * a / 255;
                px[y * W + x] = (a << 24) | (R << 16) | (G << 8) | B;
            }
        }
    };
    premul(0,     0,     r, r    );
    premul(W - r, 0,     W, r    );
    premul(0,     H - r, r, H    );
    premul(W - r, H - r, W, H    );
}

// Build the per-pixel layered surface and push it via
// UpdateLayeredWindow. Antialiased corners; opaque interior.
void paint_layered(HWND hwnd, WindowData* d) {
    RECT cr;
    GetClientRect(hwnd, &cr);
    int W = cr.right;
    int H = cr.bottom;
    if (W <= 0 || H <= 0) return;

    // Reinstate the per-thread DPI scale on every paint — WM_PAINT
    // can be called from any thread state, and we need S()/Sf() to
    // give consistent values.
    g_dpi_scale = d->dpi_scale;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = W;
    bmi.bmiHeader.biHeight      = -H;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    // Zero-init for clean transparent areas outside the silhouette.
    memset(bits, 0, (size_t)W * H * 4);

    // Pass 1 — All decoration via GDI+ on PARGB (silhouette fill,
    // highlights, icons, separators). Anti-aliasing is consistent
    // because everything goes through the same GDI+ surface.
    SelectObject(mem, oldBmp);   // detach DIB so D2D/GDI+ can read/write
    paint_contents_gdi(W, H, bits, d);

    // Pass 2 — D2D + DirectWrite text via WIC composite (alpha-correct).
    paint_contents_text(W, H, bits, d);
    oldBmp = SelectObject(mem, dib);

    // Border stroke — same path geometry as the silhouette so the
    // anti-aliasing aligns and there's no perimeter graininess.
    {
        Gdiplus::Bitmap surface(W, H, W * 4,
                                PixelFormat32bppPARGB,
                                static_cast<BYTE*>(bits));
        std::unique_ptr<Gdiplus::Graphics> g(
            Gdiplus::Graphics::FromImage(&surface));
        g->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        // PenAlignmentCenter (default) gives uniform stroke width
        // around curves; PenAlignmentInset bunches up at corners and
        // makes them visibly bolder. Path is inset by half-pen-width
        // so the stroke stays within the bitmap, and the corner
        // radius is reduced by the same amount so the stroke curve
        // matches the silhouette curve.
        const Gdiplus::REAL pw = 1.0f;
        Gdiplus::Pen pen(Gdiplus::Color(255,
                                        GetRValue(kBorder),
                                        GetGValue(kBorder),
                                        GetBValue(kBorder)), pw);

        Gdiplus::GraphicsPath path;
        Gdiplus::REAL r_adj = (Gdiplus::REAL)S(kFrameRadius) - pw / 2.0f;
        Gdiplus::REAL diam  = 2.0f * r_adj;
        Gdiplus::REAL fx    = pw / 2.0f;
        Gdiplus::REAL fy    = pw / 2.0f;
        Gdiplus::REAL fw    = (Gdiplus::REAL)W - pw;
        Gdiplus::REAL fh    = (Gdiplus::REAL)H - pw;

        path.AddArc(fx,             fy,             diam, diam, 180.0f, 90.0f);
        path.AddArc(fx + fw - diam, fy,             diam, diam, 270.0f, 90.0f);
        path.AddArc(fx + fw - diam, fy + fh - diam, diam, diam,   0.0f, 90.0f);
        path.AddArc(fx,             fy + fh - diam, diam, diam,  90.0f, 90.0f);
        path.CloseFigure();

        g->DrawPath(&pen, &path);
    }

    RECT wr;
    GetWindowRect(hwnd, &wr);
    POINT dstPt = { wr.left, wr.top };
    SIZE  sz    = { W, H };
    POINT srcPt = { 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, screen, &dstPt, &sz, mem, &srcPt,
                        0, &bf, ULW_ALPHA);

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void paint(HWND hwnd, WindowData* d) {
    paint_layered(hwnd, d);
}

int hittest(WindowData* d, int x, int y, RECT client) {
    if (x < client.left || x >= client.right ||
        y < client.top  || y >= client.bottom) return -1;
    // Cumulative scan — rows have variable heights (edge separators
    // are shorter), so we can't just divide y by a uniform height.
    int cy = S(kFramePadV);
    for (int i = 0; i < (int)d->node->children.size(); ++i) {
        int rh = row_height_at(*d->node, i);
        if (y >= cy && y < cy + rh) {
            if (d->node->children[i].is_separator()) return -1;
            return i;
        }
        cy += rh;
    }
    return -1;
}

int show_impl(const MenuNode& root, int x, int y, HWND owner,
              int spawning_item = -1);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<WindowData*>(
                  GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {

    case WM_PAINT:
        if (d) paint(hwnd, d);
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;   // handled in WM_PAINT, prevent flicker

    case WM_MOUSEMOVE: {
        if (!d) return 0;
        // Force-arrow on every move. WM_SETCURSOR is suppressed while
        // SetCapture is held, so if the system was showing the busy
        // cursor at the moment we grabbed capture it would otherwise
        // stay stuck. Cheap, idempotent.
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        RECT cr; GetClientRect(hwnd, &cr);
        int hi = hittest(d, x, y, cr);

        // Auto-dismiss this submenu only when the cursor is over a
        // *different* item in our owner than the one that spawned us.
        if (hi < 0) {
            HWND owner = GetWindow(hwnd, GW_OWNER);
            if (owner) {
                POINT scr; GetCursorPos(&scr);
                RECT or_rect; GetWindowRect(owner, &or_rect);
                if (PtInRect(&or_rect, scr)) {
                    auto* od = reinterpret_cast<WindowData*>(
                        GetWindowLongPtrW(owner, GWLP_USERDATA));
                    if (od) {
                        POINT cli = scr;
                        ScreenToClient(owner, &cli);
                        RECT ocr; GetClientRect(owner, &ocr);
                        int phi = hittest(od, cli.x, cli.y, ocr);
                        if (phi >= 0 && phi != d->spawning_item) {
                            d->done = true;
                            DestroyWindow(hwnd);
                            return 0;
                        }
                    }
                }
            }
        }

        if (hi != d->highlighted) {
            d->highlighted = hi;
            // Layered windows ignore InvalidateRect — repaint directly.
            paint_layered(hwnd, d);
        }

        if (hi >= 0 && hi != d->submenu_opened_for) {
            const auto& child = d->node->children[hi];
            if (child.is_submenu()) {
                RECT wr; GetWindowRect(hwnd, &wr);
                int sx = wr.right - 4;
                int sy = wr.top + row_top_at(*d->node, hi);
                diag::log("WM_MOUSEMOVE: opening submenu idx=%d at (%d,%d)",
                          hi, sx, sy);
                d->submenu_opened_for = hi;
                ReleaseCapture();
                int sub = show_impl(child, sx, sy, hwnd, hi);
                diag::log("submenu closed result=%d", sub);
                if (sub >= 0) {
                    d->result = sub;
                    d->done   = true;
                    DestroyWindow(hwnd);
                } else {
                    d->submenu_opened_for = -1;
                    SetCapture(hwnd);
                }
            }
        }
        return 0;
    }

    // Outside-click dismissal — primary path: polled mouse state.
    // Capture-routed WM_LBUTTONDOWN does NOT arrive when the click lands
    // on a foreign-process window, because WM_MOUSEACTIVATE activates
    // that window first and we lose foreground/capture before the
    // button-down message reaches us. So we poll GetAsyncKeyState on
    // a 25 ms timer instead. The WM_LBUTTONDOWN handler below remains
    // as a fast path for clicks that DO route here (rare on Win11).
    case WM_TIMER: {
        if (!d || wp != 1) return 0;
        // Only the deepest open menu reacts. While a submenu is up,
        // this window's auto-dismiss-on-sibling logic handles things.
        if (d->submenu_opened_for >= 0) return 0;

        bool ldown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool rdown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        bool mdown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        bool any_down = ldown || rdown || mdown;
        bool was_down = d->prev_any_btn_down;
        d->prev_any_btn_down = any_down;

        // Only react on a true up->down transition. This avoids
        // spuriously dismissing if the menu opens with a button still
        // physically pressed.
        if (!any_down || was_down) return 0;

        POINT pt; GetCursorPos(&pt);
        RECT wr; GetWindowRect(hwnd, &wr);
        if (PtInRect(&wr, pt)) {
            // Click is inside our window — let WM_LBUTTONDOWN /
            // WM_LBUTTONUP handle it (selection commit on release).
            return 0;
        }
        // Outside click: dismiss AND pass the click through to the
        // underlying window. While we held SetCapture the underlying
        // window never saw the mouse-down (so a title-bar click never
        // started a window drag, a tree-view click never selected,
        // etc). Tearing down our window first releases capture; then
        // we synthesize the down at the current cursor position so
        // the underlying window receives it. The user's still-pressed
        // physical button pairs with their eventual release.
        d->done = true;
        DestroyWindow(hwnd);

        DWORD flags = 0;
        if (ldown) flags |= MOUSEEVENTF_LEFTDOWN;
        if (rdown) flags |= MOUSEEVENTF_RIGHTDOWN;
        if (mdown) flags |= MOUSEEVENTF_MIDDLEDOWN;
        if (flags) {
            INPUT in = {};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = flags;
            ::SendInput(1, &in, sizeof(INPUT));
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!d) return 0;
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        RECT cr; GetClientRect(hwnd, &cr);
        int idx = hittest(d, x, y, cr);
        if (idx < 0) {
            d->done = true;
            DestroyWindow(hwnd);
        }
        // Inside-clicks: let WM_LBUTTONUP fire selection. This matches
        // standard menu UX where the action commits on release.
        return 0;
    }

    case WM_RBUTTONDOWN:
        if (d) { d->done = true; DestroyWindow(hwnd); }
        return 0;

    case WM_MBUTTONDOWN:
        if (d) { d->done = true; DestroyWindow(hwnd); }
        return 0;

    case WM_LBUTTONUP: {
        if (!d) return 0;
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        RECT cr; GetClientRect(hwnd, &cr);
        int idx = hittest(d, x, y, cr);

        if (idx < 0) {
            d->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        const auto& child = d->node->children[idx];
        if (child.is_selection()) {
            d->result = child.handlerId;
            d->done   = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_RBUTTONUP:
        if (d) { d->done = true; DestroyWindow(hwnd); }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && d) {
            d->done = true;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_KILLFOCUS:
        return 0;   // submenu cascading needs us to keep state

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (d) d->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_class_registered() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    // CS_DROPSHADOW removed: on Win11 24H2 it draws a hard rectangular
    // edge around layered windows that ULW_ALPHA cannot suppress.
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_SAVEBITS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // we paint everything ourselves
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    registered = true;
}

int show_impl(const MenuNode& root, int x, int y, HWND owner,
              int spawning_item) {
    ensure_class_registered();
    if (root.children.empty()) return -1;

    WindowData data;
    data.node          = &root;
    data.spawning_item = spawning_item;
    data.font_text     = make_font(9);
    data.font_chevron  = make_font(12);

    // dpi_scale fixed at 1.0 — render constants at their literal
    // pixel values regardless of monitor DPI. Menu is consistently
    // sized across hosts; explicitly user-tuned via the constants
    // at file scope.
    g_dpi_scale       = 1.0f;
    data.dpi_scale    = g_dpi_scale;

    int w = compute_width(root, data.font_text);
    int h = compute_total_height(root);

    // Clamp the menu to the work area of the monitor containing the
    // anchor point. Standard menu behavior: if the menu would extend
    // past the right or bottom edge, FLIP across the anchor so the
    // menu's right (or bottom) edge sits at the anchor instead of
    // its top-left. After flipping, clamp to the work area's left/top
    // in case the anchor itself was near an edge. This covers all
    // four sides for both the top-level menu and cascading submenus.
    {
        POINT anchor = { x, y };
        HMONITOR mon = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (::GetMonitorInfoW(mon, &mi)) {
            RECT wa = mi.rcWork;
            if (x + w > wa.right)  x = x - w;
            if (y + h > wa.bottom) y = y - h;
            if (x < wa.left)       x = wa.left;
            if (y < wa.top)        y = wa.top;
            // Final guard: if menu is taller/wider than the work area
            // (very unlikely), at least keep its top-left on-screen.
            if (x + w > wa.right)  x = wa.right - w;
            if (y + h > wa.bottom) y = wa.bottom - h;
        }
    }

    diag::log("menu_window::show ENTER owner=%p pos=(%d,%d) size=(%d,%d) "
              "children=%zu", owner, x, y, w, h, root.children.size());

    // WS_EX_LAYERED needed for UpdateLayeredWindow / per-pixel alpha.
    // No DwmSetWindowAttribute — layered windows render purely from
    // our DIB; DWM intervention (rounding, frame policy, etc.) only
    // adds an unwanted rectangular non-client frame around the alpha
    // mask we already drew.
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kClass, L"",
        WS_POPUP,
        x, y, w, h,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        diag::log("  CreateWindowExW FAILED GLE=%lu", ::GetLastError());
        if (data.font_text)    DeleteObject(data.font_text);
        if (data.font_chevron) DeleteObject(data.font_chevron);
        return -1;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&data);
    // Seed the polled-mouse dismissal state with whatever is currently
    // physically held, so the first up->down transition we react to is
    // a NEW press (not the right-click that opened us, which may still
    // be down at this exact instant).
    data.prev_any_btn_down =
        ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    // For layered windows, content has to be pushed via
    // UpdateLayeredWindow before ShowWindow shows anything visible.
    paint_layered(hwnd, &data);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    BOOL fg = SetForegroundWindow(hwnd);
    HWND prev_cap = SetCapture(hwnd);
    SetTimer(hwnd, 1, 25, nullptr);   // 40 Hz outside-click poll
    // Reset cursor at the moment of capture — if the system was
    // showing the busy cursor due to slow work just before us,
    // capture would otherwise freeze it.
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    diag::log("  hwnd=%p SetForegroundWindow=%d prev_cap=%p",
              hwnd, fg, prev_cap);

    MSG msg;
    int iters = 0;
    while (!data.done && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        ++iters;
    }
    ReleaseCapture();
    diag::log("menu_window::show EXIT result=%d iters=%d",
              data.result, iters);

    if (data.font_text)    DeleteObject(data.font_text);
    if (data.font_chevron) DeleteObject(data.font_chevron);
    return data.result;
}

}  // namespace

int show(const MenuNode& root, int x, int y, HWND owner) {
    return show_impl(root, x, y, owner);
}

}  // namespace menu_window
