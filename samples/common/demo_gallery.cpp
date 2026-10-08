#include "demo_gallery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace drizzy;

namespace demo {

GallerySettings& Gallery() {
    static GallerySettings settings;
    return settings;
}

namespace {

const char* const kFrameShapes[] = {"Filled", "Outline", "Underline"};
const char* const kButtonShapes[] = {"Filled", "Outline"};
const char* const kSliderShapes[] = {"Block", "Rail", "Fill", "Segments"};
const char* const kCheckShapes[] = {"Check mark", "Filled box", "Square"};
const char* const kToggleShapes[] = {"Pill", "Thin (Material)", "Square"};
const char* const kTabShapes[] = {"Tabs", "Underline", "Pills", "Boxes"};
const char* const kTitleShapes[] = {"Bar", "Accent stripe", "Plain", "Solid accent"};
const char* const kLookDescriptions[] = {
    "Filled frames, block sliders and tabs: the Dear ImGui look.",
    "Large radii, rail sliders with shadowed knobs, filled checkboxes, pill tabs.",
    "Outlined widgets that glow when on or focused, underlined tabs.",
    "Underlined text fields, thin Material toggles, no chrome.",
    "Square and outlined, segmented sliders, a solid title bar, hard shadows.",
    "Translucent gradients with a light top edge, filling sliders, pill tabs.",
};
static_assert(sizeof(kLookDescriptions) / sizeof(kLookDescriptions[0]) == size_t(UiLook::Count), "one per look");

void SetTheme(Ui& ui, UiTheme theme) {
    GallerySettings& s = Gallery();
    s.theme = theme;
    s.palette = UiStyle::ThemePalette(theme);
    ui.Style().ApplyTheme(theme);
}

void SetLook(Ui& ui, UiLook look) {
    Gallery().look = look;
    ui.Style().ApplyLook(look);
}

void SetFont(Ui& ui, int index) {
    GallerySettings& s = Gallery();
    if (index < 0 || index >= int(s.fonts.size())) return;
    s.font = index;
    ui.Style().font = s.fonts[size_t(index)].font;
    ui.Style().fontSize = s.fonts[size_t(index)].size;
}

BackdropColors ThemeBackdropColors(const Ui& ui) {
    BackdropColors c;
    c.primary = ui.GetColor(UiColor::Text);
    c.accent = ui.GetColor(UiColor::CheckMark);
    c.fill = colors::Transparent;
    return c;
}

// The gallery's animated window background. Called right after Begin, so it draws under every widget, clipped to the
// content area (below the title and menu bars, beside the scrollbar).
void WindowBackdrop(Ui& ui, Backdrop& backdrop, const BackdropColors& colors) {
    DrawList& dl = ui.WindowDrawList();
    backdrop.Draw(dl, dl.ClipRect(), ui.Input().deltaTime, ui.Style().font, colors,
                  ui.IsWindowHovered() ? ui.GetMousePos() : Vec2(-1e30f, -1e30f));
}

template <typename E>
bool EnumCombo(Ui& ui, const char* label, E* value, const char* const* names, int count) {
    int v = int(*value);
    if (!ui.Combo(label, &v, names, count)) return false;
    *value = E(v);
    return true;
}

void BasicsTab(Ui& ui, TextureId image) {
    ui.SeparatorText("Text");
    ui.Text("Plain text. Labels are also IDs: \"Save##a\" and \"Save##b\" differ.");
    ui.TextColored(Hex(0x8AE05A), "TextColored");
    ui.SameLine();
    ui.TextDisabled("TextDisabled");
    static float t = 0.0f;
    t += ui.Input().deltaTime;
    ui.TextF("TextF: formatted, time = %.1fs", double(t));
    ui.BulletText("BulletText item one");
    ui.BulletText("BulletText item two");
    ui.LabelText("Resolution", "1920 x 1080");
    ui.TextWrapped("TextWrapped: this paragraph wraps to the width of the window, breaking on word boundaries so a "
                   "long sentence stays readable no matter how narrow the gallery gets.");

    ui.SeparatorText("Buttons");
    static int clicks = 0;
    if (ui.Button("Button")) ++clicks;
    ui.SameLine();
    if (ui.SmallButton("SmallButton")) ++clicks;
    ui.SameLine();
    ui.Button("Wide", {120.0f, 0.0f});
    ui.SameLine();
    ui.TextF("clicked %d", clicks);
    if (image != kNoTexture) {
        ui.Image(image, {40.0f, 40.0f}, {0, 0}, {1, 1}, colors::White, 6.0f);
        ui.SameLine();
        ui.ImageButton("imgbtn", image, {40.0f, 40.0f});
        ui.SameLine();
        ui.AlignTextToFramePadding();
        ui.Text("Image / ImageButton");
    }

    ui.SeparatorText("Toggles");
    static bool check = true, toggle = false;
    ui.Checkbox("Checkbox", &check);
    ui.SameLine();
    ui.Checkbox("Another", &toggle);
    static int radio = 0;
    ui.RadioButton("Low", &radio, 0);
    ui.SameLine();
    ui.RadioButton("Medium", &radio, 1);
    ui.SameLine();
    ui.RadioButton("High", &radio, 2);
    static float progress = 0.0f;
    progress += ui.Input().deltaTime * 0.25f;
    if (progress > 1.0f) progress -= 1.0f;
    ui.ProgressBar(progress);
    ui.Bullet();
    ui.Text("Bullet + text on one line");

    ui.SeparatorText("Sliders & drags");
    static float f = 0.5f;
    ui.SliderFloat("SliderFloat", &f, 0.0f, 1.0f);
    static int n = 4;
    ui.SliderInt("SliderInt", &n, 0, 10);
    static float drag = 50.0f;
    ui.DragFloat("DragFloat", &drag, 0.5f, 0.0f, 100.0f, "%.1f");
    if (ui.IsItemHovered()) ui.SetTooltip("Drag horizontally. Shift = x10, Alt = x0.1");
    static int dragi = 3;
    ui.DragInt("DragInt", &dragi, 0.2f, 0, 100);
}

void InputTab(Ui& ui) {
    ui.SeparatorText("Text input");
    static char name[32] = "drizzy";
    ui.InputText("InputText", name, sizeof(name));
    static char search[64] = "";
    ui.InputTextWithHint("With hint", "search...", search, sizeof(search));
    static char secret[32] = "hunter2";
    ui.InputText("Password", secret, sizeof(secret), InputTextFlags::Password);
    static char readonly[32] = "read only";
    ui.InputText("ReadOnly", readonly, sizeof(readonly), InputTextFlags::ReadOnly);
    static int integer = 42;
    ui.InputInt("InputInt", &integer);
    static float number = 3.14159f;
    ui.InputFloat("InputFloat", &number, "%.4f");
    ui.SeparatorText("Multi-line");
    static char script[512] = "-- multi-line editor\nfor i = 1, 10 do\n    spawn(\"enemy\", i)\nend";
    ui.InputTextMultiline("##script", script, sizeof(script), {-1.0f, 110.0f});
    ui.TextDisabled("Arrows/Home/End, Enter for newline, Ctrl+A/C/V/X, Ctrl+arrows by word.");
}

void ColorTab(Ui& ui) {
    ui.SeparatorText("Color buttons");
    ui.TextDisabled("Click a swatch to pick its color in a popup; hover it for its values.");
    static Color primary = Hex(0xF28C28), secondary = Hex(0x00BBF9), highlight = Rgba(241, 91, 181, 170);
    ui.ColorEdit4("Primary##button", &primary, ColorEditFlags::NoInputs);
    ui.SameLine(0.0f, 24.0f);
    ui.ColorEdit4("Secondary##button", &secondary, ColorEditFlags::NoInputs);
    ui.SameLine(0.0f, 24.0f);
    ui.ColorEdit4("Highlight (translucent)##button", &highlight, ColorEditFlags::NoInputs);

    ui.SeparatorText("With inputs");
    static Color fill = Hex(0xF28C28);
    static Color tint = Rgba(90, 200, 255, 180);
    static Color rgb = Hex(0x8AC926);
    ui.ColorEdit4("ColorEdit4 (RGBA)", &fill);
    ui.ColorEdit4("With alpha", &tint);
    ui.ColorEdit3("ColorEdit3 (RGB)", &rgb);
    ui.ColorEdit4("Hex input", &fill, ColorEditFlags::DisplayHex);

    ui.SeparatorText("Swatches");
    static const Color swatches[] = {Hex(0xF15BB5), Hex(0xFEE440), Hex(0x00BBF9), Hex(0x00F5D4), Hex(0x9B5DE5),
                                     Hex(0xFF6B35), Hex(0x8AC926), Rgba(255, 255, 255, 120)};
    for (int i = 0; i < int(sizeof(swatches) / sizeof(swatches[0])); ++i) {
        if (i > 0) ui.SameLine();
        ui.PushID(i);
        if (ui.ColorButton("##swatch", swatches[i])) primary = swatches[i];
        ui.PopID();
    }
    ui.SameLine();
    ui.AlignTextToFramePadding();
    ui.TextDisabled("click one to make it Primary");

    if (ui.CollapsingHeader("Inline picker (ColorPicker4)")) {
        static Color picked = Hex(0x9B5DE5);
        static const Color original = Hex(0x9B5DE5);
        ui.SetNextItemWidth(200.0f);
        ui.ColorPicker4("##inline", &picked, ColorEditFlags::None, &original);
    }
}

void ListsTab(Ui& ui) {
    ui.SeparatorText("Combo");
    static int current = 1;
    static const char* const items[] = {"Forest", "Desert", "Snow", "Volcano", "Ocean", "Space"};
    ui.Combo("Combo", &current, items, int(sizeof(items) / sizeof(items[0])));

    static int custom = 0;
    if (ui.BeginCombo("BeginCombo", items[custom])) {
        for (int i = 0; i < 6; ++i) {
            if (ui.Selectable(items[i], i == custom)) custom = i;
        }
        ui.EndCombo();
    }

    ui.SeparatorText("Selectable & list box");
    static int selected = 2;
    for (int i = 0; i < 4; ++i) {
        char label[32];
        std::snprintf(label, sizeof(label), "Selectable %d", i);
        if (ui.Selectable(label, selected == i)) selected = i;
    }
    static int listSel = 0;
    if (ui.BeginListBox("ListBox", {-1.0f, 0.0f})) {
        for (int i = 0; i < 8; ++i) {
            char label[32];
            std::snprintf(label, sizeof(label), "Item %d", i);
            if (ui.Selectable(label, listSel == i)) listSel = i;
        }
        ui.EndListBox();
    }
}

void TreesTab(Ui& ui) {
    ui.SeparatorText("Trees");
    if (ui.TreeNode("Root node")) {
        ui.Text("A child item");
        if (ui.TreeNode("Nested node")) {
            ui.Text("Deeper item");
            if (ui.TreeNode("Leaf", TreeNodeFlags::Leaf)) ui.TreePop();
            ui.TreePop();
        }
        ui.TreePop();
    }
    ui.SeparatorText("Collapsing headers");
    if (ui.CollapsingHeader("Open by default", TreeNodeFlags::DefaultOpen)) {
        ui.Text("Content under a collapsing header.");
    }
    if (ui.CollapsingHeader("Click to expand")) {
        ui.Text("More content.");
    }
    ui.SeparatorText("Tab bar");
    if (ui.BeginTabBar("inner_tabs")) {
        if (ui.BeginTabItem("One")) {
            ui.Text("First inner tab");
            ui.EndTabItem();
        }
        if (ui.BeginTabItem("Two")) {
            ui.Text("Second inner tab");
            ui.EndTabItem();
        }
        if (ui.BeginTabItem("Three")) {
            ui.Text("Third inner tab");
            ui.EndTabItem();
        }
        ui.EndTabBar();
    }
}

void PopupsTab(Ui& ui) {
    ui.SeparatorText("Popups & menus");
    if (ui.Button("Open popup")) ui.OpenPopup("demo_popup");
    if (ui.BeginPopup("demo_popup")) {
        ui.Text("Popup content");
        ui.Separator();
        if (ui.MenuItem("Copy", "Ctrl+C")) {}
        if (ui.MenuItem("Paste", "Ctrl+V")) {}
        static bool flag = false;
        if (ui.MenuItem("Toggle me", {}, flag)) flag = !flag;
        ui.EndPopup();
    }
    ui.SameLine();
    ui.Button("Right-click me");
    if (ui.BeginPopupContextItem("ctx")) {
        if (ui.MenuItem("Action one")) {}
        if (ui.MenuItem("Action two")) {}
        ui.EndPopup();
    }
    ui.SameLine();
    ui.TextDisabled("(context menu)");

    static bool confirmOpen = false;
    if (ui.Button("Open modal")) {
        confirmOpen = true;
        ui.OpenPopup("Confirm");
    }
    if (ui.BeginPopupModal("Confirm", &confirmOpen)) {
        ui.Text("A modal blocks everything behind it.");
        if (ui.Button("OK", {100.0f, 0.0f})) ui.CloseCurrentPopup();
        ui.SameLine();
        if (ui.Button("Cancel", {100.0f, 0.0f})) ui.CloseCurrentPopup();
        ui.EndPopup();
    }

    ui.SeparatorText("Tooltip");
    ui.Text("Hover me");
    if (ui.IsItemHovered()) {
        ui.BeginTooltip();
        ui.Text("A tooltip built with BeginTooltip/EndTooltip.");
        ui.TextDisabled("Any widgets can go inside.");
        ui.EndTooltip();
    }
}

void PlotsTab(Ui& ui) {
    static float values[90] = {};
    static int offset = 0;
    static float accum = 0.0f;
    accum += ui.Input().deltaTime;
    while (accum > 1.0f / 60.0f) {
        accum -= 1.0f / 60.0f;
        values[offset] = std::sin(float(offset) * 0.3f) * 0.5f + 0.5f +
                         0.2f * std::sin(float(offset) * 0.11f + float(ui.Time()));
        offset = (offset + 1) % 90;
    }
    ui.SeparatorText("Plots");
    ui.SetNextItemWidth(-1.0f);
    ui.PlotLines("##lines", values, 90, offset, "PlotLines", 0.0f, 1.2f, {0.0f, 70.0f});
    ui.SetNextItemWidth(-1.0f);
    ui.PlotHistogram("##hist", values, 90, offset, "PlotHistogram", 0.0f, 1.2f, {0.0f, 70.0f});
}

void LayoutTab(Ui& ui) {
    ui.SeparatorText("Layout");
    ui.Text("SameLine:");
    ui.SameLine();
    ui.Button("A");
    ui.SameLine();
    ui.Button("B");
    ui.SameLine();
    ui.Button("C");
    
    ui.Text("Groups (treated as one item):");
    ui.BeginGroup();
    ui.Button("Left top");
    ui.Button("Left bottom");
    ui.EndGroup();
    ui.SameLine();
    ui.BeginGroup();
    ui.Text("A group to the right");
    static float demoValue = 0.6f;
    ui.SliderFloat("##g", &demoValue, 0.0f, 1.0f, "%.2f");
    ui.EndGroup();

    ui.Text("Indent / Unindent:");
    ui.Indent();
    ui.Text("Indented once");
    ui.Indent();
    ui.Text("Indented twice");
    ui.Unindent();
    ui.Unindent();

    ui.Separator();
    ui.Text("Spacing / Dummy below:");
    ui.Dummy({0.0f, 20.0f});
    ui.Text("...after a 20px gap.");

    ui.SeparatorText("Child region");
    if (ui.BeginChild("scroll", {0.0f, 100.0f}, true)) {
        for (int i = 0; i < 40; ++i) ui.TextF("Scrolling row %d", i);
    }
    ui.EndChild();
}

// A procedurally generated inventory for the table demos.
struct InventoryItem {
    char name[32];
    int type;
    int level;
    float price;
};

const char* const kItemTypes[] = {"Weapon", "Armor", "Potion", "Material", "Trinket"};

std::vector<InventoryItem>& Inventory() {
    static std::vector<InventoryItem> items = [] {
        static const char* const kAdjectives[] = {"Iron", "Rusty", "Ancient", "Glowing", "Cursed", "Silver", "Heavy",
                                                  "Swift"};
        static const char* const kNouns[] = {"Sword", "Shield", "Elixir", "Ingot", "Amulet", "Axe", "Helm", "Tonic",
                                             "Gem", "Bow"};
        std::vector<InventoryItem> list(10000);
        uint32_t rng = 1234567u;
        auto next = [&] {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };
        for (size_t i = 0; i < list.size(); ++i) {
            InventoryItem& item = list[i];
            std::snprintf(item.name, sizeof(item.name), "%s %s #%04zu", kAdjectives[next() % 8], kNouns[next() % 10], i);
            item.type = int(next() % 5);
            item.level = int(next() % 60) + 1;
            item.price = float(next() % 100000) / 100.0f;
        }
        return list;
    }();
    return items;
}

void SortItems(std::vector<int>& order, const TableSortSpecs& spec) {
    const std::vector<InventoryItem>& items = Inventory();
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const InventoryItem& x = items[size_t(a)];
        const InventoryItem& y = items[size_t(b)];
        int c = 0;
        switch (spec.column) {
        case 0: c = std::strcmp(x.name, y.name); break;
        case 1: c = x.type - y.type; break;
        case 2: c = x.level - y.level; break;
        case 3: c = x.price < y.price ? -1 : (x.price > y.price ? 1 : 0); break;
        default: c = a - b; break;
        }
        if (c == 0) c = a - b;
        return spec.descending ? c > 0 : c < 0;
    });
}

