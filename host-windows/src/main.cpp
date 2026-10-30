// sst-host: command-line host for testing; ScreenFleet.exe is the tray app.
#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include <vector>

#include "com_util.h"
#include "crypto.h"
#include "engine.h"
#include "net.h"

using namespace sst;

namespace {

std::atomic<bool> g_quit{false};

BOOL WINAPI OnConsoleCtrl(DWORD) {
    g_quit = true;
    return TRUE;
}

struct Args {
    bool list = false;
    bool discover = false;
    bool selfTest = false;
    std::string pairAddress;
    int dumpDisplay = 0;       // --dump: which display to encode
    std::vector<std::string> connectHosts;  // empty = USB via adb
    std::wstring adbPath;
    UINT fps = 60;
    UINT bitrateKbps = 8000;
    std::string dumpPath;
    UINT dumpWidth = 1280;
    UINT dumpHeight = 800;
    double seconds = 10;
    bool constantRate = false;
    int cycles = 0;  // leak test: restart the session this many times
    bool touchLog = false;  // log touches from the tablet instead of moving the pointer
};

void PrintUsage() {
    std::fprintf(stderr,
                 "Usage: sst-host [options]\n"
                 "By default the tablet becomes an extra monitor (needs the ScreenFleet display driver).\n"
                 "  --list                 list displays and exit\n"
                 "  --discover             list tablets on the Wi-Fi network for 6 seconds and exit\n"
                 "  --selftest             check the encryption building blocks against known answers\n"
                 "  --pair HOST[:PORT]     pair with a tablet over Wi-Fi (type the code it shows)\n"
                 "  --connect HOST[:PORT]  connect to the device over Wi-Fi; repeat for several\n"
                 "                         (default: every USB device through adb forward)\n"
                 "  --adb PATH             path to adb.exe\n"
                 "  --fps N                frame rate cap (default 60)\n"
                 "  --bitrate KBPS         video bitrate (default 8000)\n"
                 "  --touch-log            log touches from the tablet instead of acting on them\n"
                 "Testing without a device:\n"
                 "  --dump FILE.h264       encode a display to a file instead of streaming\n"
                 "  --size WxH             stream size for --dump (default 1280x800)\n"
                 "  --seconds N            --dump duration (default 10)\n"
                 "  --constant             encode every frame even if the screen is static\n");
}

bool ParseArgs(int argc, wchar_t** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + Narrow(arg));
            return argv[++i];
        };
        if (arg == L"--list") a.list = true;
        else if (arg == L"--discover") a.discover = true;
        else if (arg == L"--selftest") a.selfTest = true;
        else if (arg == L"--pair") a.pairAddress = Narrow(next());
        else if (arg == L"--dump-display") a.dumpDisplay = std::stoi(next());
        else if (arg == L"--connect") a.connectHosts.push_back(Narrow(next()));
        else if (arg == L"--adb") a.adbPath = next();
        else if (arg == L"--fps") a.fps = UINT(std::stoi(next()));
        else if (arg == L"--bitrate") a.bitrateKbps = UINT(std::stoi(next()));
        else if (arg == L"--dump") a.dumpPath = Narrow(next());
        else if (arg == L"--size") {
            std::wstring v = next();
            auto x = v.find(L'x');
            if (x == std::wstring::npos) throw std::runtime_error("--size expects WxH");
            a.dumpWidth = UINT(std::stoi(v.substr(0, x)));
            a.dumpHeight = UINT(std::stoi(v.substr(x + 1)));
        } else if (arg == L"--seconds") a.seconds = std::stod(next());
        else if (arg == L"--constant") a.constantRate = true;
        else if (arg == L"--touch-log") a.touchLog = true;
        else if (arg == L"--cycles") a.cycles = std::stoi(next());
        else if (arg == L"--help" || arg == L"-h") return false;
        else throw std::runtime_error("unknown option " + Narrow(arg));
    }
    if (a.fps < 1 || a.fps > 240) throw std::runtime_error("--fps must be 1-240");
    return true;
}

void RunDump(const Args& args) {
    auto outputs = PhysicalOutputs();
    if (outputs.empty()) throw std::runtime_error("no displays found");
    int index = args.dumpDisplay;
    if (index >= int(outputs.size())) throw std::runtime_error("no display with index " + std::to_string(index));
    auto target = OpenCapture(outputs[size_t(index)]);
    FileSink sink(args.dumpPath);
    if (!sink.Ok()) throw std::runtime_error("cannot open " + args.dumpPath);
    SessionConfig config;
    config.width = args.dumpWidth & ~1u;
    config.height = args.dumpHeight & ~1u;
    config.fps = args.fps;
    config.bitrate = args.bitrateKbps * 1000;
    config.constantRate = args.constantRate;
    config.maxSeconds = args.seconds;
    RunSession(target->gpu, target->capture, config, sink, g_quit);
    Log("wrote %s", args.dumpPath.c_str());
}

void RunDiscover() {
    Settings s;
    s.usb = false;
    s.wifi = true;
    StreamEngine engine;
    engine.Start(s, false, nullptr);
    for (int i = 0; i < 60 && !g_quit; ++i) Sleep(100);
    for (const auto& n : engine.Status().nearby) {
        std::printf("%-22s %-24s %ux%u  API %u  id %s%s%s\n", n.address.c_str(), n.name.c_str(), n.width, n.height,
                    n.sdk, n.id.c_str(), n.busy ? "  busy" : "", n.busy && !n.busyWith.empty() ? (" with " + n.busyWith).c_str() : "");
    }
    engine.Stop();
}

