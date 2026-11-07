#pragma once
#include "imgui.h"
#include "ui/Theme.h"

#include <string>

// Replayr's widget library. Every control in the app is drawn here with ImDrawList, so nothing
// looks like stock ImGui. Each widget is still a normal ImGui item (ids, layout, focus, hover).
// All state changes ease in and out through Anim().
namespace cs::ui {

// ---- buttons ------------------------------------------------------------------------------
enum class Btn {
    Primary,     // violet->magenta gradient
    Secondary,   // frosted glass
    Ghost,       // transparent until hovered
    Danger,      // red
    White,       // solid white, dark text (like "Upgrade to Premium" in the reference)
};
struct ButtonOpts {
    Btn style = Btn::Secondary;
    const char* icon = nullptr;
    ImVec2 size = ImVec2(0, 0);      // 0 = fit (icon-only buttons are square)
    const char* tooltip = nullptr;
    bool enabled = true;
    bool round = false;              // fully rounded (circle when icon-only)
    bool active = false;             // toggled-on look (for Ghost icon buttons)
};
// The one button. A label like "##id" with an icon makes an icon-only button.
// Every button reacts with a light sweep on hover and a wave (ripple + ring + spring) on click.
bool Button(const char* label, const ButtonOpts& o = ButtonOpts());

// The same hover sweep + click wave, for custom-drawn clickable things (nav items, cards).
// Called with 0 when a control starts being hovered and 1 when one is clicked (quiet UI sounds).
void SetUiSoundHook(void (*hook)(int));
void WaveFx(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, float rounding, bool hovered, bool clicked, ImU32 color);

// ---- inputs -------------------------------------------------------------------------------
bool Toggle(const char* id, bool* v, bool enabled = true);
bool Slider(const char* id, float* v, float min, float max, const char* valueText, float width = -1.0f, float step = 0.0f);
bool SliderInt(const char* id, int* v, int min, int max, const char* fmt, int step = 1, float width = -1.0f);
bool TextField(const char* id, char* buf, size_t size, const char* hint = nullptr, float width = -1.0f, ImGuiInputTextFlags flags = 0,
               const char* icon = nullptr, float trailing = 0.0f);   // trailing: space kept free on the right (e.g. a clear button)
bool Segmented(const char* id, int* current, const char* const* items, int count, float width = -1.0f);

// Dropdown: BeginDropdown + DropdownItem... + EndDropdown (only when Begin returned true).
bool BeginDropdown(const char* id, const char* preview, float width = -1.0f, const char* icon = nullptr);
bool DropdownItem(const char* label, bool selected, const char* icon = nullptr, bool enabled = true);
void EndDropdown();
bool Dropdown(const char* id, int* current, const char* const* items, int count, float width = -1.0f);

enum class BinderState { Normal, Capturing, Duplicate, InUse };
bool HotkeyBinder(const char* id, const std::string& text, BinderState state, float width = -1.0f);
float KeyCapsWidth(const std::string& text, float height);
void KeyCaps(ImDrawList* dl, ImVec2 pos, const std::string& text, float height, bool onLight = false);

// ---- menus, modals, tooltips --------------------------------------------------------------
bool BeginMenu(const char* popupId);             // after ImGui::OpenPopup(popupId)
bool MenuItem(const char* icon, const char* label, const char* shortcut = nullptr, bool danger = false, bool enabled = true);
void MenuSeparator();
void MenuLabel(const char* text);
void EndMenu();

bool BeginModal(const char* popupId, const char* title, float width);   // after ImGui::OpenPopup
void EndModal();

void Tooltip(const char* text);                  // glass tooltip for the last item, delayed fade-in

// ---- text ---------------------------------------------------------------------------------
void Text(const char* text, ImU32 col, ImFont* font = nullptr, float size = 0.0f);
void TextWrapped(const char* text, ImU32 col, float wrapWidth, ImFont* font = nullptr, float size = 0.0f);
// Truncates with an ellipsis and shows the full text as a tooltip when hovered.
void TextEllipsis(const char* text, float maxWidth, ImU32 col, ImFont* font = nullptr, float size = 0.0f);
std::string Ellipsize(const char* text, float maxWidth);   // with the current font
void Gap(float px);                                         // vertical space (unscaled px)

// ---- decoration ---------------------------------------------------------------------------
ImVec2 BadgeSize(const char* text);
void Badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg);
void GameAvatar(ImDrawList* dl, ImVec2 pos, float size, const std::string& game);
void Skeleton(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding);   // shimmering placeholder
void Spinner(ImDrawList* dl, ImVec2 center, float radius, ImU32 col);
void ProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float fraction, bool onAccent = false);
void PulseDot(ImDrawList* dl, ImVec2 center, float radius, ImU32 col);
void LevelMeter(const char* id, float peak, ImVec2 size, bool dimmed = false);
void Divider(float alpha = 0.12f);

// ---- panels -------------------------------------------------------------------------------
// A glass panel = child window with a frosted background. size.y == 0 fits the contents.
bool BeginPanel(const char* id, ImVec2 size, float padding = Tok::Space5, bool scroll = false, float rounding = Tok::RadiusCard,
                float border = Tok::GlassBorderSoft);
void EndPanel();
void PageHeader(const char* title, const char* subtitle = nullptr);
void SectionHeader(const char* title, const char* right = nullptr);

// ---- clip card ----------------------------------------------------------------------------
struct CardData {
    const char* title = "";
    const char* game = "";
    const char* when = "";             // "2h ago"
    const char* badge = "";            // "0:30" or "2560×1440"
    bool isImage = false;
    bool compact = false;              // screenshot gallery: text only on hover
    ImTextureID thumb = ImTextureID_Invalid;
    int thumbW = 0, thumbH = 0;
    bool thumbLoading = false;
    ImTextureID strip = ImTextureID_Invalid;   // filmstrip: plays as a preview while hovered
    int stripFrames = 0;
    int bookmarks = 0;                 // bookmark count badge (bottom-left of the thumbnail)
};
struct CardResult {
    bool hovered = false;
    bool clicked = false;
    bool rightClicked = false;
    bool edit = false, more = false;
};
CardResult ClipCard(const char* id, const CardData& d, ImVec2 size);
float CardTextHeight();               // height of the text block under a card's thumbnail

} // namespace cs::ui