void TablesTab(Ui& ui) {
    ui.SeparatorText("Sortable, resizable");
    static std::vector<int> small = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static int selected = -1;
    if (ui.BeginTable("small", 4, TableFlags::Resizable | TableFlags::Sortable | TableFlags::RowBg | TableFlags::Borders)) {
        ui.TableSetupColumn("Name", TableColumnFlags::DefaultSort);
        ui.TableSetupColumn("Type", TableColumnFlags::WidthFixed, 80.0f);
        ui.TableSetupColumn("Level", TableColumnFlags::WidthFixed, 60.0f);
        ui.TableSetupColumn("Price", TableColumnFlags::WidthFixed, 80.0f);
        ui.TableHeadersRow();
        if (TableSortSpecs* spec = ui.TableGetSortSpecs(); spec && spec->dirty) {
            SortItems(small, *spec);
            spec->dirty = false;
        }
        for (int index : small) {
            const InventoryItem& item = Inventory()[size_t(index)];
            ui.TableNextRow();
            ui.TableNextColumn();
            ui.PushID(index);
            if (ui.Selectable(item.name, selected == index, SelectableFlags::SpanAllColumns)) selected = index;
            ui.PopID();
            ui.TableNextColumn();
            ui.Text(kItemTypes[item.type]);
            ui.TableNextColumn();
            ui.TextF("%d", item.level);
            ui.TableNextColumn();
            ui.TextF("%.2f", double(item.price));
        }
        ui.EndTable();
    }

    ui.SeparatorText("10,000 rows: ListClipper + filter + frozen header");
    static TextFilter filter;
    static std::vector<int> rows;
    static bool dirty = true;
    static TableSortSpecs lastSort;
    if (filter.Draw(ui, "Filter##inventory")) dirty = true;
    ui.SameLine();
    ui.HelpMarker("Comma-separated terms; -term excludes. Try: sword,axe,-rusty");
    if (ui.BeginTable("big", 4, TableFlags::ScrollY | TableFlags::RowBg | TableFlags::BordersOuter |
                                    TableFlags::BordersInnerV | TableFlags::Resizable | TableFlags::Sortable,
                      {0.0f, 220.0f})) {
        ui.TableSetupColumn("Name");
        ui.TableSetupColumn("Type", TableColumnFlags::WidthFixed, 80.0f);
        ui.TableSetupColumn("Level", TableColumnFlags::WidthFixed | TableColumnFlags::PreferSortDescending, 60.0f);
        ui.TableSetupColumn("Price", TableColumnFlags::WidthFixed, 80.0f);
        ui.TableHeadersRow();
        TableSortSpecs* spec = ui.TableGetSortSpecs();
        if (spec && spec->dirty) {
            lastSort = *spec;
            spec->dirty = false;
            dirty = true;
        }
        if (dirty) {
            // Filter and sort only when something changed; drawing just walks `rows`.
            rows.clear();
            const std::vector<InventoryItem>& items = Inventory();
            for (int i = 0; i < int(items.size()); ++i) {
                if (filter.PassFilter(items[size_t(i)].name)) rows.push_back(i);
            }
            if (lastSort.column >= 0) SortItems(rows, lastSort);
            dirty = false;
        }
        ListClipper clipper;
        clipper.Begin(ui, int(rows.size()));
        while (clipper.Step()) {
            for (int r = clipper.displayStart; r < clipper.displayEnd; ++r) {
                const InventoryItem& item = Inventory()[size_t(rows[size_t(r)])];
                ui.TableNextRow();
                ui.TableNextColumn();
                ui.Text(item.name);
                ui.TableNextColumn();
                ui.Text(kItemTypes[item.type]);
                ui.TableNextColumn();
                ui.TextF("%d", item.level);
                ui.TableNextColumn();
                ui.TextF("%.2f", double(item.price));
            }
        }
        ui.EndTable();
    }
    ui.TextDisabled("Only the visible rows are submitted; each column is one draw call.");
}

