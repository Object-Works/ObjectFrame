#include "config.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <utility>

namespace of {

namespace {

std::wstring StripComment(const std::wstring& line) {
    // '#' only starts a comment at line start or after whitespace, so regexes like ^#32770$ survive.
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L'#' && (i == 0 || iswspace(line[i - 1]))) return line.substr(0, i);
    }
    return line;
}

bool ParseBool(const std::wstring& v, bool& out) {
    auto l = ToLower(v);
    if (l == L"true" || l == L"yes" || l == L"on" || l == L"1") {
        out = true;
        return true;
    }
    if (l == L"false" || l == L"no" || l == L"off" || l == L"0") {
        out = false;
        return true;
    }
    return false;
}

bool ParseColor(const std::wstring& v, COLORREF& out) {
    std::wstring s = ToLower(Trim(v));
    if (s.rfind(L"0x", 0) == 0) s = s.substr(2);
    else if (s.rfind(L"rgb(", 0) == 0 && s.back() == L')') s = s.substr(4, s.size() - 5);
    if (s.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long rgb = wcstoul(s.c_str(), &end, 16);
    if (*end) return false;
    out = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return true;
}

// The first name for each key code is the canonical one used when writing configs.
const std::pair<const wchar_t*, UINT> kNamedKeys[] = {
    {L"return", VK_RETURN},     {L"enter", VK_RETURN},       {L"space", VK_SPACE},
    {L"tab", VK_TAB},           {L"escape", VK_ESCAPE},      {L"esc", VK_ESCAPE},
    {L"backspace", VK_BACK},    {L"delete", VK_DELETE},      {L"insert", VK_INSERT},
    {L"home", VK_HOME},         {L"end", VK_END},            {L"pageup", VK_PRIOR},
    {L"pagedown", VK_NEXT},     {L"left", VK_LEFT},          {L"right", VK_RIGHT},
    {L"up", VK_UP},             {L"down", VK_DOWN},          {L"comma", VK_OEM_COMMA},
    {L"period", VK_OEM_PERIOD}, {L"minus", VK_OEM_MINUS},    {L"equal", VK_OEM_PLUS},
    {L"plus", VK_OEM_PLUS},     {L"semicolon", VK_OEM_1},    {L"slash", VK_OEM_2},
    {L"grave", VK_OEM_3},       {L"bracketleft", VK_OEM_4},  {L"backslash", VK_OEM_5},
    {L"bracketright", VK_OEM_6}, {L"apostrophe", VK_OEM_7},  {L"print", VK_SNAPSHOT},
};

bool ParseBind(const std::wstring& value, Bind& b, std::wstring& err) {
    size_t c1 = value.find(L',');
    size_t c2 = c1 == std::wstring::npos ? c1 : value.find(L',', c1 + 1);
    if (c2 == std::wstring::npos) {
        err = L"bind needs: MODS, KEY, command[, args]";
        return false;
    }
    std::wstring mods = Trim(value.substr(0, c1));
    std::wstring key = Trim(value.substr(c1 + 1, c2 - c1 - 1));
    std::wstring rest = Trim(value.substr(c2 + 1));
    size_t c3 = rest.find(L',');
    std::wstring verb = Trim(c3 == std::wstring::npos ? rest : rest.substr(0, c3));
    std::wstring args = c3 == std::wstring::npos ? L"" : Trim(rest.substr(c3 + 1));
    if (verb.empty()) {
        err = L"bind is missing a command";
        return false;
    }
    if (!ParseModifiers(mods, b.mods)) {
        err = L"unknown modifier in '" + mods + L"'";
        return false;
    }
    if (!ParseKey(key, b.vk)) {
        err = L"unknown key '" + key + L"'";
        return false;
    }
    if ((b.mods & MOD_WIN) && b.vk == 'L') {
        err = L"SUPER+L is reserved by Windows (lock screen) and cannot be bound";
        return false;
    }
    b.command = args.empty() ? verb : verb + L" " + args;
    b.display = mods + L"+" + key;
    return true;
}

bool ParseWindowRule(const std::wstring& value, WindowRule& r, std::wstring& err) {
    size_t c = value.find(L',');
    if (c == std::wstring::npos) {
        err = L"windowrule needs: action, field:regex";
        return false;
    }
    auto action = ToLower(Trim(value.substr(0, c)));
    auto cond = Trim(value.substr(c + 1));
    if (action == L"float") r.action = WindowRule::Action::Float;
    else if (action == L"ignore") r.action = WindowRule::Action::Ignore;
    else if (action == L"tile") r.action = WindowRule::Action::Tile;
    else {
        err = L"unknown windowrule action '" + action + L"'";
        return false;
    }
    size_t colon = cond.find(L':');
    if (colon == std::wstring::npos) {
        err = L"windowrule condition must be class:/exe:/title:<regex>";
        return false;
    }
    auto field = ToLower(Trim(cond.substr(0, colon)));
    if (field == L"class") r.field = WindowRule::Field::Class;
    else if (field == L"exe") r.field = WindowRule::Field::Exe;
    else if (field == L"title") r.field = WindowRule::Field::Title;
    else {
        err = L"unknown windowrule field '" + field + L"'";
        return false;
    }
    r.pattern = Trim(cond.substr(colon + 1));
    try {
        r.re = std::wregex(r.pattern, std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error&) {
        err = L"invalid regex '" + r.pattern + L"'";
        return false;
    }
    return true;
}

std::wstring SubstituteVars(std::wstring s, const std::vector<std::pair<std::wstring, std::wstring>>& vars) {
    for (auto& [name, val] : vars) {
        size_t pos = 0;
        while ((pos = s.find(name, pos)) != std::wstring::npos) {
            s.replace(pos, name.size(), val);
            pos += val.size();
        }
    }
    return s;
}

}  // namespace

bool ParseModifiers(std::wstring s, UINT& mods) {
    mods = 0;
    for (auto& c : s) {
        if (c == L'+' || c == L'_' || c == L'|') c = L' ';
    }
    for (auto& tok : SplitWhitespace(s)) {
        auto t = ToLower(tok);
        if (t == L"alt") mods |= MOD_ALT;
        else if (t == L"shift") mods |= MOD_SHIFT;
        else if (t == L"ctrl" || t == L"control") mods |= MOD_CONTROL;
        else if (t == L"super" || t == L"win" || t == L"windows") mods |= MOD_WIN;
        else return false;
    }
    return true;
}

bool ParseKey(const std::wstring& s, UINT& vk) {
    auto t = ToLower(Trim(s));
    if (t.empty()) return false;
    if (t.size() == 1) {
        wchar_t c = t[0];
        if (c >= L'a' && c <= L'z') {
            vk = L'A' + (c - L'a');
            return true;
        }
        if (c >= L'0' && c <= L'9') {
            vk = c;
            return true;
        }
        switch (c) {
            case L'.': vk = VK_OEM_PERIOD; return true;
            case L'-': vk = VK_OEM_MINUS; return true;
            case L'=': vk = VK_OEM_PLUS; return true;
            case L';': vk = VK_OEM_1; return true;
            case L'/': vk = VK_OEM_2; return true;
            case L'`': vk = VK_OEM_3; return true;
            case L'[': vk = VK_OEM_4; return true;
            case L'\\': vk = VK_OEM_5; return true;
            case L']': vk = VK_OEM_6; return true;
            case L'\'': vk = VK_OEM_7; return true;
            default: return false;
        }
    }
    if (t[0] == L'f' && t.size() <= 3 && std::all_of(t.begin() + 1, t.end(), iswdigit)) {
        int n = _wtoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) {
            vk = VK_F1 + n - 1;
            return true;
        }
    }
    for (auto& [name, code] : kNamedKeys) {
        if (t == name) {
            vk = code;
            return true;
        }
    }
    return false;
}