// Pairs with one tablet and adds it to this user's settings (the tray app's
// other settings stay as they are).
int RunPair(const std::string& address) {
    Settings s = LoadSettings();
    s.usb = false;
    s.wifi = true;
    s.wifiAddresses.clear();
    StreamEngine engine;
    engine.Start(s, false, nullptr);
    engine.Pair(address, "");
    PairStage last = PairStage::Connecting;
    for (int i = 0; i < 2400 && !g_quit; ++i) {
        PairingStatus p = engine.Status().pairing;
        if (p.stage != last) {
            last = p.stage;
            if (p.stage == PairStage::EnterCode) {
                std::printf("Type the code %s shows: ", p.name.c_str());
                std::fflush(stdout);
                char line[64] = {};
                if (!std::fgets(line, sizeof(line), stdin)) break;
                engine.SubmitPairCode(line);
            } else if (p.stage == PairStage::WaitTablet) {
                std::printf("Code accepted; tap Pair on the tablet.\n");
            } else if (p.stage == PairStage::Done || p.stage == PairStage::Failed) {
                std::printf("%s\n", p.message.c_str());
                break;
            }
        }
        Sleep(50);
    }
    bool ok = last == PairStage::Done;
    if (ok) {
        Settings paired = engine.GetSettings();
        Settings mine = LoadSettings();
        for (const auto& t : paired.tablets) {
            if (t.pairKey.empty()) continue;
            auto it = std::find_if(mine.tablets.begin(), mine.tablets.end(), [&](const KnownTablet& k) {
                return (!k.id.empty() && k.id == t.id) || (k.id.empty() && k.name == t.name);
            });
            if (it == mine.tablets.end()) mine.tablets.push_back(t);
            else *it = t;
        }
        if (std::find(mine.wifiAddresses.begin(), mine.wifiAddresses.end(), address) == mine.wifiAddresses.end())
            mine.wifiAddresses.push_back(address);
        if (mine.hostId.empty()) mine.hostId = paired.hostId;
        SaveSettings(mine);
    }
    engine.Stop();
    return ok ? 0 : 1;
}

void RunStream(const Args& args) {
    Settings s;
    // this PC's id and pairings, so paired tablets accept it over Wi-Fi (nothing is saved back)
    Settings saved = LoadSettings();
    s.hostId = saved.hostId;
    s.tablets = saved.tablets;
    s.usb = args.connectHosts.empty();
    s.wifi = !args.connectHosts.empty();
    s.wifiAddresses = args.connectHosts;
    s.fps = args.fps;
    s.bitrateMbps = std::max<UINT>(1, (args.bitrateKbps + 500) / 1000);

    StreamEngine engine;
    engine.Start(s, false, nullptr, args.adbPath);
    EngineStatus st = engine.Status();
    if (!st.driverInstalled)
        throw std::runtime_error("the ScreenFleet display driver isn't installed (see driver/README.md)");
    if (s.usb && !st.adbFound)
        throw std::runtime_error("adb.exe not found; install Android platform-tools or pass --adb PATH");
    if (args.cycles > 0) {
        // Leak test: restart the session repeatedly, recording resources
        // once each new session is streaming.
        for (int i = 0; i <= args.cycles && !g_quit; ++i) {
            for (int t = 0; t < 300 && engine.Status().state != EngineState::Streaming && !g_quit; ++t) Sleep(100);
            Sleep(1500);
            PROCESS_MEMORY_COUNTERS_EX mem{};
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
            DWORD handles = 0;
            GetProcessHandleCount(GetCurrentProcess(), &handles);
            DWORD threads = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == GetCurrentProcessId()) ++threads;
            CloseHandle(snap);
            std::printf("cycle %3d  state=%d  private=%6zu KB  handles=%5lu  threads=%3lu  gdi=%lu\n", i,
                        int(engine.Status().state), mem.PrivateUsage / 1024, handles, threads,
                        GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
            std::fflush(stdout);
            if (i == args.cycles) break;
            s.fps = s.fps == 60 ? 30 : 60;  // any change that restarts the session
            engine.SetSettings(s, true);
            // wait for the old session to end before waiting for the new one
            for (int t = 0; t < 100 && engine.Status().state == EngineState::Streaming && !g_quit; ++t) Sleep(50);
        }
    } else {
        while (!g_quit) Sleep(100);
    }
    engine.Stop();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Needed for DuplicateOutput1 and for correct sizes on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    // 1 ms timer resolution; the default 15.6 ms makes frame pacing miss slots.
    timeBeginPeriod(1);

    Args args;
    try {
        if (!ParseArgs(argc, argv, args)) {
            PrintUsage();
            return 0;
        }
        if (args.touchLog) SetInputMode(InputMode::Log);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n\n", e.what());
        PrintUsage();
        return 2;
    }

    try {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        Check(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET), "MFStartup");
        net::WinsockInit winsock;

        if (args.list) {
            for (const auto& o : EnumerateOutputs()) {
                std::printf("%d: %s %dx%d at (%ld,%ld)  \"%s\"  on %s\n", o.index, o.gdiName.c_str(), o.Width(),
                            o.Height(), o.desktopRect.left, o.desktopRect.top, o.monitorName.c_str(),
                            o.adapterName.c_str());
            }
        } else if (!args.pairAddress.empty()) {
            int code = RunPair(args.pairAddress);
            MFShutdown();
            return code;
        } else if (args.selfTest) {
            std::string report;
            bool ok = crypto::SelfTest(report);
            std::printf("%s", report.c_str());
            MFShutdown();
            return ok ? 0 : 1;
        } else if (args.discover) {
            RunDiscover();
        } else if (!args.dumpPath.empty()) {
            RunDump(args);
        } else {
            RunStream(args);
        }
    } catch (const std::exception& e) {
        Log("fatal: %s", e.what());
        MFShutdown();
        return 1;
    }
    MFShutdown();
    return 0;
}