void ExtrasTab(Ui& ui) {
    ui.SeparatorText("Toggles");
    static bool god = true, ammo = false;
    ui.ToggleSwitch("God mode", &god);
    ui.SameLine();
    ui.ToggleSwitch("Infinite ammo", &ammo);

    ui.SeparatorText("Vectors & angles");
    static float position[3] = {12.0f, 0.5f, -40.0f};
    ui.SliderFloat3("Position", position, -100.0f, 100.0f, "%.1f");
    static int grid[2] = {16, 9};
    ui.DragInt2("Grid", grid, 0.2f, 1, 64);
    static float color[4] = {0.9f, 0.4f, 0.2f, 1.0f};
    ui.DragFloat4("RGBA", color, 0.01f, 0.0f, 1.0f, "%.2f");
    static float yaw = 0.7854f;
    ui.SliderAngle("Yaw", &yaw, -180.0f, 180.0f);

    ui.SeparatorText("Busy indicators");
    ui.Spinner("spin");
    ui.SameLine();
    ui.AlignTextToFramePadding();
    ui.Text("Loading assets...");
    ui.ProgressBar(-1.0f, {-1.0f, 0.0f}, "Indeterminate");

    ui.SeparatorText("Links, help, notifications");
    if (ui.TextLink("Show a notification")) ui.Notify("Hello from a TextLink.");
    ui.SameLine();
    ui.HelpMarker("TextLink is clickable text in the accent color.");
    if (ui.Button("Info")) ui.Notify("Mod loaded: 12 items registered.", NotifyType::Info);
    ui.SameLine();
    if (ui.Button("Success")) ui.Notify("Config saved.", NotifyType::Success);
    ui.SameLine();
    if (ui.Button("Warning")) ui.Notify("Low memory: textures were downscaled.", NotifyType::Warning);
    ui.SameLine();
    if (ui.Button("Error")) ui.Notify("Could not reach the server. Retrying in 5 seconds.", NotifyType::Error, 5.0f);
}

