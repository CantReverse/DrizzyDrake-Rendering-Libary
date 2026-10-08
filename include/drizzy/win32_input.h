// drizzy_renderer - Win32 input translation (optional, header-only).
//
// For hosts that receive Win32 window messages - an injected overlay that hooks the game's WndProc, or any Win32 app.
// Translates messages into UiInput and reports whether the UI wants each message, so the host can decide to hide it
// from the game while a menu is open. The library itself never reads input or calls the OS; this is a convenience.
//
//   drizzy::Win32Input input;
//   // in your WndProc hook, before passing the message to the game:
//   if (menuVisible && input.ProcessMessage(ui, hwnd, msg, wParam, lParam)) return true;  // UI consumed it
//   // each frame, before ui.NewFrame():
//   input.NewFrame(ui, hwnd);
#pragma once

#include "drizzy/ui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

namespace drizzy {

class Win32Input {
public:
    // Refreshes the display size from the window's client rect, the modifier keys, and - when the window is in the
    // foreground - the mouse position (so the first frame a menu opens already has a valid cursor). Call before
    // Ui::NewFrame().
    void NewFrame(Ui& ui, HWND hwnd) {
        UiInput& in = ui.Input();
        RECT client;
        if (GetClientRect(hwnd, &client)) {
            in.displaySize = {float(client.right - client.left), float(client.bottom - client.top)};
        }
        in.keyCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        in.keyShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        in.keyAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (GetForegroundWindow() == hwnd && !m_haveMouseFromMessage) {
            POINT p;
            if (GetCursorPos(&p) && ScreenToClient(hwnd, &p)) in.mousePos = {float(p.x), float(p.y)};
        }
        m_haveMouseFromMessage = false;
    }

    // Translates one window message. Returns true when the message is mouse or keyboard input that the UI currently
    // wants - the host should then not forward it to the game. Never swallows non-input messages.
    bool ProcessMessage(Ui& ui, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        UiInput& in = ui.Input();
        const UiOutput& out = ui.Output();
        switch (msg) {
        case WM_MOUSEMOVE:
            in.mousePos = {float(GET_X_LPARAM(lParam)), float(GET_Y_LPARAM(lParam))};
            m_haveMouseFromMessage = true;
            if (!m_tracking) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                m_tracking = true;
            }
            return out.wantCaptureMouse;
        case WM_MOUSELEAVE:
            in.mousePos = {-1e30f, -1e30f};
            m_tracking = false;
            return false;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: {
            const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? 0
                             : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? 1 : 2;
            const bool wants = out.wantCaptureMouse;
            in.mouseDown[button] = true;
            if (!AnyMouseDownExcept(in, -1)) {}  // keep capture logic simple
            if (GetCapture() != hwnd) SetCapture(hwnd);
            return wants;
        }
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
            const int button = msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2;
            in.mouseDown[button] = false;
            if (!AnyMouseDownExcept(in, -1) && GetCapture() == hwnd) ReleaseCapture();
            return out.wantCaptureMouse;
        }
        case WM_MOUSEWHEEL:
            in.mouseWheel += float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
            return out.wantCaptureMouse;
        case WM_MOUSEHWHEEL:
            in.mouseWheelH += float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
            return out.wantCaptureMouse;
        case WM_CHAR:
            AddChar(in, wchar_t(wParam));
            return out.wantCaptureKeyboard;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
        case WM_KEYUP: case WM_SYSKEYUP: {
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            Key key;
            if (MapKey(wParam, key)) in.SetKey(key, down);
            if (wParam == VK_CONTROL) in.keyCtrl = down;
            if (wParam == VK_SHIFT) in.keyShift = down;
            if (wParam == VK_MENU) in.keyAlt = down;
            return out.wantCaptureKeyboard;
        }
        default:
            return false;
        }
    }

    // Returns the cursor to show, or null to leave it unchanged. For use inside a WM_SETCURSOR hook:
    //   if (menuVisible && LOWORD(lParam) == HTCLIENT) { if (HCURSOR c = input.Cursor(ui)) { SetCursor(c); return 1; } }
    static HCURSOR Cursor(const Ui& ui) {
        LPCWSTR id = IDC_ARROW;
        switch (ui.Output().cursor) {
        case MouseCursor::TextInput: id = IDC_IBEAM; break;
        case MouseCursor::Hand: id = IDC_HAND; break;
        case MouseCursor::ResizeAll: id = IDC_SIZEALL; break;
        case MouseCursor::ResizeNS: id = IDC_SIZENS; break;
        case MouseCursor::ResizeEW: id = IDC_SIZEWE; break;
        case MouseCursor::ResizeNWSE: id = IDC_SIZENWSE; break;
        case MouseCursor::ResizeNESW: id = IDC_SIZENESW; break;
        default: break;
        }
        return LoadCursorW(nullptr, id);
    }

