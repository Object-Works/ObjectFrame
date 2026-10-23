#include "settings_window.h"

#include "config.h"
#include "border_overlay.h"
#include "resource.h"
#include "version.h"
#include "window_utils.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <sstream>

// GDI+ expects the min/max macros that NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

namespace of {

namespace {

constexpr wchar_t kClassName[] = L"ObjectFrameSettings";
constexpr int kClientW = 912;
constexpr int kClientH = 656;

// Design metrics (96 DPI)
constexpr int kSideW = 228;        // navigation column
constexpr int kContentL = 244;     // cards
constexpr int kContentR = 896;
constexpr int kInnerL = 264;       // controls inside full-width cards
constexpr int kInnerR = 876;
constexpr int kRowH = 44;
constexpr int kNavTop = 64;
constexpr int kNavItemH = 40;
constexpr int kFooterY = 608;

constexpr COLORREF kBackground = RGB(243, 243, 243);
constexpr COLORREF kCardColor = RGB(255, 255, 255);
constexpr COLORREF kCardBorder = RGB(229, 229, 229);
constexpr COLORREF kDivider = RGB(240, 240, 240);
constexpr COLORREF kTextColor = RGB(27, 27, 27);
constexpr COLORREF kSubtleText = RGB(96, 96, 96);
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ObjectFrame";

enum Id : int {
    IDC_HINT = 1000,
    // General
    IDC_MAINMOD,
    IDC_LAYOUT,
    IDC_GAPS_IN,
    IDC_GAPS_IN_UD,
    IDC_GAPS_OUT,
    IDC_GAPS_OUT_UD,
    IDC_MASTER,
    IDC_MASTER_UD,
    IDC_DWINDLE,
    IDC_DWINDLE_UD,
    IDC_WORKSPACES,
    IDC_WORKSPACES_UD,
    IDC_CURSOR,
    IDC_FOCUS_MOUSE,
    IDC_ANIM_MOVE,
    IDC_ANIM_WORKSPACE,
    IDC_ANIM_DURATION,
    IDC_ANIM_DURATION_UD,
    IDC_ANIM_DIRECTION,
    IDC_COLUMN_WIDTH,
    IDC_COLUMN_WIDTH_UD,
    IDC_WS_MODE,
    // Border & glow
    IDC_BORDER,
    IDC_BORDER_WIDTH,
    IDC_BORDER_WIDTH_UD,
    IDC_RADIUS,
    IDC_RADIUS_UD,
    IDC_COLOR_ACTIVE,
    IDC_COLOR_INACTIVE,
    IDC_GLOW,
    IDC_GLOW_SIZE,
    IDC_GLOW_SIZE_UD,
    IDC_GLOW_INTENSITY,
    IDC_GLOW_INTENSITY_UD,
    IDC_PREVIEW,
    // Keybinds
    IDC_BIND_LIST,
    IDC_BIND_SUPER,
    IDC_BIND_ALT,
    IDC_BIND_CTRL,
    IDC_BIND_SHIFT,
    IDC_BIND_KEY,
    IDC_BIND_CMD,
    IDC_BIND_ARGS,
    IDC_BIND_BROWSE,
    IDC_BIND_ADD,
    IDC_BIND_UPDATE,
    IDC_BIND_DELETE,
    IDC_BIND_RESET,
    // Window rules
    IDC_RULE_LIST,
    IDC_RULE_ACTION,
    IDC_RULE_FIELD,
    IDC_RULE_PATTERN,
    IDC_RULE_PICK,
    IDC_RULE_ADD,
    IDC_RULE_UPDATE,
    IDC_RULE_DELETE,
    // Startup
    IDC_EXEC,
    IDC_AUTOSTART,
    // Footer
    IDC_STATUS,
    IDC_SAVE,
    IDC_CLOSE,
};

enum Page : int { kAllPages = -1, kGeneral = 0, kBehavior, kBorder, kBinds, kRules, kStartup, kPageCount };

struct PageInfo {
    const wchar_t* nav;
    const wchar_t* title;
    wchar_t icon;  // Segoe Fluent Icons / MDL2 Assets glyph
};

const PageInfo kPages[kPageCount] = {
    {L"一般", L"一般", 0xE713},
    {L"動作", L"フォーカスとアニメーション", 0xE7C9},
    {L"枠とグロー", L"枠とグロー", 0xE790},
    {L"キーバインド", L"キーバインド", 0xE765},
    {L"ウィンドウルール", L"ウィンドウルール", 0xE8FD},
    {L"起動", L"起動", 0xE7E8},
};
constexpr wchar_t kOpenFileIcon = 0xE8A7;

struct CommandInfo {
    const wchar_t* verb;
    const wchar_t* help;
};

const CommandInfo kCommands[] = {
    {L"focus", L"フォーカス移動  (left / right / up / down)"},
    {L"move", L"ウィンドウ移動  (left / right / up / down)"},
    {L"workspace", L"ワークスペース切替  (1-9 / next / prev)"},
    {L"movetoworkspace", L"ワークスペースへ移動して追従  (1-9)"},
    {L"movetoworkspacesilent", L"ワークスペースへ送る  (1-9)"},
    {L"focusmonitor", L"モニターへフォーカス  (left / right / 番号)"},
    {L"movetomonitor", L"モニターへ移動  (left / right)"},
    {L"layout", L"レイアウト  (next / dwindle / master / monocle / scroll)"},
    {L"togglefloat", L"フローティング切替"},
    {L"fullscreen", L"全画面切替"},
    {L"resize", L"比率調整  (+0.05 / -0.05)"},
    {L"promote", L"メイン位置へ移動"},
    {L"close", L"ウィンドウを閉じる"},
    {L"exec", L"アプリ / コマンドを起動  (パスやコマンド)"},
    {L"retile", L"再タイル"},
    {L"pause", L"一時停止 / 再開"},
    {L"reload", L"設定を再読み込み"},
    {L"settings", L"設定画面を開く"},
    {L"quit", L"終了"},
};

struct ModChoice {
    const wchar_t* label;
    UINT mods;
};

const ModChoice kModChoices[] = {
    {L"SUPER (Windows キー)", MOD_WIN},
    {L"ALT", MOD_ALT},
    {L"CTRL", MOD_CONTROL},
    {L"SUPER + ALT", MOD_WIN | MOD_ALT},
    {L"CTRL + ALT", MOD_CONTROL | MOD_ALT},
    {L"ALT + SHIFT", MOD_ALT | MOD_SHIFT},
};

const LayoutKind kLayouts[] = {LayoutKind::Dwindle, LayoutKind::MasterStack, LayoutKind::Monocle, LayoutKind::Niri};
const WindowRule::Action kActions[] = {WindowRule::Action::Float, WindowRule::Action::Ignore,
                                       WindowRule::Action::Tile};
const WindowRule::Field kFields[] = {WindowRule::Field::Exe, WindowRule::Field::Class, WindowRule::Field::Title};

struct Control {
    HWND hwnd;
    int page;
    RECT design;  // x, y, width, height at 96 DPI
};

// White rounded panel drawn behind a group of controls, optionally with a heading above it
// and dividers between its rows.
struct Card {
    int page;
    RECT design;
    int rows;
    const wchar_t* title;
};

struct OpenWindow {
    std::wstring exe, cls, title;
};

struct State {
    HINSTANCE instance = nullptr;
    HWND wnd = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HFONT sectionFont = nullptr;
    HFONT iconFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH cardBrush = nullptr;
    COLORREF accent = RGB(0, 95, 184);
    ULONG_PTR gdiplusToken = 0;
    std::wstring configPath;
    std::function<void()> onSaved;
    std::vector<Control> controls;
    std::vector<Card> cards;
    int page = kGeneral;
    int hoverNav = -1;  // kPageCount = the "open config file" link
    bool trackingMouse = false;