// A clickable card drawn in a theme's own colors: its background, a widget-colored bar, an accent dot and its name.
bool ThemeChip(Ui& ui, UiTheme theme, float width, bool selected) {
    const UiPalette p = UiStyle::ThemePalette(theme);
    const float height = std::floor(ui.GetFrameHeight() * 1.3f);
    ui.PushID(int(theme));
    const bool clicked = ui.InvisibleButton("##chip", {width, height});
    const bool hovered = ui.IsItemHovered();
    ui.PopID();
    const Rect r(ui.GetItemRectMin(), ui.GetItemRectMax());
    DrawList& dl = ui.WindowDrawList();
    const float rounding = std::min(ui.Style().frameRounding + 2.0f, height * 0.5f);
    RectStyle card;
    card.fill = WithAlpha(p.background, 255);
    card.radii = rounding;
    card.borderWidth = selected ? 2.0f : 1.0f;
    card.borderColor = selected ? ui.GetColor(UiColor::CheckMark) : hovered ? p.accent : WithAlpha(p.text, 60);
    dl.AddRectEx(r, card);
    const float cy = r.Center().y;
    const float dot = height * 0.18f;
    dl.AddCircleFilled({r.min.x + height * 0.42f, cy}, dot, p.accent);
    dl.AddRectFilled(Rect(r.max.x - height * 0.9f, cy - dot * 0.6f, r.max.x - height * 0.3f, cy + dot * 0.6f), p.surface,
                     dot * 0.6f);
    const Font& font = *ui.Style().font;
    const float size = ui.Style().fontSize;
    const char* name = UiStyle::ThemeName(theme);
    const Vec2 textSize = font.MeasureText(name, size);
    dl.AddTextClipped(font, size, {r.min.x + height * 0.75f, std::floor(cy - textSize.y * 0.5f)}, p.text, name,
                      Rect(r.min.x, r.min.y, r.max.x - height * 0.95f, r.max.y));
    return clicked;
}