private:
    static bool AnyMouseDownExcept(const UiInput& in, int except) {
        for (int b = 0; b < 3; ++b) {
            if (b != except && in.mouseDown[b]) return true;
        }
        return false;
    }

    void AddChar(UiInput& in, wchar_t c) {
        if (c >= 0xD800 && c < 0xDC00) {
            m_highSurrogate = c;
        } else if (c >= 0xDC00 && c < 0xE000) {
            if (m_highSurrogate) {
                in.AddCharacter(0x10000u + ((uint32_t(m_highSurrogate) - 0xD800u) << 10) + (uint32_t(c) - 0xDC00u));
                m_highSurrogate = 0;
            }
        } else if (c >= 0x20 || c == '\t') {
            in.AddCharacter(c);
        }
    }

    static bool MapKey(WPARAM vk, Key& key) {
        switch (vk) {
        case VK_TAB: key = Key::Tab; return true;
        case VK_LEFT: key = Key::LeftArrow; return true;
        case VK_RIGHT: key = Key::RightArrow; return true;
        case VK_UP: key = Key::UpArrow; return true;
        case VK_DOWN: key = Key::DownArrow; return true;
        case VK_PRIOR: key = Key::PageUp; return true;
        case VK_NEXT: key = Key::PageDown; return true;
        case VK_HOME: key = Key::Home; return true;
        case VK_END: key = Key::End; return true;
        case VK_INSERT: key = Key::Insert; return true;
        case VK_DELETE: key = Key::Delete; return true;
        case VK_BACK: key = Key::Backspace; return true;
        case VK_SPACE: key = Key::Space; return true;
        case VK_RETURN: key = Key::Enter; return true;
        case VK_ESCAPE: key = Key::Escape; return true;
        case 'A': key = Key::A; return true;
        case 'C': key = Key::C; return true;
        case 'V': key = Key::V; return true;
        case 'X': key = Key::X; return true;
        case 'Y': key = Key::Y; return true;
        case 'Z': key = Key::Z; return true;
        default: return false;
        }
    }

    bool m_tracking = false;
    bool m_haveMouseFromMessage = false;
    wchar_t m_highSurrogate = 0;
};

// Win32 clipboard backends for UiPlatform (optional): ui.Platform().getClipboardText = &drizzy::Win32GetClipboard; ...
inline const char* Win32GetClipboard(void*) {
    static std::string text;
    text.clear();
    if (!OpenClipboard(nullptr)) return nullptr;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle))) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 0) {
                text.resize(size_t(bytes));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
                text.resize(size_t(bytes - 1));
            }
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return text.c_str();
}

inline void Win32SetClipboard(void*, const char* utf8) {
    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (chars <= 0) return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size_t(chars) * sizeof(wchar_t));
    if (!memory) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, static_cast<wchar_t*>(GlobalLock(memory)), chars);
    GlobalUnlock(memory);
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
    } else {
        GlobalFree(memory);
    }
}

} // namespace drizzy