    Config cfg;
    UINT mainMod = MOD_WIN;
    std::vector<UINT> modValues;
    std::vector<Bind> binds;
    std::vector<WindowRule> rules;
    COLORREF activeColor = 0;
    COLORREF inactiveColor = 0;
    COLORREF customColors[16] = {};
    std::vector<OpenWindow> openWindows;
};

State g;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

HWND Ctl(int id) { return GetDlgItem(g.wnd, id); }
int Scale(int v) { return MulDiv(v, (int)GetDpiForWindow(g.wnd), 96); }

std::wstring GetText(int id) {
    HWND h = Ctl(id);
    int len = GetWindowTextLengthW(h);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    s.resize(len);
    return s;
}

void SetText(int id, const std::wstring& s) { SetWindowTextW(Ctl(id), s.c_str()); }
bool IsChecked(int id) { return SendMessageW(Ctl(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetChecked(int id, bool on) { SendMessageW(Ctl(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
int ComboSel(int id) { return (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0); }
void ComboSetSel(int id, int index) { SendMessageW(Ctl(id), CB_SETCURSEL, index, 0); }
void ComboAdd(int id, const std::wstring& s) { SendMessageW(Ctl(id), CB_ADDSTRING, 0, (LPARAM)s.c_str()); }
int UpDownPos(int id) { return (int)SendMessageW(Ctl(id), UDM_GETPOS32, 0, 0); }
void UpDownSetPos(int id, int v) { SendMessageW(Ctl(id), UDM_SETPOS32, 0, v); }

std::wstring ComboText(int id) {
    int sel = ComboSel(id);
    if (sel < 0) return {};
    int len = (int)SendMessageW(Ctl(id), CB_GETLBTEXTLEN, sel, 0);
    std::wstring s(len + 1, L'\0');
    SendMessageW(Ctl(id), CB_GETLBTEXT, sel, (LPARAM)s.data());
    s.resize(len);
    return s;
}

std::wstring ChordText(UINT mods, UINT vk) {
    std::wstring s;
    for (auto& mod : SplitWhitespace(ModsToString(mods))) s += mod + L" + ";
    return s + KeyToString(vk);
}

std::wstring RegexEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (wcschr(L"\\^$.|?*+()[]{}", c)) out += L'\\';
        out += c;
    }
    return out;
}

void SetStatus(const std::wstring& s) { SetText(IDC_STATUS, s); }

void Warn(const std::wstring& text) { MessageBoxW(g.wnd, text.c_str(), L"ObjectFrame 設定", MB_ICONWARNING); }

// ---------------------------------------------------------------------------
// Control creation and layout
// ---------------------------------------------------------------------------

HWND Add(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0,
         DWORD exStyle = 0) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | style, 0, 0, 0, 0, g.wnd, (HMENU)(INT_PTR)id, g.instance,
                             nullptr);
    g.controls.push_back({c, page, {x, y, w, h}});
    return c;
}

void Label(int page, const wchar_t* text, int x, int y, int w, int id = 0) {
    Add(page, WC_STATICW, text, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, x, y, w, 24, id);
}

void Hint(int page, const wchar_t* text, int x, int y, int w, int h) {
    Add(page, WC_STATICW, text, SS_LEFT | SS_NOPREFIX, x, y, w, h, IDC_HINT);
}

void Number(int page, int id, int x, int y, int lo, int hi) {
    DWORD digitsOnly = lo >= 0 ? ES_NUMBER : 0;  // ES_NUMBER would reject the minus sign
    Add(page, WC_EDITW, L"", WS_TABSTOP | digitsOnly | ES_AUTOHSCROLL, x, y, 90, 24, id, WS_EX_CLIENTEDGE);
    HWND ud = Add(page, UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0,
                  0, 0, id + 1);
    SendMessageW(ud, UDM_SETRANGE32, lo, hi);
}

void Combo(int page, int id, int x, int y, int w) {
    Add(page, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, w, 320, id);
}

// `y` is the top of a 24px row slot, like the other controls; buttons are a little taller.
void Button(int page, int id, const wchar_t* text, int x, int y, int w, DWORD extra = 0) {
    Add(page, WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON | extra, x, y - 4, w, 32, id);
}

void Check(int page, int id, const wchar_t* text, int x, int y, int w) {
    Add(page, WC_BUTTONW, text, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, 24, id);
}

HWND ListView(int page, int id, int x, int y, int w, int h) {
    HWND lv = Add(page, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, x, y, w, h,
                  id);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(lv, L"Explorer", nullptr);
    return lv;
}

void AddColumn(HWND lv, int index, const wchar_t* title) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = 100;
    ListView_InsertColumn(lv, index, &col);
}

COLORREF AccentColor() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentColorMenu", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value & 0xFFFFFF;  // stored as 0xAABBGGRR, the same channel order as COLORREF
    }
    return RGB(0, 95, 184);
}

bool FontExists(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        },
        (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

void ApplyFont() {
    UINT dpi = GetDpiForWindow(g.wnd);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    HFONT font = CreateFontIndirectW(&lf);
    for (auto& c : g.controls) SendMessageW(c.hwnd, WM_SETFONT, (WPARAM)font, TRUE);

    LOGFONTW title = lf;
    title.lfHeight = -MulDiv(26, dpi, 96);
    title.lfWeight = FW_SEMIBOLD;
    LOGFONTW section = lf;
    section.lfHeight = -MulDiv(14, dpi, 96);
    section.lfWeight = FW_SEMIBOLD;
    LOGFONTW icon{};
    icon.lfHeight = -MulDiv(16, dpi, 96);
    icon.lfCharSet = DEFAULT_CHARSET;
    icon.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(icon.lfFaceName, FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");

    for (HFONT* f : {&g.font, &g.titleFont, &g.sectionFont, &g.iconFont}) {
        if (*f) DeleteObject(*f);
    }
    g.font = font;
    g.titleFont = CreateFontIndirectW(&title);
    g.sectionFont = CreateFontIndirectW(&section);
    g.iconFont = CreateFontIndirectW(&icon);
}

void LayoutControls() {
    for (auto& c : g.controls) {
        SetWindowPos(c.hwnd, nullptr, Scale(c.design.left), Scale(c.design.top), Scale(c.design.right),
                     Scale(c.design.bottom), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int ud : {IDC_GAPS_IN_UD, IDC_GAPS_OUT_UD, IDC_MASTER_UD, IDC_DWINDLE_UD, IDC_WORKSPACES_UD,
                   IDC_ANIM_DURATION_UD, IDC_COLUMN_WIDTH_UD, IDC_BORDER_WIDTH_UD, IDC_RADIUS_UD, IDC_GLOW_SIZE_UD,
                   IDC_GLOW_INTENSITY_UD}) {
        SendMessageW(Ctl(ud), UDM_SETBUDDY, (WPARAM)Ctl(ud - 1), 0);
    }
    HWND binds = Ctl(IDC_BIND_LIST);
    ListView_SetColumnWidth(binds, 0, Scale(220));
    ListView_SetColumnWidth(binds, 1, Scale(396));
    HWND rules = Ctl(IDC_RULE_LIST);
    ListView_SetColumnWidth(rules, 0, Scale(90));
    ListView_SetColumnWidth(rules, 1, Scale(80));
    ListView_SetColumnWidth(rules, 2, Scale(446));
}

void ShowPage(int page) {
    g.page = page;
    for (auto& c : g.controls) ShowWindow(c.hwnd, (c.page == kAllPages || c.page == page) ? SW_SHOW : SW_HIDE);
    InvalidateRect(g.wnd, nullptr, TRUE);
}

// Adds a card and returns the top of its first row's controls.
int AddCard(int page, int left, int top, int right, int rows, const wchar_t* title = nullptr, int bottom = 0) {
    g.cards.push_back({page, {left, top, right, bottom ? bottom : top + rows * kRowH + 8}, rows, title});
    return top + 14;
}

void CreateControls() {
    // Two label + number pairs fit side by side in a full-width card.
    constexpr int kNumL = 454, kLabel2 = 580, kNumR = 786;
    constexpr int kWideCombo = kInnerR - 240;

    // General
    int y = AddCard(kGeneral, kContentL, 96, kContentR, 1, L"キーボード");
    Label(kGeneral, L"メインモディファイア ($mod)", kInnerL, y, 300);
    Combo(kGeneral, IDC_MAINMOD, kWideCombo, y, 240);

    y = AddCard(kGeneral, kContentL, 186, kContentR, 4, L"レイアウト");
    Label(kGeneral, L"デフォルトのレイアウト", kInnerL, y, 300);
    Combo(kGeneral, IDC_LAYOUT, kWideCombo, y, 240);
    y += kRowH;
    Label(kGeneral, L"ウィンドウ間の隙間 (px)", kInnerL, y, 180);
    Number(kGeneral, IDC_GAPS_IN, kNumL, y, 0, 200);
    Label(kGeneral, L"画面端との隙間 (px)", kLabel2, y, 200);
    Number(kGeneral, IDC_GAPS_OUT, kNumR, y, 0, 200);
    y += kRowH;
    Label(kGeneral, L"master の比率 (%)", kInnerL, y, 180);
    Number(kGeneral, IDC_MASTER, kNumL, y, 10, 90);
    Label(kGeneral, L"dwindle の分割比率 (%)", kLabel2, y, 200);
    Number(kGeneral, IDC_DWINDLE, kNumR, y, 10, 90);
    y += kRowH;
    Label(kGeneral, L"scroll の列幅 (%)", kInnerL, y, 180);
    Number(kGeneral, IDC_COLUMN_WIDTH, kNumL, y, 20, 100);

    y = AddCard(kGeneral, kContentL, 412, kContentR, 1, L"ワークスペース");
    Label(kGeneral, L"ワークスペース数", kInnerL, y, 180);
    Number(kGeneral, IDC_WORKSPACES, kNumL, y, 1, 20);
    Label(kGeneral, L"割り当て", kLabel2, y, 80);
    Combo(kGeneral, IDC_WS_MODE, 666, y, kInnerR - 666);

    Hint(kGeneral,
         L"SUPER は Windows キーです。Win+L はロック画面に予約されているため割り当てできません。\n"
         L"メインモディファイアを変えると、$mod を使うキーバインドがすべて置き換わります。\n"
         L"ワークスペース数は再起動後に反映されます。",
         kContentL + 4, 478, kContentR - kContentL - 8, 60);

    // Focus & animation
    y = AddCard(kBehavior, kContentL, 96, kContentR, 2, L"フォーカス");
    Check(kBehavior, IDC_FOCUS_MOUSE, L"マウスが乗ったウィンドウをアクティブにする", kInnerL, y, 560);
    y += kRowH;
    Check(kBehavior, IDC_CURSOR, L"キーボードでフォーカスを移したとき、マウスカーソルも移動する", kInnerL, y, 560);

    y = AddCard(kBehavior, kContentL, 234, kContentR, 3, L"アニメーション");
    Check(kBehavior, IDC_ANIM_MOVE, L"ウィンドウの移動をアニメーションする", kInnerL, y, 560);
    y += kRowH;
    Check(kBehavior, IDC_ANIM_WORKSPACE, L"ワークスペースの切り替えをスライドさせる", kInnerL, y, 560);
    y += kRowH;
    Label(kBehavior, L"長さ (ms)", kInnerL, y, 180);
    Number(kBehavior, IDC_ANIM_DURATION, kNumL, y, 50, 1000);
    Label(kBehavior, L"スライド方向", kLabel2, y, 86);
    Combo(kBehavior, IDC_ANIM_DIRECTION, 666, y, kInnerR - 666);

    // Border & glow: settings on the left, preview on the right
    constexpr int kSplit = 560, kBorderNum = 450;
    y = AddCard(kBorder, kContentL, 96, kSplit, 5, L"フォーカス枠");
    Check(kBorder, IDC_BORDER, L"フォーカス枠を表示する", kInnerL, y, 280);
    y += kRowH;
    Label(kBorder, L"枠の太さ (px)", kInnerL, y, 170);
    Number(kBorder, IDC_BORDER_WIDTH, kBorderNum, y, 0, 30);
    y += kRowH;
    Label(kBorder, L"角丸の半径 (px)", kInnerL, y, 170);
    Number(kBorder, IDC_RADIUS, kBorderNum, y, 0, 30);
    y += kRowH;
    Label(kBorder, L"アクティブ枠の色", kInnerL, y, 170);
    Add(kBorder, WC_BUTTONW, L"", WS_TABSTOP | BS_OWNERDRAW, kBorderNum, y - 2, 90, 28, IDC_COLOR_ACTIVE);
    y += kRowH;
    Label(kBorder, L"非アクティブ枠の色", kInnerL, y, 170);
    Add(kBorder, WC_BUTTONW, L"", WS_TABSTOP | BS_OWNERDRAW, kBorderNum, y - 2, 90, 28, IDC_COLOR_INACTIVE);

    y = AddCard(kBorder, kContentL, 366, kSplit, 3, L"グロー");
    Check(kBorder, IDC_GLOW, L"アクティブな枠の周りを光らせる", kInnerL, y, 280);
    y += kRowH;
    Label(kBorder, L"光の広がり (px)", kInnerL, y, 170);
    Number(kBorder, IDC_GLOW_SIZE, kBorderNum, y, 0, 60);
    y += kRowH;
    Label(kBorder, L"光の明るさ (%)", kInnerL, y, 170);
    Number(kBorder, IDC_GLOW_INTENSITY, kBorderNum, y, 0, 100);

    AddCard(kBorder, kSplit + 16, 96, kContentR, 0, L"プレビュー", 506);
    Add(kBorder, WC_STATICW, L"", SS_OWNERDRAW, kSplit + 28, 108, kContentR - kSplit - 40, 386, IDC_PREVIEW);
    Hint(kBorder,
         L"太さ 0 のときは Windows 11 標準の 1px 枠に色を付けるだけになります。グローはアクティブ枠の色で光ります。\n"
         L"ウィンドウ自体の角は 0 / 4 / 8px しか選べないため、それと違う半径は枠で埋めて表現します。\n"
         L"太い枠のぶん、ウィンドウは少し小さく配置されます。",
         kContentL + 4, 520, kContentR - kContentL - 8, 60);

    // Keybinds
    AddCard(kBinds, kContentL, 70, kContentR, 0, nullptr, 318);
    HWND blv = ListView(kBinds, IDC_BIND_LIST, kContentL + 6, 76, kContentR - kContentL - 12, 236);
    AddColumn(blv, 0, L"キー");
    AddColumn(blv, 1, L"コマンド");
    y = AddCard(kBinds, kContentL, 356, kContentR, 3, L"編集");
    Check(kBinds, IDC_BIND_SUPER, L"SUPER", kInnerL, y, 76);
    Check(kBinds, IDC_BIND_ALT, L"ALT", 344, y, 60);
    Check(kBinds, IDC_BIND_CTRL, L"CTRL", 408, y, 64);
    Check(kBinds, IDC_BIND_SHIFT, L"SHIFT", 476, y, 70);
    Label(kBinds, L"キー", 560, y, 40);
    Combo(kBinds, IDC_BIND_KEY, 604, y, kInnerR - 604);
    y += kRowH;
    Label(kBinds, L"コマンド", kInnerL, y, 70);
    Combo(kBinds, IDC_BIND_CMD, 336, y, 250);
    Label(kBinds, L"引数", 598, y, 36);
    Add(kBinds, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, 636, y, 130, 24, IDC_BIND_ARGS, WS_EX_CLIENTEDGE);
    Button(kBinds, IDC_BIND_BROWSE, L"アプリを選択...", 772, y, kInnerR - 772);
    y += kRowH;
    Button(kBinds, IDC_BIND_ADD, L"追加", kInnerL, y, 96);
    Button(kBinds, IDC_BIND_UPDATE, L"選択を更新", 366, y, 110);
    Button(kBinds, IDC_BIND_DELETE, L"削除", 482, y, 96);
    Button(kBinds, IDC_BIND_RESET, L"既定に戻す", kInnerR - 120, y, 120);
    Hint(kBinds,
         L"一覧から選ぶと編集欄に読み込まれます。同じキーの組み合わせは1つにしか割り当てられません。\n"
         L"アプリを起動するには「アプリを選択...」で exe やスタートメニューのショートカットを選びます。",
         kContentL + 4, 508, kContentR - kContentL - 8, 40);

    // Window rules
    AddCard(kRules, kContentL, 70, kContentR, 0, nullptr, 290);
    HWND rlv = ListView(kRules, IDC_RULE_LIST, kContentL + 6, 76, kContentR - kContentL - 12, 208);
    AddColumn(rlv, 0, L"動作");
    AddColumn(rlv, 1, L"対象");
    AddColumn(rlv, 2, L"パターン (正規表現)");
    y = AddCard(kRules, kContentL, 328, kContentR, 4, L"編集");
    Label(kRules, L"動作", kInnerL, y, 50);
    Combo(kRules, IDC_RULE_ACTION, 336, y, 220);
    Label(kRules, L"対象", 580, y, 50);
    Combo(kRules, IDC_RULE_FIELD, 636, y, kInnerR - 636);
    y += kRowH;
    Label(kRules, L"パターン", kInnerL, y, 70);
    Add(kRules, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, 336, y, kInnerR - 336, 24, IDC_RULE_PATTERN,
        WS_EX_CLIENTEDGE);
    y += kRowH;
    Label(kRules, L"開いている窓から", kInnerL, y, 120);
    Combo(kRules, IDC_RULE_PICK, 388, y, kInnerR - 388);
    y += kRowH;
    Button(kRules, IDC_RULE_ADD, L"追加", kInnerL, y, 96);
    Button(kRules, IDC_RULE_UPDATE, L"選択を更新", 366, y, 110);
    Button(kRules, IDC_RULE_DELETE, L"削除", 482, y, 96);
    Hint(kRules, L"上から順に評価され、最初に一致したルールが使われます。", 596, y + 4, kInnerR - 596, 20);

    // Startup
    AddCard(kStartup, kContentL, 96, kContentR, 0, L"起動時に実行するコマンド (1行に1つ)", 400);
    Add(kStartup, WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        kContentL + 12, 106, kContentR - kContentL - 20, 284, IDC_EXEC);
    y = AddCard(kStartup, kContentL, 442, kContentR, 1, L"自動起動");
    Check(kStartup, IDC_AUTOSTART, L"Windows へのサインイン時に ObjectFrame を起動する", kInnerL, y, 560);

    // Footer
    Add(kAllPages, WC_STATICW, L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, kContentL + 4, kFooterY + 4, 400, 24,
        IDC_STATUS);
    Add(kAllPages, WC_BUTTONW, L"保存して適用", WS_TABSTOP | BS_OWNERDRAW, kContentR - 228, kFooterY, 128, 32,
        IDC_SAVE);
    Add(kAllPages, WC_BUTTONW, L"閉じる", WS_TABSTOP | BS_PUSHBUTTON, kContentR - 92, kFooterY, 92, 32, IDC_CLOSE);
}

// ---------------------------------------------------------------------------
// Data <-> controls
// ---------------------------------------------------------------------------

void RefreshBindList(int select = -1) {
    HWND lv = Ctl(IDC_BIND_LIST);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < (int)g.binds.size(); ++i) {
        std::wstring key = ChordText(g.binds[i].mods, g.binds[i].vk);
        std::wstring cmd = g.binds[i].command;
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = key.data();
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, i, 1, cmd.data());
    }
    if (select >= 0 && select < (int)g.binds.size()) {
        ListView_SetItemState(lv, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(lv, select, FALSE);
    }
}

void RefreshRuleList(int select = -1) {
    HWND lv = Ctl(IDC_RULE_LIST);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < (int)g.rules.size(); ++i) {
        std::wstring action = RuleActionName(g.rules[i].action);
        std::wstring field = RuleFieldName(g.rules[i].field);
        std::wstring pattern = g.rules[i].pattern;
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = action.data();
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, i, 1, field.data());
        ListView_SetItemText(lv, i, 2, pattern.data());
    }
    if (select >= 0 && select < (int)g.rules.size()) {
        ListView_SetItemState(lv, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(lv, select, FALSE);
    }
}

int SelectedIndex(int listId) { return ListView_GetNextItem(Ctl(listId), -1, LVNI_SELECTED); }

void LoadBindEditor(int index) {
    if (index < 0 || index >= (int)g.binds.size()) return;
    const Bind& b = g.binds[index];
    SetChecked(IDC_BIND_SUPER, b.mods & MOD_WIN);
    SetChecked(IDC_BIND_ALT, b.mods & MOD_ALT);
    SetChecked(IDC_BIND_CTRL, b.mods & MOD_CONTROL);
    SetChecked(IDC_BIND_SHIFT, b.mods & MOD_SHIFT);
    ComboSetSel(IDC_BIND_KEY,
                (int)SendMessageW(Ctl(IDC_BIND_KEY), CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)KeyToString(b.vk).c_str()));
    size_t sp = b.command.find(L' ');
    std::wstring verb = ToLower(b.command.substr(0, sp));
    int cmdIndex = -1;
    for (int i = 0; i < (int)std::size(kCommands); ++i) {
        if (verb == kCommands[i].verb) cmdIndex = i;
    }
    ComboSetSel(IDC_BIND_CMD, cmdIndex);
    SetText(IDC_BIND_ARGS, sp == std::wstring::npos ? L"" : Trim(b.command.substr(sp + 1)));
}

bool ReadBindEditor(Bind& b) {
    b.mods = (IsChecked(IDC_BIND_SUPER) ? MOD_WIN : 0) | (IsChecked(IDC_BIND_ALT) ? MOD_ALT : 0) |
             (IsChecked(IDC_BIND_CTRL) ? MOD_CONTROL : 0) | (IsChecked(IDC_BIND_SHIFT) ? MOD_SHIFT : 0);
    if (!ParseKey(ComboText(IDC_BIND_KEY), b.vk)) {
        Warn(L"キーを選択してください。");
        return false;
    }
    int cmd = ComboSel(IDC_BIND_CMD);
    if (cmd < 0) {
        Warn(L"コマンドを選択してください。");
        return false;
    }
    if ((b.mods & MOD_WIN) && b.vk == 'L') {
        Warn(L"Win+L は Windows のロック画面に予約されているため割り当てできません。");
        return false;
    }
    std::wstring args = Trim(GetText(IDC_BIND_ARGS));
    if (args.empty() && wcscmp(kCommands[cmd].verb, L"exec") == 0) {
        Warn(L"exec には実行するコマンドを引数に入力してください。");
        return false;
    }
    b.command = args.empty() ? std::wstring(kCommands[cmd].verb) : std::wstring(kCommands[cmd].verb) + L" " + args;
    b.display = ChordText(b.mods, b.vk);
    return true;
}

// Returns false if the user declined to replace a conflicting bind.
bool ResolveBindConflict(const Bind& b, int except) {
    for (int i = 0; i < (int)g.binds.size(); ++i) {
        if (i == except || g.binds[i].mods != b.mods || g.binds[i].vk != b.vk) continue;
        std::wstring msg = ChordText(b.mods, b.vk) + L" は既に「" + g.binds[i].command +
                           L"」に割り当てられています。\n置き換えますか？";
        if (MessageBoxW(g.wnd, msg.c_str(), L"ObjectFrame 設定", MB_ICONQUESTION | MB_YESNO) != IDYES) return false;
        g.binds.erase(g.binds.begin() + i);
        return true;
    }
    return true;
}

void RemapMainMod(std::vector<Bind>& binds, UINT from, UINT to) {
    if (from == to || from == 0) return;
    for (auto& b : binds) {
        if ((b.mods & from) == from) b.mods = (b.mods & ~from) | to;
    }
}

void LoadRuleEditor(int index) {
    if (index < 0 || index >= (int)g.rules.size()) return;
    const WindowRule& r = g.rules[index];
    for (int i = 0; i < 3; ++i) {
        if (kActions[i] == r.action) ComboSetSel(IDC_RULE_ACTION, i);
        if (kFields[i] == r.field) ComboSetSel(IDC_RULE_FIELD, i);
    }
    SetText(IDC_RULE_PATTERN, r.pattern);
}

bool ReadRuleEditor(WindowRule& r) {
    int action = ComboSel(IDC_RULE_ACTION), field = ComboSel(IDC_RULE_FIELD);
    std::wstring pattern = Trim(GetText(IDC_RULE_PATTERN));
    if (action < 0 || field < 0 || pattern.empty()) {
        Warn(L"動作・対象・パターンを入力してください。");
        return false;
    }
    if (!MakeWindowRule(kActions[action], kFields[field], pattern, r)) {
        Warn(L"パターンが正しい正規表現ではありません:\n" + pattern);
        return false;
    }
    return true;
}

void RefreshOpenWindows() {
    g.openWindows.clear();
    SendMessageW(Ctl(IDC_RULE_PICK), CB_RESETCONTENT, 0, 0);
    EnumWindows(
        [](HWND h, LPARAM) -> BOOL {
            if (!IsWindowVisible(h) || GetAncestor(h, GA_ROOT) != h || IsCloaked(h)) return TRUE;
            if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            std::wstring title = GetWindowTitle(h);
            std::wstring cls = GetWindowClass(h);
            if (title.empty() || IsShellWindowClass(cls)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            g.openWindows.push_back({GetProcessExeName(pid), cls, title});
            return TRUE;
        },
        0);
    for (auto& w : g.openWindows) {
        std::wstring title = w.title.size() > 50 ? w.title.substr(0, 50) + L"…" : w.title;
        ComboAdd(IDC_RULE_PICK, w.exe + L"  —  " + title + L"  [" + w.cls + L"]");
    }
}

void PickOpenWindow() {
    int sel = ComboSel(IDC_RULE_PICK);
    int field = ComboSel(IDC_RULE_FIELD);
    if (sel < 0 || sel >= (int)g.openWindows.size()) return;
    if (field < 0) {
        field = 0;
        ComboSetSel(IDC_RULE_FIELD, 0);
    }
    const OpenWindow& w = g.openWindows[sel];
    const std::wstring& value = kFields[field] == WindowRule::Field::Exe     ? w.exe
                                : kFields[field] == WindowRule::Field::Class ? w.cls
                                                                             : w.title;
    SetText(IDC_RULE_PATTERN, L"^" + RegexEscape(value) + L"$");
}

std::wstring AutostartCommand() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    if (_wcsicmp(g.configPath.c_str(), DefaultConfigPath().c_str()) != 0) cmd += L" --config \"" + g.configPath + L"\"";
    return cmd;
}

bool IsAutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void SetAutostart(bool enabled) {
    if (enabled) {
        std::wstring cmd = AutostartCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                        (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
}

void PopulateControls() {
    const Config& c = g.cfg;

    g.modValues.clear();
    for (auto& m : kModChoices) {
        ComboAdd(IDC_MAINMOD, m.label);
        g.modValues.push_back(m.mods);
    }
    auto known = std::find(g.modValues.begin(), g.modValues.end(), c.mainMod);
    if (known == g.modValues.end()) {
        ComboAdd(IDC_MAINMOD, ModsToString(c.mainMod));
        g.modValues.push_back(c.mainMod);
        known = g.modValues.end() - 1;
    }
    ComboSetSel(IDC_MAINMOD, (int)(known - g.modValues.begin()));
    g.mainMod = c.mainMod;

    ComboAdd(IDC_LAYOUT, L"dwindle (交互に分割)");
    ComboAdd(IDC_LAYOUT, L"master (メイン + スタック)");
    ComboAdd(IDC_LAYOUT, L"monocle (全画面で重ねる)");
    ComboAdd(IDC_LAYOUT, L"scroll (横に並ぶ列をスクロール)");
    for (int i = 0; i < (int)std::size(kLayouts); ++i) {
        if (kLayouts[i] == c.defaultLayout) ComboSetSel(IDC_LAYOUT, i);
    }

    UpDownSetPos(IDC_GAPS_IN_UD, c.gapsIn);
    UpDownSetPos(IDC_GAPS_OUT_UD, c.gapsOut);
    UpDownSetPos(IDC_MASTER_UD, (int)std::lround(c.masterRatio * 100));
    UpDownSetPos(IDC_DWINDLE_UD, (int)std::lround(c.dwindleRatio * 100));
    UpDownSetPos(IDC_WORKSPACES_UD, c.workspaceCount);
    UpDownSetPos(IDC_COLUMN_WIDTH_UD, (int)std::lround(c.niriColumnWidth * 100));
    ComboAdd(IDC_WS_MODE, L"モニターごとに独立");
    ComboAdd(IDC_WS_MODE, L"全モニター連動 (順繰り)");
    ComboSetSel(IDC_WS_MODE, c.linkedWorkspaces ? 1 : 0);
    SetChecked(IDC_CURSOR, c.cursorFollowsFocus);
    SetChecked(IDC_FOCUS_MOUSE, c.focusFollowsMouse);
    SetChecked(IDC_ANIM_MOVE, c.animateMove);
    SetChecked(IDC_ANIM_WORKSPACE, c.animateWorkspace);
    UpDownSetPos(IDC_ANIM_DURATION_UD, c.animationDuration);
    ComboAdd(IDC_ANIM_DIRECTION, L"自動 (隣のモニターを避ける)");
    ComboAdd(IDC_ANIM_DIRECTION, L"上へ");
    ComboAdd(IDC_ANIM_DIRECTION, L"下へ");
    ComboAdd(IDC_ANIM_DIRECTION, L"左へ");
    ComboAdd(IDC_ANIM_DIRECTION, L"右へ");
    ComboSetSel(IDC_ANIM_DIRECTION, (int)c.workspaceSlide);

    SetChecked(IDC_BORDER, c.borders);
    UpDownSetPos(IDC_BORDER_WIDTH_UD, c.borderWidth);
    UpDownSetPos(IDC_RADIUS_UD, c.borderRadius);
    g.activeColor = c.borderActive;
    g.inactiveColor = c.borderInactive;
    SetChecked(IDC_GLOW, c.glow);
    UpDownSetPos(IDC_GLOW_SIZE_UD, c.glowSize);
    UpDownSetPos(IDC_GLOW_INTENSITY_UD, c.glowIntensity);

    for (auto& name : AllKeyNames()) ComboAdd(IDC_BIND_KEY, name);
    for (auto& cmd : kCommands) ComboAdd(IDC_BIND_CMD, std::wstring(cmd.verb) + L"  —  " + cmd.help);
    g.binds = c.binds;
    RefreshBindList();

    ComboAdd(IDC_RULE_ACTION, L"float (フローティング)");
    ComboAdd(IDC_RULE_ACTION, L"ignore (管理しない)");
    ComboAdd(IDC_RULE_ACTION, L"tile (強制タイル)");
    ComboAdd(IDC_RULE_FIELD, L"exe (実行ファイル)");
    ComboAdd(IDC_RULE_FIELD, L"class (クラス名)");
    ComboAdd(IDC_RULE_FIELD, L"title (タイトル)");
    ComboSetSel(IDC_RULE_ACTION, 0);
    ComboSetSel(IDC_RULE_FIELD, 0);
    g.rules = c.rules;
    RefreshRuleList();
    RefreshOpenWindows();

    std::wstring exec;
    for (auto& cmd : c.execOnce) exec += cmd + L"\r\n";
    SetText(IDC_EXEC, exec);
    SetChecked(IDC_AUTOSTART, IsAutostartEnabled());
}

void CollectBorderSettings(Config& c) {
    c.borders = IsChecked(IDC_BORDER);
    c.borderWidth = std::clamp(UpDownPos(IDC_BORDER_WIDTH_UD), 0, 30);
    c.borderRadius = std::clamp(UpDownPos(IDC_RADIUS_UD), 0, 30);
    c.borderActive = g.activeColor;
    c.borderInactive = g.inactiveColor;
    c.glow = IsChecked(IDC_GLOW);
    c.glowSize = std::clamp(UpDownPos(IDC_GLOW_SIZE_UD), 0, 60);
    c.glowIntensity = std::clamp(UpDownPos(IDC_GLOW_INTENSITY_UD), 0, 100);
}

bool Save() {
    Config c = g.cfg;
    c.mainMod = g.mainMod;
    int layout = ComboSel(IDC_LAYOUT);
    c.defaultLayout = kLayouts[layout < 0 ? 0 : layout];
    c.gapsIn = UpDownPos(IDC_GAPS_IN_UD);
    c.gapsOut = UpDownPos(IDC_GAPS_OUT_UD);
    c.masterRatio = UpDownPos(IDC_MASTER_UD) / 100.0;
    c.dwindleRatio = UpDownPos(IDC_DWINDLE_UD) / 100.0;
    c.workspaceCount = UpDownPos(IDC_WORKSPACES_UD);
    c.niriColumnWidth = std::clamp(UpDownPos(IDC_COLUMN_WIDTH_UD), 20, 100) / 100.0;
    c.linkedWorkspaces = ComboSel(IDC_WS_MODE) == 1;
    c.cursorFollowsFocus = IsChecked(IDC_CURSOR);
    c.focusFollowsMouse = IsChecked(IDC_FOCUS_MOUSE);
    c.animateMove = IsChecked(IDC_ANIM_MOVE);
    c.animateWorkspace = IsChecked(IDC_ANIM_WORKSPACE);
    c.animationDuration = std::clamp(UpDownPos(IDC_ANIM_DURATION_UD), 50, 1000);
    int slide = ComboSel(IDC_ANIM_DIRECTION);
    c.workspaceSlide = (SlideDirection)(slide < 0 ? 0 : slide);
    CollectBorderSettings(c);
    c.binds = g.binds;
    c.rules = g.rules;

    c.execOnce.clear();
    std::wistringstream lines(GetText(IDC_EXEC));
    std::wstring line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (!line.empty()) c.execOnce.push_back(line);
    }

    // Round-trip through the parser so we never write something we can't read back.
    Config check;
    std::vector<std::wstring> warnings;
    LoadConfigText(SerializeConfig(c), check, warnings);
    if (!warnings.empty()) {
        std::wstring msg = L"設定に問題があります:\n";
        for (auto& w : warnings) msg += L"・" + w + L"\n";
        Warn(msg);
        return false;
    }
    if (!SaveConfigFile(g.configPath, c)) {
        Warn(L"設定ファイルを書き込めませんでした:\n" + g.configPath);
        return false;
    }
    SetAutostart(IsChecked(IDC_AUTOSTART));
    g.cfg = c;

    if (g.onSaved) g.onSaved();
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"保存して適用しました (%02d:%02d:%02d)", st.wHour, st.wMinute, st.wSecond);
    SetStatus(buf);
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

COLORREF& ColorFor(int id) {
    return id == IDC_COLOR_ACTIVE ? g.activeColor : g.inactiveColor;
}

void OnCommand(int id, int code) {
    if (id >= IDC_BORDER && id < IDC_PREVIEW) InvalidateRect(Ctl(IDC_PREVIEW), nullptr, FALSE);

    switch (id) {
        case IDC_MAINMOD:
            if (code == CBN_SELCHANGE) {
                int sel = ComboSel(IDC_MAINMOD);
                if (sel < 0) break;
                UINT mods = g.modValues[sel];
                RemapMainMod(g.binds, g.mainMod, mods);
                g.mainMod = mods;
                RefreshBindList(SelectedIndex(IDC_BIND_LIST));
                SetStatus(L"$mod を変更しました (未保存)");
            }
            break;

        case IDC_COLOR_ACTIVE:
        case IDC_COLOR_INACTIVE: {
            COLORREF& color = ColorFor(id);
            CHOOSECOLORW cc{sizeof(cc)};
            cc.hwndOwner = g.wnd;
            cc.rgbResult = color;
            cc.lpCustColors = g.customColors;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                color = cc.rgbResult;
                InvalidateRect(Ctl(id), nullptr, TRUE);
                InvalidateRect(Ctl(IDC_PREVIEW), nullptr, FALSE);
            }
            break;
        }

        case IDC_BIND_BROWSE: {
            wchar_t initialDir[MAX_PATH] = L"";
            ExpandEnvironmentStringsW(L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs", initialDir,
                                      MAX_PATH);
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = g.wnd;
            ofn.lpstrFilter = L"アプリ (*.exe;*.lnk;*.bat;*.cmd)\0*.exe;*.lnk;*.bat;*.cmd\0すべてのファイル (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrInitialDir = initialDir;
            ofn.lpstrTitle = L"キーで起動するアプリを選択";
            // Keep shortcuts as-is so their arguments and working directory are honored.
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NODEREFERENCELINKS | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) break;
            for (int i = 0; i < (int)std::size(kCommands); ++i) {
                if (wcscmp(kCommands[i].verb, L"exec") == 0) ComboSetSel(IDC_BIND_CMD, i);
            }
            SetText(IDC_BIND_ARGS, L"\"" + std::wstring(file) + L"\"");
            SetStatus(L"キーを選んで「追加」を押してください");
            break;
        }

        case IDC_BIND_ADD: {
            Bind b;
            if (!ReadBindEditor(b) || !ResolveBindConflict(b, -1)) break;
            g.binds.push_back(b);
            RefreshBindList((int)g.binds.size() - 1);
            break;
        }

        case IDC_BIND_UPDATE: {
            int sel = SelectedIndex(IDC_BIND_LIST);
            if (sel < 0) {
                Warn(L"更新するキーバインドを一覧から選択してください。");
                break;
            }
            Bind b;
            if (!ReadBindEditor(b)) break;
            const Bind original = g.binds[sel];
            if (!ResolveBindConflict(b, sel)) break;
            // The conflict resolution may have removed an earlier entry and shifted indices.
            auto it = std::find_if(g.binds.begin(), g.binds.end(), [&](const Bind& x) {
                return x.mods == original.mods && x.vk == original.vk && x.command == original.command;
            });
            if (it == g.binds.end()) break;
            *it = b;
            RefreshBindList((int)(it - g.binds.begin()));
            break;
        }

        case IDC_BIND_DELETE: {
            int sel = SelectedIndex(IDC_BIND_LIST);
            if (sel < 0) break;
            g.binds.erase(g.binds.begin() + sel);
            RefreshBindList(std::min(sel, (int)g.binds.size() - 1));
            break;
        }

        case IDC_BIND_RESET: {
            if (MessageBoxW(g.wnd, L"キーバインドを既定の内容に戻しますか？", L"ObjectFrame 設定",
                            MB_ICONQUESTION | MB_YESNO) != IDYES) {
                break;
            }
            Config defaults;
            std::vector<std::wstring> warnings;
            LoadConfigText(DefaultConfigText(), defaults, warnings);
            g.binds = defaults.binds;
            RemapMainMod(g.binds, defaults.mainMod, g.mainMod);
            RefreshBindList();
            break;
        }

        case IDC_RULE_ADD: {
            WindowRule r;
            if (!ReadRuleEditor(r)) break;
            g.rules.push_back(r);
            RefreshRuleList((int)g.rules.size() - 1);
            break;
        }

        case IDC_RULE_UPDATE: {
            int sel = SelectedIndex(IDC_RULE_LIST);
            if (sel < 0) {
                Warn(L"更新するルールを一覧から選択してください。");
                break;
            }
            WindowRule r;
            if (!ReadRuleEditor(r)) break;
            g.rules[sel] = r;
            RefreshRuleList(sel);
            break;
        }

        case IDC_RULE_DELETE: {
            int sel = SelectedIndex(IDC_RULE_LIST);
            if (sel < 0) break;
            g.rules.erase(g.rules.begin() + sel);
            RefreshRuleList(std::min(sel, (int)g.rules.size() - 1));
            break;
        }

        case IDC_RULE_PICK:
            if (code == CBN_DROPDOWN) RefreshOpenWindows();
            else if (code == CBN_SELCHANGE) PickOpenWindow();
            break;

        case IDC_SAVE:
            Save();
            break;

        case IDC_CLOSE:
        case IDCANCEL:
            DestroyWindow(g.wnd);
            break;
    }
}

