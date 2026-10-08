#include "keybinds.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace drizzy;

namespace demo {

namespace {

bool IsModifier(uint8_t vk) {
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

// Keys a binding may use: anything but the modifiers and the left / right / middle mouse buttons.
bool IsBindable(uint8_t vk) {
    return vk != 0 && vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_MBUTTON && vk != VK_CANCEL &&
           vk != VK_PROCESSKEY && vk != VK_PACKET && !IsModifier(vk);
}

void AppendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    }
}

std::string KeyName(uint8_t vk) {
    switch (vk) {
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "Caps Lock";
    case VK_ESCAPE: return "Esc";
    case VK_SPACE: return "Space";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "Left";
    case VK_UP: return "Up";
    case VK_RIGHT: return "Right";
    case VK_DOWN: return "Down";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_APPS: return "Menu";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DECIMAL: return "Num .";
    case VK_DIVIDE: return "Num /";
    case VK_NUMLOCK: return "Num Lock";
    case VK_SCROLL: return "Scroll Lock";
    default: break;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    // Punctuation and other layout-dependent keys: the character the key types on the current keyboard layout.
    const UINT c = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;  // the top bit marks dead keys
    std::string name;
    if (c > 0x20 && c != 0x7F && c < 0x800) {
        AppendUtf8(name, c);
        return name;
    }
    char hex[16];
    std::snprintf(hex, sizeof(hex), "Key 0x%02X", vk);
    return hex;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// "0x47 ctrl alt" or "none".
bool ParseCombo(std::string_view text, KeyCombo& combo) {
    combo = {};
    bool first = true;
    while (!(text = Trim(text)).empty()) {
        const size_t end = std::min(text.find(' '), text.size());
        const std::string token(text.substr(0, end));
        text.remove_prefix(end);
        if (first) {
            first = false;
            if (token == "none") return Trim(text).empty();
            char* tail = nullptr;
            const unsigned long vk = std::strtoul(token.c_str(), &tail, 16);
            if (*tail != '\0' || vk > 0xFF || !IsBindable(uint8_t(vk))) return false;
            combo.vk = uint8_t(vk);
        } else if (token == "ctrl") {
            combo.ctrl = true;
        } else if (token == "shift") {
            combo.shift = true;
        } else if (token == "alt") {
            combo.alt = true;
        } else {
            return false;
        }
    }
    return !first;
}

// What the binding button shows while waiting for a key: the modifiers already held.
std::string CapturePrompt() {
    std::string held;
    if (GetKeyState(VK_CONTROL) < 0) held += "Ctrl+";
    if (GetKeyState(VK_SHIFT) < 0) held += "Shift+";
    if (GetKeyState(VK_MENU) < 0) held += "Alt+";
    return held.empty() ? "Press a key..." : held + "...";
}

} // namespace

std::string KeyComboName(KeyCombo combo) {
    if (!combo.Bound()) return "Unbound";
    std::string name;
    if (combo.ctrl) name += "Ctrl+";
    if (combo.shift) name += "Shift+";
    if (combo.alt) name += "Alt+";
    return name + KeyName(combo.vk);
}

void KeyBinds::Group(std::string title) { m_group = std::move(title); }

void KeyBinds::Add(std::string id, std::string label, KeyCombo defaultCombo, Command run, Condition enabled) {
    Action action;
    action.id = std::move(id);
    action.label = std::move(label);
    action.group = m_group;
    action.combo = action.defaultCombo = defaultCombo;
    action.run = std::move(run);
    action.enabled = std::move(enabled);
    m_actions.push_back(std::move(action));
}

bool KeyBinds::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, bool typing) {
    KeyCombo pressed;
    bool repeat = false;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:  // Alt combinations and F10
        if (wParam > 0xFF) return false;
        pressed.vk = uint8_t(wParam);
        repeat = (lParam & (LPARAM(1) << 30)) != 0;  // the key was already down: auto-repeat
        break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        pressed.vk = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    default:
        return false;  // key releases, and the left / right / middle mouse buttons, never bind or run anything
    }
    // GetKeyState is in step with the message being handled, so these are the modifiers held at the press.
    pressed.ctrl = GetKeyState(VK_CONTROL) < 0;
    pressed.shift = GetKeyState(VK_SHIFT) < 0;
    pressed.alt = GetKeyState(VK_MENU) < 0;
    return OnPress(pressed, repeat, typing);
}