void CustomizeTab(Ui& ui) {
    GallerySettings& s = Gallery();
    UiStyle& style = ui.Style();

    ui.SeparatorText("Theme");
    const int columns = 3;
    const float spacing = style.itemSpacing.x;
    const float chipW = std::floor((ui.GetContentRegionAvail().x - spacing * float(columns - 1)) / float(columns));
    for (int i = 0; i < int(UiTheme::Count); ++i) {
        if (i % columns) ui.SameLine();
        if (ThemeChip(ui, UiTheme(i), chipW, s.theme == UiTheme(i))) SetTheme(ui, UiTheme(i));
    }

    ui.SeparatorText("Style");
    int look = int(s.look);
    const char* lookNames[int(UiLook::Count)];
    for (int i = 0; i < int(UiLook::Count); ++i) lookNames[i] = UiStyle::LookName(UiLook(i));
    if (ui.Combo("##style", &look, lookNames, int(UiLook::Count))) SetLook(ui, UiLook(look));
    ui.TextDisabled(kLookDescriptions[size_t(s.look)]);

    if (!s.fonts.empty()) {
        ui.SeparatorText("Font");
        s.font = std::clamp(s.font, 0, int(s.fonts.size()) - 1);
        if (ui.BeginCombo("##font", s.fonts[size_t(s.font)].name)) {
            for (int i = 0; i < int(s.fonts.size()); ++i) {
                const DemoFont& f = s.fonts[size_t(i)];
                ui.PushFont(f.font, f.size);  // each entry in its own font
                ui.PushID(i);
                if (ui.Selectable(f.name, i == s.font)) SetFont(ui, i);
                ui.PopID();
                ui.PopFont();
            }
            ui.EndCombo();
        }
        ui.SliderFloat("Font size", &style.fontSize, 9.0f, 28.0f, "%.0f px");
    }

    ui.SeparatorText("Colors");
    ui.TextDisabled("Pick a few colors and every UI color is derived from them.");
    UiPalette& p = s.palette;
    bool changed = false;
    const uint32_t swatch = ColorEditFlags::NoInputs;
    changed |= ui.ColorEdit4("Accent", &p.accent, swatch | ColorEditFlags::NoAlpha);
    ui.SameLine(0.0f, 20.0f);
    changed |= ui.ColorEdit4("Background", &p.background, swatch);
    ui.SameLine(0.0f, 20.0f);
    changed |= ui.ColorEdit4("Widgets", &p.surface, swatch | ColorEditFlags::NoAlpha);
    ui.SameLine(0.0f, 20.0f);
    changed |= ui.ColorEdit4("Text", &p.text, swatch | ColorEditFlags::NoAlpha);
    changed |= ui.ColorEdit4("Text on accent", &p.accentText, swatch | ColorEditFlags::NoAlpha);
    ui.SameLine(0.0f, 20.0f);
    changed |= ui.ColorEdit4("Glow", &p.glow, swatch);
    ui.SameLine(0.0f, 20.0f);
    changed |= ui.ColorEdit4("Border", &p.border, swatch);
    ui.SameLine(0.0f, 20.0f);
    changed |= ui.ColorEdit4("Shadow", &p.shadow, swatch);
    if (changed) style.ApplyPalette(p);

    if (ui.CollapsingHeader("Every UI color")) {
        static TextFilter filter;
        filter.Draw(ui, "Filter##colors");
        if (ui.BeginChild("##colors", {0.0f, 230.0f}, true)) {
            const float half = std::floor(ui.GetContentRegionAvail().x * 0.5f);
            int shown = 0;
            for (int i = 0; i < int(UiColor::Count); ++i) {
                const char* name = UiStyle::ColorName(UiColor(i));
                if (!filter.PassFilter(name)) continue;
                if (shown++ % 2) ui.SameLine(half);
                ui.PushID(i);
                ui.ColorEdit4(name, &style.colors[i], ColorEditFlags::NoInputs);
                ui.PopID();
            }
        }
        ui.EndChild();
    }

    ui.SeparatorText("Animated background");
    Backdrop& b = s.backdrop;
    int kind = int(b.kind);
    const char* kindNames[int(BackdropKind::Count)];
    for (int i = 0; i < int(BackdropKind::Count); ++i) kindNames[i] = BackdropName(BackdropKind(i));
    if (ui.Combo("Background", &kind, kindNames, int(BackdropKind::Count))) b.kind = BackdropKind(kind);
    ui.SliderFloat("Speed", &b.speed, 0.0f, 3.0f, "%.2fx");
    ui.SliderFloat("Density", &b.density, 0.25f, 3.0f, "%.2fx");
    ui.SliderFloat("Opacity", &b.opacity, 0.0f, 1.0f, "%.2f");
    if (ui.Checkbox("Use the theme's colors", &s.backdropThemeColors) && !s.backdropThemeColors) {
        s.backdropColors = ThemeBackdropColors(ui);  // start from what is showing
    }
    if (!s.backdropThemeColors) {
        ui.ColorEdit4("Lines and points", &s.backdropColors.primary, swatch);
        ui.SameLine(0.0f, 20.0f);
        ui.ColorEdit4("Highlights", &s.backdropColors.accent, swatch);
        ui.SameLine(0.0f, 20.0f);
        ui.ColorEdit4("Fill", &s.backdropColors.fill, swatch);
    }

    if (ui.CollapsingHeader("Fine-tune the style")) {
        ui.TextDisabled("Mix and match: every part of a style can be changed on its own.");
        EnumCombo(ui, "Text fields", &style.frameShape, kFrameShapes, 3);
        EnumCombo(ui, "Buttons", &style.buttonShape, kButtonShapes, 2);
        EnumCombo(ui, "Sliders", &style.sliderShape, kSliderShapes, 4);
        EnumCombo(ui, "Checkboxes", &style.checkShape, kCheckShapes, 3);
        EnumCombo(ui, "Toggles", &style.toggleShape, kToggleShapes, 3);
        EnumCombo(ui, "Tabs", &style.tabShape, kTabShapes, 4);
        EnumCombo(ui, "Title bar", &style.titleShape, kTitleShapes, 4);
        ui.SliderFloat("Glow", &style.glowSize, 0.0f, 24.0f, "%.0f px");
        ui.SliderFloat("Gradient", &style.gradient, 0.0f, 1.0f, "%.2f");
        ui.SliderFloat("Bevel", &style.bevel, 0.0f, 1.0f, "%.2f");
        ui.SliderFloat("Knob shadow", &style.knobShadow, 0.0f, 1.0f, "%.2f");
        ui.DragFloat2("Hard shadow", &style.hardShadow.x, 0.1f, -12.0f, 12.0f, "%.0f");
        ui.SliderFloat("Window rounding", &style.windowRounding, 0.0f, 20.0f, "%.0f");
        ui.SliderFloat("Frame rounding", &style.frameRounding, 0.0f, 14.0f, "%.0f");
        ui.SliderFloat("Window border", &style.windowBorderSize, 0.0f, 3.0f, "%.0f");
        ui.SliderFloat("Frame border", &style.frameBorderSize, 0.0f, 2.0f, "%.0f");
        ui.SliderFloat("Window shadow", &style.windowShadowSize, 0.0f, 48.0f, "%.0f");
        ui.DragFloat2("Item spacing", &style.itemSpacing.x, 0.2f, 0.0f, 24.0f, "%.0f");
    }

    ui.Spacing();
    if (ui.Button("Copy theme as text")) {
        const UiPlatform& platform = ui.Platform();
        if (platform.setClipboardText) {
            platform.setClipboardText(platform.userData, style.SaveTheme().c_str());
            ui.Notify("Theme copied: paste it into a file and load it with UiStyle::LoadTheme.", NotifyType::Success);
        } else {
            ui.Notify("No clipboard in this host.", NotifyType::Warning);
        }
    }
    ui.SameLine();
    if (ui.Button("Reset")) {
        SetLook(ui, s.look);
        SetTheme(ui, s.theme);
        SetFont(ui, s.font);
    }
}