bool ParseLayout(const std::wstring& s, LayoutKind& out) {
    auto l = ToLower(Trim(s));
    if (l == L"dwindle" || l == L"bsp") out = LayoutKind::Dwindle;
    else if (l == L"master" || l == L"masterstack" || l == L"tall") out = LayoutKind::MasterStack;
    else if (l == L"monocle" || l == L"full") out = LayoutKind::Monocle;
    else if (l == L"niri" || l == L"scrolling" || l == L"scroll") out = LayoutKind::Niri;
    else return false;
    return true;
}

const wchar_t* LayoutName(LayoutKind kind) {
    switch (kind) {
        case LayoutKind::Dwindle: return L"dwindle";
        case LayoutKind::MasterStack: return L"master";
        case LayoutKind::Monocle: return L"monocle";
        case LayoutKind::Niri: return L"scroll";
    }
    return L"?";
}

bool ParseSlideDirection(const std::wstring& s, SlideDirection& out) {
    auto l = ToLower(Trim(s));
    if (l == L"auto") out = SlideDirection::Auto;
    else if (l == L"up") out = SlideDirection::Up;
    else if (l == L"down") out = SlideDirection::Down;
    else if (l == L"left") out = SlideDirection::Left;
    else if (l == L"right") out = SlideDirection::Right;
    else return false;
    return true;
}

