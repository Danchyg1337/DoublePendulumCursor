#include "CursorController.h"
#include "Config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <stdexcept>
#include <vector>

CursorController::CursorController() {
    sizeBytes_ = cfg::CANVAS * cfg::CANVAS * 4;

    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(BITMAPV5HEADER);
    bi.bV5Width       = cfg::CANVAS;
    bi.bV5Height      = -cfg::CANVAS;   // negative = top-down DIB
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    dc_ = hdc;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (hdc) ReleaseDC(nullptr, hdc);
        throw std::runtime_error("CreateDIBSection failed");
    }
    hbmColor_ = color;
    bits_ = bits;

    // All-zero AND mask: a 32-bpp colour bitmap with an alpha channel drives
    // transparency through alpha, so the mask just needs to be fully opaque(0).
    hbmMask_ = CreateBitmap(cfg::CANVAS, cfg::CANVAS, 1, 1, nullptr);
    if (hbmMask_) {
        // CreateBitmap leaves contents undefined; zero it explicitly.
        const int maskBytes = ((cfg::CANVAS + 15) / 16) * 2 * cfg::CANVAS;
        std::vector<std::uint8_t> zeros(static_cast<std::size_t>(maskBytes), 0);
        SetBitmapBits(static_cast<HBITMAP>(hbmMask_), maskBytes, zeros.data());
    }
}

CursorController::~CursorController() {
    if (hbmColor_) DeleteObject(static_cast<HBITMAP>(hbmColor_));
    if (hbmMask_)  DeleteObject(static_cast<HBITMAP>(hbmMask_));
    if (dc_)       ReleaseDC(nullptr, static_cast<HDC>(dc_));
}

void CursorController::installCursor(const std::uint8_t* bgra, int ocrId) {
    std::memcpy(bits_, bgra, static_cast<std::size_t>(sizeBytes_));

    ICONINFO ii{};
    ii.fIcon    = FALSE;                 // FALSE = cursor (uses hotspot)
    ii.xHotspot = cfg::HOTSPOT_X;
    ii.yHotspot = cfg::HOTSPOT_Y;
    ii.hbmMask  = static_cast<HBITMAP>(hbmMask_);
    ii.hbmColor = static_cast<HBITMAP>(hbmColor_);

    HCURSOR hcur = CreateIconIndirect(&ii);
    if (hcur) {
        // SetSystemCursor takes ownership and destroys hcur -- never free it.
        SetSystemCursor(hcur, static_cast<DWORD>(ocrId));
    }
}

const void* CursorController::loadSlotHandle(int ocrId) const {
    return LoadCursorW(nullptr, MAKEINTRESOURCEW(ocrId));
}

const void* CursorController::activeCursorHandle() const {
    CURSORINFO ci{};
    ci.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&ci)) return nullptr;
    return ci.hCursor;
}

int CursorController::refreshRateHz(int def) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(DEVMODEW);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        return def;
    const DWORD hz = dm.dmDisplayFrequency;
    if (hz <= 1) return def;   // 0/1 means "hardware default", i.e. unknown
    return static_cast<int>(hz);
}

void CursorController::cursorPos(long& x, long& y) {
    POINT pt{};
    GetCursorPos(&pt);
    x = pt.x;
    y = pt.y;
}

void CursorController::restore() {
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}