void ViewMenu(Ui& ui) {
    GallerySettings& s = Gallery();
    if (ui.BeginMenu("Theme")) {
        for (int i = 0; i < int(UiTheme::Count); ++i) {
            if (ui.MenuItem(UiStyle::ThemeName(UiTheme(i)), {}, s.theme == UiTheme(i))) SetTheme(ui, UiTheme(i));
        }
        ui.EndMenu();
    }
    if (ui.BeginMenu("Style")) {
        for (int i = 0; i < int(UiLook::Count); ++i) {
            if (ui.MenuItem(UiStyle::LookName(UiLook(i)), {}, s.look == UiLook(i))) SetLook(ui, UiLook(i));
        }
        ui.EndMenu();
    }
    if (!s.fonts.empty() && ui.BeginMenu("Font")) {
        for (int i = 0; i < int(s.fonts.size()); ++i) {
            ui.PushID(i);
            if (ui.MenuItem(s.fonts[size_t(i)].name, {}, s.font == i)) SetFont(ui, i);
            ui.PopID();
        }
        ui.EndMenu();
    }
    if (ui.BeginMenu("Background")) {
        for (int i = 0; i < int(BackdropKind::Count); ++i) {
            if (ui.MenuItem(BackdropName(BackdropKind(i)), {}, s.backdrop.kind == BackdropKind(i))) {
                s.backdrop.kind = BackdropKind(i);
            }
        }
        ui.EndMenu();
    }
}

