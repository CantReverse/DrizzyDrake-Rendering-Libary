// Shared sample code: named commands bound to keys or the extra mouse buttons, rebindable at runtime from a window.
// Win32 (it reads virtual-key codes and window messages), so it drops straight into an injected overlay's WndProc hook
// as well as the sandbox.
//
//   demo::KeyBinds binds;
//   binds.Group("Debug");
//   binds.Add("god_mode", "Toggle god mode", {'G', true}, [] { god = !god; });  // Ctrl+G
//   // in the WndProc (hook), before the UI or the game sees the message:
//   if (binds.OnMessage(msg, wParam, lParam, ui.Output().wantCaptureKeyboard)) return 0;
//   // in the UI frame:
//   if (showBinds) binds.ShowWindow(ui, &showBinds);
#pragma once

#include "drizzy/ui.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

// A key or an extra mouse button, plus the modifiers held with it. `vk` is a Win32 virtual-key code; the extra mouse
// buttons are VK_XBUTTON1 / VK_XBUTTON2 ("Mouse 4" / "Mouse 5"). The left, right and middle buttons are never bound.
struct KeyCombo {
    uint8_t vk = 0;  // 0 = unbound
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    bool Bound() const { return vk != 0; }
    bool operator==(const KeyCombo&) const = default;
};

std::string KeyComboName(KeyCombo combo);  // "Ctrl+Shift+F5", "Mouse 4", "Unbound"

class KeyBinds {
public:
    using Command = std::function<void()>;
    using Condition = std::function<bool()>;

    // Commands added after this are listed under `title` in the window.
    void Group(std::string title);
    // `id` names the command in Save/Load; `enabled` (optional) limits it to where it makes sense - a disabled
    // command keeps its binding but does not run, and its key passes through.
    void Add(std::string id, std::string label, KeyCombo defaultCombo, Command run, Condition enabled = {});

    // Feed every window message. Returns true when it was a key / extra mouse button press that the binds used - a
    // command ran, or the press was captured as a new binding - so the host should not pass it on. Commands never run
    // while `typing` (a text field has keyboard focus), on key auto-repeat, or while a binding is being captured.
    bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, bool typing);
    // The same, for a press already decoded (OnMessage reads the modifiers with GetKeyState and calls this).
    bool OnPress(KeyCombo pressed, bool repeat, bool typing);

    // The rebinding window: click a binding, then press the new key or extra mouse button. Ctrl / Shift / Alt held
    // at that moment become part of the binding; pressed alone, the key is bound by itself. Esc or a click elsewhere
    // cancels. Binding a combo that another command uses takes it from that command.
    void ShowWindow(drizzy::Ui& ui, bool* open);
    bool Capturing() const { return m_capturing >= 0; }
    void CancelCapture() { m_capturing = -1; }

    const KeyCombo* Find(std::string_view id) const;
    void ResetToDefaults();
    // Text, one "id = vk [ctrl] [shift] [alt]" line per command (vk in hex, or "none"). Load skips unknown ids and
    // malformed lines; commands missing from the text keep their current binding.
    std::string Save() const;
    void Load(std::string_view text);
    // True once after the user changes a binding (rebound, cleared or reset), so the host knows to save them.
    bool TakeChanged();

private:
    struct Action {
        std::string id, label, group;
        KeyCombo combo, defaultCombo;
        Command run;
        Condition enabled;
        bool Enabled() const { return !enabled || enabled(); }
    };
    // Binds `combo` to action `index`, unbinding any other action that had it.
    void Assign(size_t index, KeyCombo combo, bool reportConflict);

    std::vector<Action> m_actions;
    std::string m_group;
    int m_capturing = -1;    // action being rebound, or -1
    std::string m_notice;    // what the last rebinding took from another command
    std::string m_lastRun;   // "label (combo)" of the last command run, shown in the window
    bool m_changed = false;
};

} // namespace demo