const wchar_t* SlideDirectionName(SlideDirection dir) {
    switch (dir) {
        case SlideDirection::Auto: return L"auto";
        case SlideDirection::Up: return L"up";
        case SlideDirection::Down: return L"down";
        case SlideDirection::Left: return L"left";
        case SlideDirection::Right: return L"right";
    }
    return L"auto";
}

std::wstring DefaultConfigPath() {
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(home) : L".";
    return base + L"\\.config\\ofwm\\ofwm.conf";
}

bool EnsureConfigFile(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::path p(path);
    if (std::filesystem::exists(p, ec)) return true;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out << WideToUtf8(DefaultConfigText());
    return true;
}

bool LoadConfigText(const std::wstring& text, Config& out, std::vector<std::wstring>& warnings) {
    Config cfg;
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    std::wistringstream in(text);
    std::wstring raw;
    int lineNo = 0;

    while (std::getline(in, raw)) {
        ++lineNo;
        auto warn = [&](const std::wstring& msg) {
            warnings.push_back(L"line " + std::to_wstring(lineNo) + L": " + msg);
        };
        std::wstring line = Trim(StripComment(raw));
        if (line.empty() || line.back() == L'{' || line == L"}") continue;

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            warn(L"expected 'key = value'");
            continue;
        }
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring value = Trim(line.substr(eq + 1));

        if (!key.empty() && key[0] == L'$') {
            value = SubstituteVars(value, vars);
            if (ToLower(key) == L"$mod" && !ParseModifiers(value, cfg.mainMod)) warn(L"invalid $mod '" + value + L"'");
            vars.emplace_back(key, value);
            std::sort(vars.begin(), vars.end(),
                      [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
            continue;
        }
        value = SubstituteVars(value, vars);
        auto k = ToLower(key);

        if (k == L"gaps_in") cfg.gapsIn = std::max(0, _wtoi(value.c_str()));
        else if (k == L"gaps_out") cfg.gapsOut = std::max(0, _wtoi(value.c_str()));
        else if (k == L"border") {
            if (!ParseBool(value, cfg.borders)) warn(L"invalid bool");
        } else if (k == L"border_active") {
            if (!ParseColor(value, cfg.borderActive)) warn(L"invalid color (use 0xRRGGBB)");
        } else if (k == L"border_inactive") {
            if (!ParseColor(value, cfg.borderInactive)) warn(L"invalid color (use 0xRRGGBB)");
        } else if (k == L"border_width") cfg.borderWidth = std::clamp(_wtoi(value.c_str()), 0, 30);
        else if (k == L"border_radius") cfg.borderRadius = std::clamp(_wtoi(value.c_str()), 0, 30);
        else if (k == L"border_corners") {
            auto l = ToLower(value);
            if (l == L"round") cfg.borderRadius = 8;
            else if (l == L"small") cfg.borderRadius = 4;
            else if (l == L"square") cfg.borderRadius = 0;
            else warn(L"border_corners must be round, small or square (prefer border_radius)");
        } else if (k == L"glow") {
            if (!ParseBool(value, cfg.glow)) warn(L"invalid bool");
        } else if (k == L"glow_size") cfg.glowSize = std::clamp(_wtoi(value.c_str()), 0, 60);
        else if (k == L"glow_intensity") cfg.glowIntensity = std::clamp(_wtoi(value.c_str()), 0, 100);
        else if (k == L"animate_move") {
            if (!ParseBool(value, cfg.animateMove)) warn(L"invalid bool");
        } else if (k == L"animate_workspace") {
            if (!ParseBool(value, cfg.animateWorkspace)) warn(L"invalid bool");
        } else if (k == L"animation_duration") cfg.animationDuration = std::clamp(_wtoi(value.c_str()), 50, 1000);
        else if (k == L"column_width" || k == L"niri_column_width") cfg.niriColumnWidth = std::clamp(_wtof(value.c_str()), 0.2, 1.0);
        else if (k == L"workspace_mode") {
            auto l = ToLower(value);
            if (l == L"linked") cfg.linkedWorkspaces = true;
            else if (l == L"independent") cfg.linkedWorkspaces = false;
            else warn(L"workspace_mode must be independent or linked");
        } else if (k == L"workspace_slide") {
            if (!ParseSlideDirection(value, cfg.workspaceSlide)) warn(L"workspace_slide must be auto, up, down, left or right");
        } else if (k == L"default_layout") {
            if (!ParseLayout(value, cfg.defaultLayout)) warn(L"unknown layout '" + value + L"'");
        } else if (k == L"master_ratio") cfg.masterRatio = std::clamp(_wtof(value.c_str()), 0.1, 0.9);
        else if (k == L"dwindle_ratio") cfg.dwindleRatio = std::clamp(_wtof(value.c_str()), 0.1, 0.9);
        else if (k == L"workspaces") cfg.workspaceCount = std::clamp(_wtoi(value.c_str()), 1, 20);
        else if (k == L"cursor_follows_focus") {
            if (!ParseBool(value, cfg.cursorFollowsFocus)) warn(L"invalid bool");
        } else if (k == L"focus_follows_mouse") {
            if (!ParseBool(value, cfg.focusFollowsMouse)) warn(L"invalid bool");
        } else if (k == L"exec-once" || k == L"exec_once") {
            if (!value.empty()) cfg.execOnce.push_back(value);
        } else if (k == L"bind") {
            Bind b;
            std::wstring err;
            if (ParseBind(value, b, err)) cfg.binds.push_back(std::move(b));
            else warn(err);
        } else if (k == L"windowrule") {
            WindowRule r;
            std::wstring err;
            if (ParseWindowRule(value, r, err)) cfg.rules.push_back(std::move(r));
            else warn(err);
        } else {
            warn(L"unknown key '" + key + L"'");
        }
    }

    out = std::move(cfg);
    return true;
}

bool LoadConfigFile(const std::wstring& path, Config& out, std::vector<std::wstring>& warnings) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }
    return LoadConfigText(Utf8ToWide(bytes), out, warnings);
}

