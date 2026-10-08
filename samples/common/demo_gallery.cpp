#include "demo_gallery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace drizzy;

namespace demo {

// =====================================================================================================================
// ParticleBackground
// =====================================================================================================================
float ParticleBackground::Random01() {
    m_rng ^= m_rng << 13;  // xorshift32
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return float(m_rng >> 8) * (1.0f / 16777216.0f);
}

void ParticleBackground::Update(float deltaTime, const Rect& area) {
    const Vec2 size = area.Size();
    const Vec2 oldSize = m_area.Size();
    m_area = area;
    if (size.x <= 0.0f || size.y <= 0.0f) return;
    if (oldSize.x > 0.0f && oldSize.y > 0.0f && oldSize != size) {
        const Vec2 scale(size.x / oldSize.x, size.y / oldSize.y);
        for (Node& node : m_nodes) node.pos = {node.pos.x * scale.x, node.pos.y * scale.y};
    }
    // Constant density: a growing area gains points (they fade in), a shrinking one drops them. The first seeding
    // shows at once, so a single-frame screenshot is fully populated.
    const int target = std::clamp(int(size.x * size.y * density * 1e-4f), 0, std::max(maxCount, 0));
    const float startAge = m_nodes.empty() ? 1.0f : 0.0f;
    if (int(m_nodes.size()) > target) m_nodes.resize(size_t(target));
    while (int(m_nodes.size()) < target) {
        const float angle = Random01() * 2.0f * kPi;
        const Vec2 pos(Random01() * size.x, Random01() * size.y);
        m_nodes.push_back({pos, {std::cos(angle) * speed, std::sin(angle) * speed}, startAge});
    }

    deltaTime = Clamp(deltaTime, 0.0f, 0.05f);  // stay stable after a long stall
    for (Node& node : m_nodes) {
        node.age += deltaTime;
        node.pos += node.vel * deltaTime;
        // Bounce off the edges, so the field stays evenly spread.
        if (node.pos.x < 0.0f) { node.pos.x = 0.0f; node.vel.x = std::fabs(node.vel.x); }
        else if (node.pos.x > size.x) { node.pos.x = size.x; node.vel.x = -std::fabs(node.vel.x); }
        if (node.pos.y < 0.0f) { node.pos.y = 0.0f; node.vel.y = std::fabs(node.vel.y); }
        else if (node.pos.y > size.y) { node.pos.y = size.y; node.vel.y = -std::fabs(node.vel.y); }
    }
}

void ParticleBackground::Draw(DrawList& dl, Vec2 mouse) const {
    if (ColorAlpha(background)) dl.AddRectFilled(m_area, background);
    const Vec2 origin = m_area.min;
    const float linkSq = linkDistance * linkDistance;
    auto fadeIn = [](const Node& node) { return std::min(node.age * 2.0f, 1.0f); };

    // Point-to-point links: alpha fades with distance. O(n^2), fine for a few hundred points.
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        const Node& a = m_nodes[i];
        for (size_t j = i + 1; j < m_nodes.size(); ++j) {
            const Node& b = m_nodes[j];
            const float d2 = LengthSq(b.pos - a.pos);
            if (d2 >= linkSq) continue;
            const float t = (1.0f - std::sqrt(d2) / linkDistance) * std::min(fadeIn(a), fadeIn(b));
            dl.AddLine(origin + a.pos, origin + b.pos, ScaleAlpha(lineColor, t * 0.4f), 1.0f);
        }
    }
    // Point-to-cursor links: brighter, so the field visibly reaches for the mouse as it moves.
    if (m_area.Contains(mouse)) {
        const float cursorSq = cursorDistance * cursorDistance;
        for (const Node& node : m_nodes) {
            const Vec2 p = origin + node.pos;
            const float d2 = LengthSq(p - mouse);
            if (d2 >= cursorSq) continue;
            const float t = std::min((1.0f - std::sqrt(d2) / cursorDistance) * 1.6f, 1.0f) * fadeIn(node);
            dl.AddLine(p, mouse, ScaleAlpha(cursorColor, t), 1.25f);
        }
    }
    // Points on top of the lines.
    for (const Node& node : m_nodes) dl.AddCircleFilled(origin + node.pos, 1.6f, ScaleAlpha(pointColor, fadeIn(node)));
}