// A compact mod menu for the showcase; `i` keeps each window's values apart.
void MiniMenu(Ui& ui, int i) {
    static bool god[9], noclip[9], ammo[9], esp[9];
    static float speed[9];
    static int jump[9], quality[9], weather[9];
    static char name[9][24];
    static Color tint[9];
    static bool initialized = false;
    if (!initialized) {
        for (int k = 0; k < 9; ++k) {
            god[k] = esp[k] = true;
            speed[k] = 6.5f;
            jump[k] = 40;
            quality[k] = 1;
            weather[k] = 2;
            std::snprintf(name[k], sizeof(name[k]), "Player_%d", k + 1);
            tint[k] = Hex(0x7AE7FF);
        }
        initialized = true;
    }
    static const char* const kWeather[] = {"Clear", "Rain", "Storm", "Snow", "Fog"};
    if (ui.BeginTabBar("tabs")) {
        if (ui.BeginTabItem("Player")) {
            ui.ToggleSwitch("God mode", &god[i]);
            ui.SameLine(0.0f, 18.0f);
            ui.ToggleSwitch("No clip", &noclip[i]);
            ui.Checkbox("Infinite ammo", &ammo[i]);
            ui.SameLine(0.0f, 18.0f);
            ui.Checkbox("ESP boxes", &esp[i]);
            ui.SliderFloat("Speed", &speed[i], 0.0f, 10.0f, "%.1f");
            ui.SliderInt("Jump", &jump[i], 0, 100);
            ui.RadioButton("Low", &quality[i], 0);
            ui.SameLine();
            ui.RadioButton("Medium", &quality[i], 1);
            ui.SameLine();
            ui.RadioButton("High", &quality[i], 2);
            ui.Combo("Weather", &weather[i], kWeather, 5);
            ui.InputText("Name", name[i], sizeof(name[i]));
            ui.ColorEdit4("Tint", &tint[i], ColorEditFlags::NoInputs);
            ui.SameLine(0.0f, 18.0f);
            ui.ProgressBar(0.62f, {-1.0f, 0.0f});
            ui.Button("Apply");
            ui.SameLine();
            ui.Button("Reset");
            ui.EndTabItem();
        }
        if (ui.BeginTabItem("World")) {
            ui.SliderFloat("Time of day", &speed[i], 0.0f, 24.0f, "%.1f h");
            ui.EndTabItem();
        }
        if (ui.BeginTabItem("Visuals")) {
            ui.Checkbox("Bloom", &esp[i]);
            ui.EndTabItem();
        }
        ui.EndTabBar();
    }
}

} // namespace

void ApplyGallerySettings(Ui& ui) {
    GallerySettings& s = Gallery();
    SetLook(ui, s.look);
    SetTheme(ui, s.theme);
    SetFont(ui, s.font);
}