std::wstring DefaultConfigText() {
    return LR"CFG(# ObjectFrame WM 設定ファイル
# 書式: key = value     '#' 以降はコメント (行頭 or 空白の直後のみ)
# 編集したら $mod SHIFT + R、または ofwm dispatch reload で再読み込みできます。

# SUPER = Windows キー。Win+L は OS のロック画面に予約されているため割り当てできません。
$mod = SUPER

# ---- 見た目 ----
gaps_in = 8               # ウィンドウ同士の隙間 (px)
gaps_out = 12             # 画面端との隙間 (px)
border = true             # フォーカス枠を表示
border_width = 2          # 枠の太さ (px)。0 = Windows 11 標準の 1px 枠に色を付けるだけ
border_radius = 8         # 角丸の半径 (px, 0-30)。枠を描くとウィンドウの角もこの半径で丸く見えます
border_active = 0x89b4fa
border_inactive = 0x313244

# ---- グロー (アクティブなウィンドウの枠の周りを光らせる) ----
glow = false
glow_size = 12            # 光の広がり (px)
glow_intensity = 60       # 明るさ (%)

# ---- アニメーション ----
animate_move = true       # タイル配置が変わったときにウィンドウを滑らかに移動
animate_workspace = true  # ワークスペース切替時にウィンドウをスライド
animation_duration = 200  # 長さ (ms, 50-1000)
workspace_slide = auto    # 次のワークスペースへ進むときの動き: auto | up | down | left | right
                          # auto は隣にモニターがない方向を選びます (左右は隣のモニターに映り込むことがあります)

# ---- レイアウト ----
default_layout = dwindle  # dwindle | master | monocle | scroll
master_ratio = 0.55
dwindle_ratio = 0.5
column_width = 0.5        # scroll の列の幅 (画面幅に対する割合)。$mod+, / . で変更
workspaces = 9            # モニターごとのワークスペース数 (再起動で反映)
# independent = モニターごとに独立 / linked = 全モニター連動
# linked ではモニター 1 2 3 に連続したワークスペースが並び、マウスのあるモニターを起点に
# 一斉に切り替わります (例: マウスがモニター2 にあるとき $mod+3 → 2 3 4)。最後まで行くと 1 に戻ります
workspace_mode = independent
cursor_follows_focus = false
# マウスが乗ったウィンドウをアクティブにする
focus_follows_mouse = false

# ---- 起動時に実行 ----
# exec-once = explorer.exe

# ---- ウィンドウルール ----
# windowrule = float|ignore|tile, class:|exe:|title:<正規表現>   (最初にマッチしたものが適用)
windowrule = float, class:^#32770$
windowrule = float, exe:^Taskmgr\.exe$
windowrule = float, title:^(Calculator|電卓)$
windowrule = float, title:^(Picture-in-Picture|ピクチャー イン ピクチャー)$
windowrule = float, class:^OperationStatusWindow$

# ---- キーバインド ----
# bind = MODS, KEY, command[, args]
# MODS: SUPER ALT CTRL SHIFT
# Windows 標準の Win+キー (Win+D, Win+E など) よりこちらのバインドが優先されます。

bind = $mod, RETURN, exec, wt.exe
bind = $mod, Q, close

bind = $mod, LEFT, focus, left
bind = $mod, DOWN, focus, down
bind = $mod, UP, focus, up
bind = $mod, RIGHT, focus, right

bind = $mod SHIFT, LEFT, move, left
bind = $mod SHIFT, DOWN, move, down
bind = $mod SHIFT, UP, move, up
bind = $mod SHIFT, RIGHT, move, right

bind = $mod CTRL, LEFT, focusmonitor, left
bind = $mod CTRL, RIGHT, focusmonitor, right
bind = $mod CTRL SHIFT, LEFT, movetomonitor, left
bind = $mod CTRL SHIFT, RIGHT, movetomonitor, right

bind = $mod, F, togglefloat
bind = $mod SHIFT, F, fullscreen
bind = $mod, N, layout, next
bind = $mod, D, layout, dwindle
bind = $mod, T, layout, master
bind = $mod, M, layout, monocle
bind = $mod, COMMA, resize, -0.05
bind = $mod, PERIOD, resize, +0.05
bind = $mod SHIFT, RETURN, promote
bind = $mod SHIFT, T, retile

bind = $mod SHIFT, P, pause
bind = $mod SHIFT, R, reload
bind = $mod SHIFT, E, quit

bind = $mod, 1, workspace, 1
bind = $mod, 2, workspace, 2
bind = $mod, 3, workspace, 3
bind = $mod, 4, workspace, 4
bind = $mod, 5, workspace, 5
bind = $mod, 6, workspace, 6
bind = $mod, 7, workspace, 7
bind = $mod, 8, workspace, 8
bind = $mod, 9, workspace, 9

bind = $mod SHIFT, 1, movetoworkspace, 1
bind = $mod SHIFT, 2, movetoworkspace, 2
bind = $mod SHIFT, 3, movetoworkspace, 3
bind = $mod SHIFT, 4, movetoworkspace, 4
bind = $mod SHIFT, 5, movetoworkspace, 5
bind = $mod SHIFT, 6, movetoworkspace, 6
bind = $mod SHIFT, 7, movetoworkspace, 7
bind = $mod SHIFT, 8, movetoworkspace, 8
bind = $mod SHIFT, 9, movetoworkspace, 9
)CFG";
}

}  // namespace of
