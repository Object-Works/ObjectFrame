#pragma once

#include "common.h"

#include <regex>
#include <string>
#include <vector>

namespace of {

enum class LayoutKind { Dwindle, MasterStack, Monocle, Niri };
// Which way windows move when switching to a higher-numbered workspace.
enum class SlideDirection { Auto, Up, Down, Left, Right };

bool ParseLayout(const std::wstring& s, LayoutKind& out);
const wchar_t* LayoutName(LayoutKind kind);
bool ParseSlideDirection(const std::wstring& s, SlideDirection& out);
const wchar_t* SlideDirectionName(SlideDirection dir);

struct Bind {
    UINT mods = 0;
    UINT vk = 0;
    std::wstring command;  // "verb args", same format as `ofwm dispatch`
    std::wstring display;  // human readable "ALT SHIFT+H"
};

struct WindowRule {
    enum class Action { Float, Ignore, Tile } action = Action::Float;
    enum class Field { Class, Exe, Title } field = Field::Class;
    std::wstring pattern;
    std::wregex re;
};

struct Config {
    UINT mainMod = MOD_WIN;  // value of `$mod`
    int gapsIn = 8;
    int gapsOut = 12;
    bool borders = true;
    int borderWidth = 2;  // 0 = native 1px DWM border only
    int borderRadius = 8;  // windows themselves can only round to 0, 4 or 8 px
    COLORREF borderActive = RGB(0x89, 0xb4, 0xfa);
    COLORREF borderInactive = RGB(0x31, 0x32, 0x44);
    bool glow = false;  // focused window only, in the active border color
    int glowSize = 12;
    int glowIntensity = 60;
    bool animateMove = true;
    bool animateWorkspace = true;
    int animationDuration = 200;  // ms
    SlideDirection workspaceSlide = SlideDirection::Auto;
    // Linked: monitors left to right always show consecutive workspaces (wrapping around),
    // anchored at the monitor under the mouse.
    bool linkedWorkspaces = false;
    double niriColumnWidth = 0.5;  // fraction of the monitor width
    LayoutKind defaultLayout = LayoutKind::Dwindle;
    double masterRatio = 0.55;
    double dwindleRatio = 0.5;
    int workspaceCount = 9;
    bool cursorFollowsFocus = false;
    bool focusFollowsMouse = false;
    std::vector<Bind> binds;
    std::vector<WindowRule> rules;
    std::vector<std::wstring> execOnce;
};

std::wstring DefaultConfigPath();
std::wstring DefaultConfigText();
bool EnsureConfigFile(const std::wstring& path);

bool LoadConfigText(const std::wstring& text, Config& out, std::vector<std::wstring>& warnings);
bool LoadConfigFile(const std::wstring& path, Config& out, std::vector<std::wstring>& warnings);
std::wstring SerializeConfig(const Config& config);
bool SaveConfigFile(const std::wstring& path, const Config& config);

bool ParseModifiers(std::wstring s, UINT& mods);
bool ParseKey(const std::wstring& s, UINT& vk);
std::wstring ModsToString(UINT mods);  // "SUPER SHIFT"
std::wstring KeyToString(UINT vk);     // "H", "RETURN", "F5"
std::vector<std::wstring> AllKeyNames();
const wchar_t* RuleActionName(WindowRule::Action action);
const wchar_t* RuleFieldName(WindowRule::Field field);
bool MakeWindowRule(WindowRule::Action action, WindowRule::Field field, const std::wstring& pattern, WindowRule& out);

}  // namespace of