void ShowWidgetGallery(Ui& ui, TextureId image, bool* open) {
    // On first use, wide enough for every tab label in the current font (fonts differ a lot in width).
    const UiStyle& style = ui.Style();
    const float tabsWidth = ui.CalcTextSize("BasicsInputColorListsTreesPopupsPlotsTablesExtrasLayoutCustomize").x +
                            11.0f * (style.framePadding.x * 2.0f + 2.0f) + style.windowPadding.x * 2.0f +
                            style.scrollbarSize + 8.0f;
    ui.SetNextWindowSize({std::max(660.0f, std::floor(tabsWidth)), 640.0f}, Cond::FirstUseEver);
    ui.SetNextWindowPos({40.0f, 40.0f}, Cond::FirstUseEver);
    if (!ui.Begin("Widget Gallery", open, WindowFlags::MenuBar)) {
        ui.End();
        return;
    }
    GallerySettings& s = Gallery();
    WindowBackdrop(ui, s.backdrop, s.backdropThemeColors ? ThemeBackdropColors(ui) : s.backdropColors);
    if (ui.BeginMenuBar()) {
        if (ui.BeginMenu("File")) {
            ui.MenuItem("New", "Ctrl+N");
            ui.MenuItem("Open", "Ctrl+O");
            if (ui.BeginMenu("Open Recent")) {
                ui.MenuItem("level_forest.map");
                ui.MenuItem("level_desert.map");
                if (ui.BeginMenu("More")) {
                    ui.MenuItem("level_snow.map");
                    ui.MenuItem("level_space.map");
                    ui.EndMenu();
                }
                ui.EndMenu();
            }
            ui.Separator();
            ui.MenuItem("Save", "Ctrl+S");
            ui.EndMenu();
        }
        if (ui.BeginMenu("Edit")) {
            ui.MenuItem("Undo", "Ctrl+Z");
            ui.MenuItem("Redo", "Ctrl+Y", false, false);
            ui.Separator();
            static bool wordWrap = true;
            if (ui.MenuItem("Word wrap", {}, wordWrap)) wordWrap = !wordWrap;
            ui.EndMenu();
        }
        if (ui.BeginMenu("View")) {
            ViewMenu(ui);
            ui.EndMenu();
        }
        ui.EndMenuBar();
    }
    ui.Text("Every drizzy widget, grouped into tabs. Customize changes the look.");
    ui.Separator();
    if (ui.BeginTabBar("gallery_tabs")) {
        if (ui.BeginTabItem("Basics")) { BasicsTab(ui, image); ui.EndTabItem(); }
        if (ui.BeginTabItem("Input")) { InputTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Color")) { ColorTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Lists")) { ListsTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Trees")) { TreesTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Popups")) { PopupsTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Plots")) { PlotsTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Tables")) { TablesTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Extras")) { ExtrasTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Layout")) { LayoutTab(ui); ui.EndTabItem(); }
        if (ui.BeginTabItem("Customize")) { CustomizeTab(ui); ui.EndTabItem(); }
        ui.EndTabBar();
    }
    ui.End();
}

void ShowStyleShowcase(Ui& ui, bool themes) {
    struct Entry {
        UiTheme theme;
        UiLook look;
        const char* font;  // a name prefix in Gallery().fonts; null keeps the current font
        BackdropKind backdrop;
    };
    static const Entry kLooks[] = {
        {UiTheme::Obsidian, UiLook::Classic, "Inter", BackdropKind::Constellation},
        {UiTheme::Nord, UiLook::Soft, "Varela", BackdropKind::Bokeh},
        {UiTheme::Cyberpunk, UiLook::Neon, "Orbitron", BackdropKind::Synthwave},
        {UiTheme::Light, UiLook::Flat, "Inter", BackdropKind::Waves},
        {UiTheme::Emerald, UiLook::Retro, "Monocraft", BackdropKind::MatrixRain},
        {UiTheme::Dracula, UiLook::Glass, "Rajdhani", BackdropKind::Gradient},
    };
    static Backdrop backdrops[9];
    const GallerySettings& s = Gallery();
    const UiStyle saved = ui.Style();
    const int count = themes ? int(UiTheme::Count) : int(sizeof(kLooks) / sizeof(kLooks[0]));
    const int columns = 3, rows = (count + columns - 1) / columns;
    const Vec2 display = ui.GetDisplaySize();
    const float margin = 24.0f;
    const Vec2 cell((display.x - margin * float(columns + 1)) / float(columns),
                    (display.y - margin * float(rows + 1)) / float(rows));
    for (int i = 0; i < count; ++i) {
        Entry e = themes ? Entry{UiTheme(i), s.look, nullptr, BackdropKind::None} : kLooks[i];
        UiStyle style = saved;
        style.ApplyLook(e.look);
        style.ApplyTheme(e.theme);
        const DemoFont* font = nullptr;
        for (const DemoFont& f : s.fonts) {
            if (e.font && std::strncmp(f.name, e.font, std::strlen(e.font)) == 0) font = &f;
        }
        ui.Style() = style;
        ui.PushFont(font ? font->font : saved.font, font ? font->size : saved.fontSize);
        char title[96];
        std::snprintf(title, sizeof(title), "%s  |  %s###showcase%d", UiStyle::LookName(e.look),
                      UiStyle::ThemeName(e.theme), i);
        ui.SetNextWindowPos({margin + float(i % columns) * (cell.x + margin), margin + float(i / columns) * (cell.y + margin)});
        ui.SetNextWindowSize(cell);
        if (ui.Begin(title, nullptr, WindowFlags::NoSavedSettings | WindowFlags::NoCollapse)) {
            if (e.backdrop != BackdropKind::None) {
                Backdrop& b = backdrops[i];
                b.kind = e.backdrop;
                b.opacity = e.backdrop == BackdropKind::MatrixRain ? 0.4f : e.backdrop == BackdropKind::Synthwave ? 0.7f : 0.8f;
                WindowBackdrop(ui, b, ThemeBackdropColors(ui));
            }
            ui.PushID(i);
            MiniMenu(ui, i);
            ui.PopID();
            if (font) ui.TextDisabled(font->name);
        }
        ui.End();
        ui.PopFont();
    }
    ui.Style() = saved;
}

} // namespace demo
