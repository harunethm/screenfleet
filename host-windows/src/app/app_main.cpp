// ScreenFleet.exe: the ScreenFleet tray app.
#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <timeapi.h>

#include "../engine.h"
#include "../log.h"
#include "../net.h"
#include "main_window.h"

using namespace sst;

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR cmdLine, _In_ int) {
    // One instance: a second launch just brings the window forward.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ScreenFleet.App");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"ScreenFleetWindow", nullptr))
            PostMessageW(other, app::MainWindow::kMsgShow, 0, 0);
        return 0;
    }
    bool startInTray = cmdLine && wcsstr(cmdLine, L"--tray") != nullptr;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    timeBeginPeriod(1);  // precise frame pacing
    int exitCode = 0;
    {
        net::WinsockInit winsock;
        StreamEngine engine;
        app::MainWindow window(engine);
        if (!window.Create(instance, !startInTray)) {
            MessageBoxW(nullptr, L"ScreenFleet couldn’t open its window.", L"ScreenFleet", MB_ICONERROR);
            return 1;
        }
        engine.Start(LoadSettings(), true, [&window] { window.PostStatusChanged(); });

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = int(msg.wParam);
        engine.Stop();  // removes the virtual monitor and says goodbye to the tablet
    }
    timeEndPeriod(1);
    MFShutdown();
    CoUninitialize();
    if (single) CloseHandle(single);
    return exitCode;
}
