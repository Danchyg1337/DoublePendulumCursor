// CursorController.h -- the only Windows-specific piece.
//
// Owns a single reusable 32-bpp DIB section and monochrome mask, so installing
// a new cursor image each frame is just a memcpy + CreateIconIndirect +
// SetSystemCursor, with no per-frame CreateDIBSection / CreateBitmap /
// DeleteObject churn (the original Python allocated all of those every frame).
#pragma once

#include <cstdint>

class CursorController {
public:
    CursorController();
    ~CursorController();

    CursorController(const CursorController&) = delete;
    CursorController& operator=(const CursorController&) = delete;

    // Copy CANVAS*CANVAS BGRA8 pixels into the shared DIB and install the
    // resulting cursor into the given OCR_* system-cursor slot.
    void installCursor(const std::uint8_t* bgra, int ocrId);

    // Look up the stable handle of a system-cursor slot (SetSystemCursor only
    // swaps a slot's bitmap contents, never its handle, so this is a reliable
    // identity for "is this the active cursor").
    const void* loadSlotHandle(int ocrId) const;

    // Handle currently shown by the foreground app, or nullptr on failure.
    const void* activeCursorHandle() const;

    // Primary monitor refresh rate in Hz (falls back to `def` if unknown).
    static int refreshRateHz(int def = 60);

    // Current mouse position in screen pixels.
    static void cursorPos(long& x, long& y);

    // Reload the user's normal cursor scheme. Safe to call repeatedly.
    static void restore();

private:
    void* dc_ = nullptr;       // HDC used to create the DIB
    void* hbmColor_ = nullptr; // HBITMAP: 32-bpp top-down DIB section
    void* hbmMask_ = nullptr;  // HBITMAP: monochrome AND mask (all zero)
    void* bits_ = nullptr;     // raw pixel pointer into hbmColor_
    int   sizeBytes_ = 0;      // CANVAS*CANVAS*4
};