uint32_t Blend(uint32_t dst, double r, double gr, double b, double alpha) {
    auto mix = [&](int shift, double c) {
        double d = (dst >> shift) & 0xFF;
        return (uint32_t)std::lround(c * alpha + d * (1.0 - alpha)) << shift;
    };
    return mix(16, r) | mix(8, gr) | mix(0, b);
}

// Draws a stand-in window (title bar + body) with anti-aliased corners.
void DrawMockWindow(uint32_t* canvas, int cw, int ch, const RECT& r, int radius, int titleH) {
    for (int y = std::max<int>(0, r.top - 1); y < std::min<int>(ch, r.bottom + 1); ++y) {
        for (int x = std::max<int>(0, r.left - 1); x < std::min<int>(cw, r.right + 1); ++x) {
            double cov = std::clamp(0.5 - RoundedRectDistance(x + 0.5, y + 0.5, r, radius), 0.0, 1.0);
            if (cov <= 0) continue;
            double shade = y < r.top + titleH ? 236 : 252;
            uint32_t& px = canvas[(size_t)y * cw + x];
            px = Blend(px, shade, shade, shade, cov);
        }
    }
}

void DrawPreview(const DRAWITEMSTRUCT& d) {
    int w = Width(d.rcItem), h = Height(d.rcItem);
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(bi.bmiHeader), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(d.hDC);
    HBITMAP bmp = CreateDIBSection(d.hDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        return;
    }
    auto* canvas = static_cast<uint32_t*>(bits);
    std::fill(canvas, canvas + (size_t)w * h, 0xFFFFFFu);
    // A rounded stand-in for the desktop behind the mock windows.
    RECT desk{0, 0, w, h};
    int deskRadius = Scale(8);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double cov = std::clamp(0.5 - RoundedRectDistance(x + 0.5, y + 0.5, desk, deskRadius), 0.0, 1.0);
            if (cov > 0) canvas[(size_t)y * w + x] = Blend(canvas[(size_t)y * w + x], 226, 231, 239, cov);
        }
    }

    Config c = g.cfg;
    CollectBorderSettings(c);
    UINT dpi = GetDpiForWindow(g.wnd);
    int pad = Scale(30), gap = Scale(16), titleH = Scale(24);
    RECT windows[2] = {
        {pad, pad, w - pad, h / 2 - gap},       // inactive
        {pad, h / 2 + gap, w - pad, h - pad},   // active
    };

    for (int i = 0; i < 2; ++i) {
        const RECT& wr = windows[i];
        BorderStyle s = MakeBorderStyle(c, i == 1).Scaled(dpi);
        if (c.borders && c.borderWidth == 0) s.width = 1;  // native DWM border
        int margin = s.Margin();
        if (margin > 0) {
            int ow = Width(wr) + 2 * margin, oh = Height(wr) + 2 * margin;
            std::vector<uint32_t> deco((size_t)ow * oh);
            RenderBorder(deco.data(), {0, 0, ow, oh}, Width(wr), Height(wr), s);
            for (int y = 0; y < oh; ++y) {
                int cy = wr.top - margin + y;
                if (cy < 0 || cy >= h) continue;
                for (int x = 0; x < ow; ++x) {
                    int cx = wr.left - margin + x;
                    uint32_t src = deco[(size_t)y * ow + x];
                    if (cx < 0 || cx >= w || !src) continue;
                    double a = (src >> 24) / 255.0;
                    uint32_t& px = canvas[(size_t)cy * w + cx];
                    // `src` is premultiplied, so un-premultiply before blending.
                    px = Blend(px, ((src >> 16) & 0xFF) / a, ((src >> 8) & 0xFF) / a, (src & 0xFF) / a, a);
                }
            }
        }
        DrawMockWindow(canvas, w, h, wr, s.windowRadius, titleH);
    }

    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HGDIOBJ oldFont = SelectObject(mem, g.font);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(60, 60, 60));
    const wchar_t* labels[2] = {L"非アクティブ", L"アクティブ"};
    for (int i = 0; i < 2; ++i) {
        RECT tr{windows[i].left + Scale(10), windows[i].top, windows[i].right, windows[i].top + titleH};
        DrawTextW(mem, labels[i], -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
    BitBlt(d.hDC, d.rcItem.left, d.rcItem.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

Gdiplus::Color ToGdi(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

void RoundedPath(Gdiplus::GraphicsPath& path, const RECT& r, float radius) {
    float x = (float)r.left, y = (float)r.top, w = (float)Width(r) - 1, h = (float)Height(r) - 1;
    float d = std::min({radius * 2, w, h});
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRounded(Gdiplus::Graphics& gr, const RECT& r, float radius, COLORREF fill, const COLORREF* border = nullptr) {
    Gdiplus::GraphicsPath path;
    RoundedPath(path, r, radius);
    Gdiplus::SolidBrush brush(ToGdi(fill));
    gr.FillPath(&brush, &path);
    if (border) {
        Gdiplus::Pen pen(ToGdi(*border), 1.0f);
        gr.DrawPath(&pen, &path);
    }
}

COLORREF Shade(COLORREF c, double factor) {
    auto f = [&](int v) { return (BYTE)std::clamp((int)std::lround(v * factor), 0, 255); };
    return RGB(f(GetRValue(c)), f(GetGValue(c)), f(GetBValue(c)));
}

void DrawColorButton(const DRAWITEMSTRUCT& d) {
    COLORREF color = ColorFor(d.CtlID);
    RECT r = d.rcItem;
    FillRect(d.hDC, &r, g.cardBrush);
    Gdiplus::Graphics gr(d.hDC);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    InflateRect(&r, -2, -2);
    COLORREF border = (d.itemState & ODS_FOCUS) ? g.accent : RGB(200, 200, 200);
    FillRounded(gr, r, (float)Scale(5), color, &border);
}

void DrawAccentButton(const DRAWITEMSTRUCT& d) {
    RECT r = d.rcItem;
    FillRect(d.hDC, &r, g.backgroundBrush);
    COLORREF fill = g.accent;
    if (d.itemState & ODS_SELECTED) fill = Shade(fill, 0.85);
    {
        Gdiplus::Graphics gr(d.hDC);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        FillRounded(gr, r, (float)Scale(5), fill);
        if ((d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT)) {
            RECT f = r;
            InflateRect(&f, -Scale(2), -Scale(2));
            Gdiplus::GraphicsPath path;
            RoundedPath(path, f, (float)Scale(4));
            Gdiplus::Pen pen(Gdiplus::Color(200, 255, 255, 255), 1.0f);
            gr.DrawPath(&pen, &path);
        }
    }
    wchar_t text[64];
    GetWindowTextW(d.hwndItem, text, (int)std::size(text));
    HGDIOBJ old = SelectObject(d.hDC, g.font);
    SetBkMode(d.hDC, TRANSPARENT);
    SetTextColor(d.hDC, RGB(255, 255, 255));
    DrawTextW(d.hDC, text, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SelectObject(d.hDC, old);
}

RECT ScaledRect(const RECT& r) { return {Scale(r.left), Scale(r.top), Scale(r.right), Scale(r.bottom)}; }

// Navigation entries, with index kPageCount being the "open config file" link at the bottom.
RECT NavRect(int i) {
    if (i == kPageCount) return ScaledRect({8, kFooterY, kSideW - 8, kFooterY + 34});
    int top = kNavTop + i * kNavItemH;
    return ScaledRect({8, top, kSideW - 8, top + kNavItemH - 4});
}

int NavHit(POINT p) {
    for (int i = 0; i <= kPageCount; ++i) {
        RECT r = NavRect(i);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

void DrawNavItem(HDC dc, Gdiplus::Graphics& gr, int i, wchar_t icon, const wchar_t* text) {
    RECT r = NavRect(i);
    bool selected = i == g.page;
    if (selected || i == g.hoverNav) {
        FillRounded(gr, r, (float)Scale(5), selected ? RGB(233, 233, 233) : RGB(237, 237, 237));
    }
    if (selected) {
        int cy = (r.top + r.bottom) / 2;
        RECT pill{r.left + Scale(3), cy - Scale(8), r.left + Scale(6), cy + Scale(8)};
        FillRounded(gr, pill, (float)Scale(2), g.accent);
    }
    RECT ir{r.left + Scale(12), r.top, r.left + Scale(40), r.bottom};
    SelectObject(dc, g.iconFont);
    SetTextColor(dc, selected ? g.accent : kTextColor);
    DrawTextW(dc, &icon, 1, &ir, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    RECT tr{r.left + Scale(48), r.top, r.right - Scale(8), r.bottom};
    SelectObject(dc, g.font);
    SetTextColor(dc, kTextColor);
    DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void Paint(HDC target, const RECT& client) {
    int w = Width(client), h = Height(client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g.font);
    FillRect(dc, &client, g.backgroundBrush);
    SetBkMode(dc, TRANSPARENT);
    {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // App header
        int iconSize = Scale(24);
        HICON icon = (HICON)LoadImageW(g.instance, MAKEINTRESOURCEW(IDI_OFWM), IMAGE_ICON, iconSize, iconSize, LR_SHARED);
        if (icon) DrawIconEx(dc, Scale(20), Scale(20), icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
        RECT appName{Scale(54), Scale(16), Scale(kSideW), Scale(48)};
        SelectObject(dc, g.sectionFont);
        SetTextColor(dc, kTextColor);
        DrawTextW(dc, L"ObjectFrame", -1, &appName, DT_SINGLELINE | DT_VCENTER | DT_CALCRECT);
        appName.bottom = Scale(48);
        DrawTextW(dc, L"ObjectFrame", -1, &appName, DT_SINGLELINE | DT_VCENTER);
        RECT version{appName.right + Scale(6), Scale(16), Scale(kSideW), Scale(48)};
        SelectObject(dc, g.font);
        SetTextColor(dc, kSubtleText);
        DrawTextW(dc, L"v" OFWM_VERSION_WSTRING, -1, &version, DT_SINGLELINE | DT_VCENTER);

        for (int i = 0; i < kPageCount; ++i) DrawNavItem(dc, gr, i, kPages[i].icon, kPages[i].nav);
        DrawNavItem(dc, gr, kPageCount, kOpenFileIcon, L"設定ファイルを開く");

        // Page heading
        RECT title{Scale(kContentL), Scale(14), Scale(kContentR), Scale(58)};
        SelectObject(dc, g.titleFont);
        SetTextColor(dc, kTextColor);
        DrawTextW(dc, kPages[g.page].title, -1, &title, DT_SINGLELINE | DT_VCENTER);

        COLORREF border = kCardBorder;
        for (const Card& card : g.cards) {
            if (card.page != g.page) continue;
            RECT r = ScaledRect(card.design);
            FillRounded(gr, r, (float)Scale(7), kCardColor, &border);
            HPEN pen = CreatePen(PS_SOLID, 1, kDivider);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            for (int k = 1; k < card.rows; ++k) {
                int y = Scale(card.design.top + 4 + k * kRowH);
                MoveToEx(dc, r.left + Scale(16), y, nullptr);
                LineTo(dc, r.right - Scale(16), y);
            }
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            if (card.title) {
                RECT tr{r.left + Scale(4), r.top - Scale(28), r.right, r.top - Scale(4)};
                SelectObject(dc, g.sectionFont);
                SetTextColor(dc, kTextColor);
                DrawTextW(dc, card.title, -1, &tr, DT_SINGLELINE | DT_VCENTER);
            }
        }
    }
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

bool OnCard(HWND ctl) {
    RECT r;
    GetWindowRect(ctl, &r);
    MapWindowPoints(nullptr, g.wnd, reinterpret_cast<POINT*>(&r), 2);
    POINT c = Center(r);
    for (const Card& card : g.cards) {
        RECT cr = ScaledRect(card.design);
        if (card.page == g.page && PtInRect(&cr, c)) return true;
    }
    return false;
}

void SetHoverNav(int index) {
    if (index == g.hoverNav) return;
    g.hoverNav = index;
    RECT side{0, 0, Scale(kSideW), Scale(kClientH)};
    InvalidateRect(g.wnd, &side, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->code == LVN_ITEMCHANGED) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) &&
                    !(nm->uOldState & LVIS_SELECTED)) {
                    if (hdr->idFrom == IDC_BIND_LIST) LoadBindEditor(nm->iItem);
                    else if (hdr->idFrom == IDC_RULE_LIST) LoadRuleEditor(nm->iItem);
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            Paint(dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!g.trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                g.trackingMouse = TrackMouseEvent(&tme) != FALSE;
            }
            SetHoverNav(NavHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;
        }

        case WM_MOUSELEAVE:
            g.trackingMouse = false;
            SetHoverNav(-1);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && g.hoverNav >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            int hit = NavHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hit == kPageCount) {
                ShellExecuteW(g.wnd, L"open", L"notepad.exe", (L"\"" + g.configPath + L"\"").c_str(), nullptr,
                              SW_SHOWNORMAL);
            } else if (hit >= 0 && hit != g.page) {
                ShowPage(hit);
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto& d = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (wp == IDC_PREVIEW) DrawPreview(d);
            else if (wp == IDC_SAVE) DrawAccentButton(d);
            else DrawColorButton(d);
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            int id = GetDlgCtrlID(ctl);
            bool card = OnCard(ctl);
            SetBkColor(dc, card ? kCardColor : kBackground);
            SetTextColor(dc, id == IDC_HINT || id == IDC_STATUS ? kSubtleText : kTextColor);
            return reinterpret_cast<LRESULT>(card ? g.cardBrush : g.backgroundBrush);
        }

        case WM_SETTINGCHANGE:
            g.accent = AccentColor();
            InvalidateRect(hwnd, nullptr, TRUE);
            break;

        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyFont();
            LayoutControls();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g.controls.clear();
            g.cards.clear();
            for (HFONT* f : {&g.font, &g.titleFont, &g.sectionFont, &g.iconFont}) {
                if (*f) DeleteObject(*f);
                *f = nullptr;
            }
            for (HBRUSH* b : {&g.backgroundBrush, &g.cardBrush}) {
                if (*b) DeleteObject(*b);
                *b = nullptr;
            }
            g.hoverNav = -1;
            g.trackingMouse = false;
            g.wnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void SettingsWindow::Open(HINSTANCE instance, const std::wstring& configPath, std::function<void()> onSaved) {
    if (g.wnd) {
        if (IsIconic(g.wnd)) ShowWindow(g.wnd, SW_RESTORE);
        ForceForeground(g.wnd);
        return;
    }

    static bool initialized = false;
    if (!initialized) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&g.gdiplusToken, &gsi, nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_OFWM));
        wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_OFWM), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        initialized = true;
    }

    g.instance = instance;
    g.configPath = configPath;
    g.onSaved = std::move(onSaved);
    g.cfg = Config{};
    g.accent = AccentColor();
    g.backgroundBrush = CreateSolidBrush(kBackground);
    g.cardBrush = CreateSolidBrush(kCardColor);
    std::vector<std::wstring> warnings;
    if (!LoadConfigFile(configPath, g.cfg, warnings)) LoadConfigText(DefaultConfigText(), g.cfg, warnings);

    POINT cursor;
    GetCursorPos(&cursor);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    g.wnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"ObjectFrame 設定", style, cursor.x, cursor.y, 100, 100,
                            nullptr, nullptr, instance, nullptr);
    if (!g.wnd) return;
    // Blend the title bar into the window background (Windows 11).
    COLORREF caption = kBackground;
    DwmSetWindowAttribute(g.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));

    CreateControls();
    ApplyFont();
    LayoutControls();
    PopulateControls();
    ShowPage(kGeneral);

    UINT dpi = GetDpiForWindow(g.wnd);
    RECT rc{0, 0, MulDiv(kClientW, dpi, 96), MulDiv(kClientH, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_CONTROLPARENT, dpi);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = mi.rcWork.left + (Width(mi.rcWork) - w) / 2;
    int y = mi.rcWork.top + (Height(mi.rcWork) - h) / 2;
    SetWindowPos(g.wnd, nullptr, x, y, w, h, SWP_NOZORDER);
    ShowWindow(g.wnd, SW_SHOW);
    ForceForeground(g.wnd);
}

void SettingsWindow::Close() {
    if (g.wnd) DestroyWindow(g.wnd);
}

HWND SettingsWindow::Handle() { return g.wnd; }

}  // namespace of
