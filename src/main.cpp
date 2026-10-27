#include "common.h"
#include "config.h"
#include "version.h"
#include "window_manager.h"

#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

void PrintToParentConsole(const std::wstring& text) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    std::wstring line = text + L"\n";
    WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &written, nullptr);
    FreeConsole();
}

int SendCommand(std::wstring cmd) {
    HWND wm = FindWindowW(of::kWindowClass, nullptr);
    if (!wm) {
        PrintToParentConsole(L"ofwm: no running instance found");
        return 1;
    }
    COPYDATASTRUCT cds{};
    cds.dwData = of::kCopyDataMagic;
    cds.cbData = (DWORD)((cmd.size() + 1) * sizeof(wchar_t));
    cds.lpData = cmd.data();
    DWORD_PTR result = 0;
    SendMessageTimeoutW(wm, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 2000, &result);
    return result ? 0 : 1;
}

// `ofwm dispatch <command...>` forwards a command to the running instance.
int SendDispatch(int argc, wchar_t** argv) {
    std::wstring cmd;
    for (int i = 2; i < argc; ++i) {
        if (!cmd.empty()) cmd += L' ';
        cmd += argv[i];
    }
    if (cmd.empty()) {
        PrintToParentConsole(L"usage: ofwm dispatch <command> [args]   e.g. ofwm dispatch workspace 2");
        return 2;
    }
    return SendCommand(cmd);
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    of::WindowManager::Instance().RestoreAllWindows();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::wstring configPath = of::DefaultConfigPath();
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"dispatch" || a == L"msg") return SendDispatch(argc, argv);
        if (a == L"settings") {
            PrintToParentConsole(
                L"ObjectFrame has no settings window. Edit ofwm.conf, then run: ofwm dispatch reload");
            return 0;
        }
        if ((a == L"--config" || a == L"-c") && i + 1 < argc) configPath = argv[++i];
        else if (a == L"--version" || a == L"-v") {
            PrintToParentConsole(L"ObjectFrame " OFWM_VERSION_WSTRING);
            return 0;
        } else if (a == L"--help" || a == L"-h") {
            PrintToParentConsole(
                L"ObjectFrame " OFWM_VERSION_WSTRING L" - tiling window manager for Windows\n"
                L"  ofwm [--config <path>]          start the window manager\n"
                L"  ofwm dispatch <command> [args]  send a command to the running instance\n"
                L"                                  edit ofwm.conf, then: ofwm dispatch reload\n"
                L"  ofwm --version                  print the version");
            return 0;
        }
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ObjectFrameWM");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"ObjectFrame is already running.", L"ObjectFrame", MB_ICONINFORMATION);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    of::EnsureConfigFile(configPath);
    of::InitLog(std::filesystem::path(configPath).parent_path().wstring() + L"\\ofwm.log");
    SetUnhandledExceptionFilter(CrashHandler);

    auto& wm = of::WindowManager::Instance();
    if (!wm.Init(instance, configPath)) {
        MessageBoxW(nullptr, L"Failed to initialize ObjectFrame.", L"ObjectFrame", MB_ICONERROR);
        return 1;
    }
    int code = wm.Run();

    LocalFree(argv);
    if (mutex) CloseHandle(mutex);
    return code;
}