// =====================================================================================================================
// ShowWidgetGallery
// =====================================================================================================================
namespace {

// The gallery's animated window background, in the theme's colors: points and links in the text color, cursor links
// in the accent color. Called right after Begin, so it draws under every widget, clipped to the content area (below
// the title and menu bars, beside the scrollbar).
void WindowBackdrop(Ui& ui) {
    static ParticleBackground backdrop;
    backdrop.lineColor = backdrop.pointColor = ui.GetColor(UiColor::Text);
    backdrop.cursorColor = ui.GetColor(UiColor::CheckMark);
    DrawList& dl = ui.WindowDrawList();
    backdrop.Update(ui.Input().deltaTime, dl.ClipRect());
    backdrop.Draw(dl, ui.IsWindowHovered() ? ui.GetMousePos() : Vec2(-1e30f, -1e30f));
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
    ui.SeparatorText("Color edit");
    static Color fill = Hex(0xF28C28);
    static Color tint = Rgba(90, 200, 255, 180);
    static Color rgb = Hex(0x8AC926);
    ui.ColorEdit4("ColorEdit4 (RGBA)", &fill);
    ui.ColorEdit4("With alpha", &tint);
    ui.ColorEdit3("ColorEdit3 (RGB)", &rgb);
    ui.ColorEdit4("Hex input", &fill, ColorEditFlags::DisplayHex);
    ui.Text("ColorButton:");
    ui.SameLine();
    ui.ColorButton("b1", Hex(0xF15BB5));
    ui.SameLine();
    ui.ColorButton("b2", Hex(0x00BBF9));
    ui.SameLine();
    ui.ColorButton("b3", Rgba(0, 245, 212, 120));
    ui.SeparatorText("Inline picker");
    static Color picked = Hex(0x9B5DE5);
    ui.SetNextItemWidth(200.0f);
    ui.ColorPicker4("##picker", &picked);
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

void StyleTab(Ui& ui) {
    ui.SeparatorText("Theme");
    static int theme = 0;
    const int previous = theme;
    ui.RadioButton("Dark", &theme, 0);
    ui.SameLine();
    ui.RadioButton("Light", &theme, 1);
    if (theme != previous) {
        const Font* font = ui.Style().font;
        const float size = ui.Style().fontSize;
        ui.Style() = theme == 0 ? UiStyle::Dark() : UiStyle::Light();
        ui.Style().font = font;
        ui.Style().fontSize = size;
    }
    ui.SliderFloat("Window rounding", &ui.Style().windowRounding, 0.0f, 16.0f, "%.0f");
    ui.SliderFloat("Frame rounding", &ui.Style().frameRounding, 0.0f, 12.0f, "%.0f");
    ui.SliderFloat("Frame border", &ui.Style().frameBorderSize, 0.0f, 2.0f, "%.0f");
    ui.DragFloat("Item spacing Y", &ui.Style().itemSpacing.y, 0.2f, 0.0f, 20.0f, "%.0f");
    ui.TextDisabled("Themes also load/save as text (UiStyle::LoadTheme/SaveTheme).");
}

} // namespace

void ShowWidgetGallery(Ui& ui, TextureId image, bool* open) {
    ui.SetNextWindowSize({640.0f, 600.0f}, Cond::FirstUseEver);
    ui.SetNextWindowPos({40.0f, 40.0f}, Cond::FirstUseEver);
    if (!ui.Begin("Widget Gallery", open, WindowFlags::MenuBar)) {
        ui.End();
        return;
    }
    WindowBackdrop(ui);
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
            static int theme = 0;
            if (ui.MenuItem("Dark", {}, theme == 0)) theme = 0;
            if (ui.MenuItem("Light", {}, theme == 1)) theme = 1;
            ui.EndMenu();
        }
        ui.EndMenuBar();
    }
    ui.Text("Every drizzy widget, grouped into tabs.");
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
        if (ui.BeginTabItem("Style")) { StyleTab(ui); ui.EndTabItem(); }
        ui.EndTabBar();
    }
    ui.End();
}

} // namespace demo