bool KeyBinds::OnPress(KeyCombo pressed, bool repeat, bool typing) {
    if (pressed.vk == 0 || pressed.vk == VK_LBUTTON || pressed.vk == VK_RBUTTON || pressed.vk == VK_MBUTTON ||
        pressed.vk == VK_PROCESSKEY || pressed.vk == VK_PACKET) {
        return false;  // clicks, and keys an IME is composing with
    }
    if (m_capturing >= 0) {
        // Rebinding: every press is swallowed until the binding is set or cancelled. A modifier on its own waits for
        // the key it goes with; a key pressed with no modifier held is bound by itself.
        if (IsModifier(pressed.vk) || repeat) return true;
        if (pressed.vk == VK_ESCAPE && !pressed.ctrl && !pressed.shift && !pressed.alt) {
            m_capturing = -1;
            return true;
        }
        Assign(size_t(m_capturing), pressed, true);
        m_capturing = -1;
        return true;
    }
    if (repeat || typing || IsModifier(pressed.vk)) return false;
    for (Action& action : m_actions) {
        // Exact match: G does not run a Ctrl+G binding, and Ctrl+G does not run a G binding.
        if (action.combo != pressed || !action.Enabled()) continue;
        m_lastRun = action.label + " (" + KeyComboName(pressed) + ")";
        if (action.run) action.run();
        return true;
    }
    return false;
}

void KeyBinds::Assign(size_t index, KeyCombo combo, bool reportConflict) {
    if (reportConflict) m_notice.clear();
    if (combo.Bound()) {
        for (size_t i = 0; i < m_actions.size(); ++i) {
            if (i == index || m_actions[i].combo != combo) continue;
            m_actions[i].combo = {};
            if (reportConflict) {
                m_notice = KeyComboName(combo) + " was taken from \"" + m_actions[i].label + "\", which is now unbound.";
            }
        }
    }
    m_actions[index].combo = combo;
    m_changed = true;
}

void KeyBinds::ShowWindow(Ui& ui, bool* open) {
    ui.SetNextWindowPos({std::max(ui.GetDisplaySize().x - 600.0f, 20.0f), 40.0f}, Cond::FirstUseEver);
    ui.SetNextWindowSize({560.0f, 600.0f}, Cond::FirstUseEver);
    if (!ui.Begin("Keybinds", open)) {
        ui.End();
        if (open && !*open) m_capturing = -1;
        return;
    }
    ui.TextWrapped("Click a binding, then press a key or an extra mouse button (Mouse 4 / 5). Hold Ctrl, Shift or Alt "
                   "with it for a combination, or press the key alone to bind just that key. Esc or a click elsewhere "
                   "cancels.");
    ui.PushStyleColor(UiColor::Text, ui.GetColor(UiColor::TextDisabled));
    ui.TextWrapped("Bindings never run while you are typing in a text field. Left, right and middle clicks can't be "
                   "bound.");
    ui.PopStyleColor();

    // Labels in one column, bindings lined up in the next.
    float labelWidth = 0.0f;
    for (const Action& action : m_actions) labelWidth = std::max(labelWidth, ui.CalcTextSize(action.label).x);
    const float bindX = labelWidth + ui.Style().itemSpacing.x * 3.0f;
    bool captureHovered = false;
    for (size_t i = 0; i < m_actions.size(); ++i) {
        Action& action = m_actions[i];
        if (i == 0 || action.group != m_actions[i - 1].group) {
            ui.SeparatorText(action.group.empty() ? "Commands" : action.group);
        }
        ui.PushID(int(i));
        ui.AlignTextToFramePadding();
        if (action.Enabled()) {
            ui.Text(action.label);
        } else {
            ui.TextDisabled(action.label);
            if (ui.IsItemHovered()) ui.SetTooltip("Not available in this scene: the binding is kept, its key passes through.");
        }
        ui.SameLine(bindX);
        const bool capturing = m_capturing == int(i);
        if (capturing) {
            ui.PushStyleColor(UiColor::Button, ui.GetColor(UiColor::ButtonActive));
            ui.PushStyleColor(UiColor::ButtonHovered, ui.GetColor(UiColor::ButtonActive));
        }
        // "###bind" keeps the button's ID stable while its text changes.
        const std::string text = (capturing ? CapturePrompt() : KeyComboName(action.combo)) + "###bind";
        if (ui.Button(text, {150.0f, 0.0f})) {
            m_capturing = capturing ? -1 : int(i);  // clicking the waiting button again cancels
            m_notice.clear();
        }
        if (capturing) {
            captureHovered = ui.IsItemHovered();
            ui.PopStyleColor(2);
        }
        ui.SameLine();
        ui.BeginDisabled(!action.combo.Bound());
        if (ui.Button("Clear")) {
            Assign(i, {}, true);
            m_capturing = -1;
        }
        ui.EndDisabled();
        ui.SameLine();
        ui.BeginDisabled(action.combo == action.defaultCombo);
        if (ui.Button("Default")) {
            Assign(i, action.defaultCombo, true);
            m_capturing = -1;
        }
        ui.EndDisabled();
        ui.PopID();
    }
    // A click anywhere but the waiting button cancels (a click on another binding then starts rebinding that one).
    if (m_capturing >= 0 && ui.IsMouseClicked(MouseButton::Left) && !captureHovered) m_capturing = -1;

    ui.Spacing();
    ui.Separator();
    if (!m_notice.empty()) {
        ui.PushStyleColor(UiColor::Text, Hex(0xFFB35C));
        ui.TextWrapped(m_notice);
        ui.PopStyleColor();
    }
    ui.TextDisabled(m_lastRun.empty() ? std::string("Last command: none yet") : "Last command: " + m_lastRun);
    if (ui.Button("Reset all to defaults")) ResetToDefaults();
    ui.End();
    if (open && !*open) m_capturing = -1;
}

const KeyCombo* KeyBinds::Find(std::string_view id) const {
    for (const Action& action : m_actions) {
        if (action.id == id) return &action.combo;
    }
    return nullptr;
}

void KeyBinds::ResetToDefaults() {
    for (Action& action : m_actions) action.combo = action.defaultCombo;
    m_capturing = -1;
    m_notice.clear();
    m_changed = true;
}

std::string KeyBinds::Save() const {
    std::string out = "# id = virtual-key code (hex) [ctrl] [shift] [alt], or none\n";
    for (const Action& action : m_actions) {
        out += action.id + " = ";
        if (!action.combo.Bound()) {
            out += "none\n";
            continue;
        }
        char vk[8];
        std::snprintf(vk, sizeof(vk), "0x%02X", action.combo.vk);
        out += vk;
        if (action.combo.ctrl) out += " ctrl";
        if (action.combo.shift) out += " shift";
        if (action.combo.alt) out += " alt";
        out += '\n';
    }
    return out;
}

void KeyBinds::Load(std::string_view text) {
    while (!text.empty()) {
        const size_t eol = std::min(text.find('\n'), text.size());
        const std::string_view line = Trim(text.substr(0, eol));
        text.remove_prefix(std::min(eol + 1, text.size()));
        const size_t eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) continue;
        const std::string_view id = Trim(line.substr(0, eq));
        KeyCombo combo;
        if (!ParseCombo(line.substr(eq + 1), combo)) continue;
        for (size_t i = 0; i < m_actions.size(); ++i) {
            if (m_actions[i].id == id) Assign(i, combo, false);
        }
    }
    m_changed = false;  // what was just loaded is already saved
}

bool KeyBinds::TakeChanged() {
    const bool changed = m_changed;
    m_changed = false;
    return changed;
}

} // namespace demo
