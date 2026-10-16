// Pimax Eye Tracking Monitor -- https://github.com/jcooray/PimaxEyeTrackingMonitor (MIT licence, see LICENSE)
//
// A small always-on-top window showing, layer by layer, whether eye tracking is getting from a Pimax headset's eye tracker
// to SteamVR, and restarting it automatically when Pimax's runtime gives up on the tracker.
//
//   1. Pimax runtime  -- pi_server's connection to the Tobii eye tracker, read from its log
//                        (%LOCALAPPDATA%\Pimax\runtime\pvr_srv_log_*.txt): connected / lost / gave up, eyes detected,
//                        tracker rate. Pimax restarts the Tobii service on SteamVR start and on eye tracking toggles,
//                        and can lose the tracker mid-session; it then gives up after 6 reconnect attempts.
//   2. Headset driver -- CustomHeadsetOpenVR's %APPDATA%\CustomHeadset\diagnostic.json (4x/s): valid + gaze angles.
//   3. SteamVR        -- IVRSystem::GetEyeTrackedFoveationCenter, as games and foveated rendering mods read it, polled from our own
//                        background OpenVR connection (never starts SteamVR). SteamVR keeps returning the last gaze as
//                        valid when the tracker stops, so a gaze that hasn't changed for 1 s counts as frozen.
//
// Events (state changes) are listed in the window and appended to eye_tracking_monitor_log.txt next to the exe.
// Keys: T toggles always-on-top, S toggles a sound when eye tracking is lost, R (or the button) restarts eye tracking.
// Taskbar: the button is filled green (OK), yellow (headset off / just reconnected), red (lost) or animated (restarting),
// with a coloured dot on the icon; hovering it shows a "Restart eye tracking" button in the preview.
//
// In the headset (2026-10-09): once SteamVR runs, the monitor connects as a SteamVR overlay application (never starting
// SteamVR itself): a dashboard tab "Eye tracking" with the status and a restart button (click it with the laser), and a
// small badge in view while the gaze is frozen with the headset worn or a restart is running. "Worn" is the eye tracker
// seeing eyes (see WornForVerdict). Frozen while worn = red; frozen with the headset off = yellow.
// Automatic recovery (A toggles, on by default since 2026-10-10): when pi_server logs "tobi api failed" (it has given up
// on the tracker and never retries -- after a lost race when Pimax Play restarts the Tobii runtime, e.g. at SteamVR start)
// and Pimax's launcher has been quiet for 12 s: restart eye tracking, up to 3 times 20 s apart; never if the switch is
// off in Pimax Play. See AutoRecover.
//
// Restart eye tracking: what Pimax Play's "Eye tracking" switch does, off then on -- PiPlayService.dll (Pimax's own
// client library, which Pimax Play loads through FFI): PiServiceInit(), then PiService_SetIntConfig(
// "piplay_enable_eye_tracking", 0), ~5 s, 1. The headset is told to stop and restart its eye tracker, and pi_server
// reconnects to the Tobii runtime -- the known workaround for the runtime giving up after a SteamVR start.
#include <windows.h>
#include <shobjidl.h>   // ITaskbarList3: taskbar button colour, overlay dot, thumbnail button
#include <openvr.h>   // Valve's OpenVR SDK header (BSD-3-Clause, third_party/openvr); only its types are used -- SteamVR's
                      // own openvr_api.dll is loaded at run time, so nothing from Valve ships with the exe

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <functional>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Common

double NowS() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

std::wstring Clock() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[16];
    swprintf_s(b, L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring EnvPath(const wchar_t* var, const wchar_t* rest) {
    wchar_t b[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(var, b, MAX_PATH)) return L"";
    return std::wstring(b) + rest;
}

std::wstring ExeDir() {
    wchar_t b[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring p(b);
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : p.substr(0, k);
}

// Events: shown in the window (latest first) and appended to the log file.
std::deque<std::wstring> g_events;
bool g_soundOnLoss = false;

void Event(const std::wstring& when, const std::wstring& what) {
    const std::wstring line = when + L"  " + what;
    g_events.push_front(line);
    while (g_events.size() > 14) g_events.pop_back();
    std::ofstream f(ExeDir() + L"\\eye_tracking_monitor_log.txt", std::ios::app);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char d[16];
    sprintf_s(d, "%04u-%02u-%02u ", t.wYear, t.wMonth, t.wDay);
    f << d << Narrow(line) << "\n";
}

// ---------------------------------------------------------------------------------------------------------------
// Layer 1: Pimax runtime log

enum class PimaxState { Unknown, Connected, Lost, Reconnected, GaveUp, Stopped };

struct Pimax {
    std::wstring file;          // newest pvr_srv_log
    uint64_t offset = 0;
    std::string partial;
    PimaxState state = PimaxState::Unknown;
    std::wstring since;         // time of the last state change (from the log)
    int eyes = -1;              // user eye detected: -1 unknown, 0, 1
    double fps = -1;
    std::wstring fpsAt;
    double lastRead = -10;
} g_pimax;

const wchar_t* PimaxText(PimaxState s) {
    switch (s) {
        case PimaxState::Connected: return L"eye tracker connected";
        case PimaxState::Lost: return L"eye tracker connection LOST, reconnecting";
        case PimaxState::Reconnected: return L"eye tracker reconnected (no gaze seen since -- wear the headset to confirm)";
        case PimaxState::GaveUp: return L"eye tracker LOST -- Pimax gave up reconnecting";
        case PimaxState::Stopped: return L"Pimax service stopped";
        default: return L"no eye tracker news in the log yet";
    }
}

std::wstring NewestPimaxLog() {
    const std::wstring dir = EnvPath(L"LOCALAPPDATA", L"\\Pimax\\runtime\\");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"pvr_srv_log_*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;
    FILETIME bestTime{};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) { bestTime = fd.ftLastWriteTime; best = fd.cFileName; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? L"" : dir + best;
}

void PimaxSet(PimaxState s, const std::wstring& when, bool live) {
    if (s == g_pimax.state) return;
    g_pimax.state = s;
    g_pimax.since = when;
    if (live) Event(when, std::wstring(L"Pimax: ") + PimaxText(s));
}

// One log line: "[26-09-27 21:19:06.118][22096][err][PSRV] message".
void PimaxLine(const std::string& line, bool live) {
    std::wstring when = L"--:--:--";
    if (line.size() > 19 && line[0] == '[') when = Widen(line.substr(10, 8));
    if (line.find("Eye calibration applied") != std::string::npos) {
        PimaxSet(PimaxState::Connected, when, live);
    } else if (line.find("tobii_device_process_callbacks failed") != std::string::npos) {
        PimaxSet(PimaxState::Lost, when, live);
    } else if (line.find("tobi api failed") != std::string::npos || line.find("tobii_calibration_apply failed") != std::string::npos) {
        PimaxSet(PimaxState::GaveUp, when, live);
    } else if (line.find("Connected to platform module") != std::string::npos
               && (g_pimax.state == PimaxState::Lost || g_pimax.state == PimaxState::GaveUp)) {
        // Also after "gave up": a restart (ours or Pimax Play's) reconnects it then -- 2026-10-11 the monitor missed that and
        // restarted a reconnected tracker again (no "Eye calibration applied" after a clean Pimax Play install).
        PimaxSet(PimaxState::Reconnected, when, live);   // gaze frames or a calibration mark it connected again
    } else if (line.find("receive quit event to exit") != std::string::npos) {
        PimaxSet(PimaxState::Stopped, when, live);
    } else if (const size_t k = line.find("User eye detected info changed to "); k != std::string::npos) {
        const int e = line[k + 34] == '1' ? 1 : 0;
        if (e != g_pimax.eyes && live) Event(when, e ? L"Pimax: eyes detected (headset on)" : L"Pimax: eyes NOT detected (headset off?)");
        g_pimax.eyes = e;
    } else if (const size_t g = line.find("tobii_gaze: "); g != std::string::npos) {
        g_pimax.fps = atof(line.c_str() + g + 12);
        g_pimax.fpsAt = when;
        if (g_pimax.fps > 1.0 && g_pimax.state != PimaxState::Connected) PimaxSet(PimaxState::Connected, when, live);
    }
}

void PimaxPoll() {
    const double now = NowS();
    if (now - g_pimax.lastRead < 0.5) return;
    g_pimax.lastRead = now;
    const std::wstring newest = NewestPimaxLog();
    if (newest.empty()) return;
    const bool first = g_pimax.file.empty();
    if (newest != g_pimax.file) {
        if (!first) Event(Clock(), L"Pimax: service (re)started, new log");
        g_pimax.file = newest;
        g_pimax.offset = 0;
        g_pimax.partial.clear();
        g_pimax.state = PimaxState::Unknown;
        g_pimax.eyes = -1;
        g_pimax.fps = -1;
    }
    HANDLE h = CreateFileW(g_pimax.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const bool live = g_pimax.offset > 0 || !first;   // the backlog read at startup sets the state without events
    if (static_cast<uint64_t>(size.QuadPart) > g_pimax.offset) {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(g_pimax.offset);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        std::vector<char> buf(static_cast<size_t>(size.QuadPart - static_cast<LONGLONG>(g_pimax.offset)));
        DWORD got = 0;
        ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
        g_pimax.offset += got;
        g_pimax.partial.append(buf.data(), got);
        size_t start = 0, nl;
        while ((nl = g_pimax.partial.find('\n', start)) != std::string::npos) {
            PimaxLine(g_pimax.partial.substr(start, nl - start), live);
            start = nl + 1;
        }
        g_pimax.partial.erase(0, start);
        if (!live) Event(Clock(), std::wstring(L"Pimax (from its log): ") + PimaxText(g_pimax.state) +
                                  (g_pimax.since.empty() ? L"" : L" since " + g_pimax.since));
    }
    CloseHandle(h);
}

// ---------------------------------------------------------------------------------------------------------------
// Pimax Play's "mounted" flag, as its service logs it -- %LOCALAPPDATA%\Pimax\PiService\Log\PiService__<date>-<hour>.log,
// "[SCClient::SetMounted] set OSC Check(mounted) : true|false" on every change. Only a fallback for "worn": Pimax confirmed
// (2026-10) that the Dream Air has no proximity sensor and this flag is about OSC availability, and it can stay stuck after
// a Pimax service restart. The eye tracker seeing eyes is what counts (WornForVerdict).

struct Mount {
    int mounted = -1;           // -1 unknown, 0 off, 1 on
    std::wstring at;            // time of the last change (log)
    std::wstring file;
    uint64_t offset = 0;
    std::string partial;
    double lastRead = 0;
    bool started = false;
} g_mount;

std::vector<std::wstring> PiServiceLogs() {   // newest first (names sort by date and hour)
    const std::wstring dir = EnvPath(L"LOCALAPPDATA", L"\\Pimax\\PiService\\Log\\");
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"PiService__*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do v.push_back(dir + fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b) { return a > b; });
    return v;
}

// The mounted state in a chunk of log text (the last change wins); -1 if none. `when` gets its time.
int MountIn(const std::string& text, std::wstring* when) {
    static const char kKey[] = "[SCClient::SetMounted] set OSC Check(mounted) : ";
    int st = -1;
    for (size_t k = text.find(kKey); k != std::string::npos; k = text.find(kKey, k + 1)) {
        const size_t v = k + sizeof(kKey) - 1;
        st = text.compare(v, 4, "true") == 0 ? 1 : 0;
        const size_t ls = text.rfind('\n', k);
        const size_t b = ls == std::string::npos ? 0 : ls + 1;
        if (when && text.size() > b + 19) *when = Widen(text.substr(b + 11, 8));   // "2026-10-09 16:21:13.089"
    }
    return st;
}

std::string ReadFrom(const std::wstring& path, uint64_t from, uint64_t* end) {
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *end = from; return out; }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (static_cast<uint64_t>(size.QuadPart) > from) {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(from);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        out.resize(static_cast<size_t>(size.QuadPart - static_cast<LONGLONG>(from)));
        DWORD got = 0;
        ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
        out.resize(got);
    }
    *end = from + out.size();
    CloseHandle(h);
    return out;
}

void MountPoll() {
    const double now = NowS();
    if (now - g_mount.lastRead < 0.5) return;
    g_mount.lastRead = now;
    const std::vector<std::wstring> logs = PiServiceLogs();
    if (logs.empty()) return;
    if (!g_mount.started) {   // the latest state: newest log back, a few hours at most
        g_mount.started = true;
        for (size_t i = 0; i < logs.size() && i < 6 && g_mount.mounted < 0; ++i) {
            uint64_t end = 0;
            g_mount.mounted = MountIn(ReadFrom(logs[i], 0, &end), &g_mount.at);
        }
        g_mount.file = logs[0];
        ReadFrom(g_mount.file, 0, &g_mount.offset);
        Event(Clock(), g_mount.mounted < 0 ? std::wstring(L"Pimax 'mounted' flag: no reading in Pimax Play's log yet")
                                           : std::wstring(L"Pimax 'mounted' flag: ") + (g_mount.mounted ? L"on" : L"off") + L" since " + g_mount.at);
        return;
    }
    if (logs[0] != g_mount.file) { g_mount.file = logs[0]; g_mount.offset = 0; g_mount.partial.clear(); }   // next hour's log
    g_mount.partial += ReadFrom(g_mount.file, g_mount.offset, &g_mount.offset);
    const size_t lastNl = g_mount.partial.rfind('\n');
    if (lastNl == std::string::npos) return;
    std::wstring when;
    const int st = MountIn(g_mount.partial.substr(0, lastNl + 1), &when);
    g_mount.partial.erase(0, lastNl + 1);
    if (st >= 0 && st != g_mount.mounted) {
        g_mount.mounted = st;
        g_mount.at = when;
        Event(when, st ? L"Pimax 'mounted' flag: on" : L"Pimax 'mounted' flag: off");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Layer 2: headset driver diagnostics

struct Driver {
    bool present = false, updating = false, valid = false;
    bool empty = false;              // valid, but the gaze has been exactly (0, 0) for over 2 s
    double nonZeroAt = 0;
    double lx = 0, ly = 0, rx = 0, ry = 0;
    double lastRead = -10;
    int lastValid = -1;
} g_driver;

double JsonNumberAfter(const std::string& s, size_t from, const char* key, size_t* at) {
    const size_t k = s.find(key, from);
    if (k == std::string::npos) { *at = std::string::npos; return 0; }
    *at = k + strlen(key);
    return atof(s.c_str() + *at);
}

void DriverPoll() {
    const double now = NowS();
    if (now - g_driver.lastRead < 0.25) return;
    g_driver.lastRead = now;
    const std::wstring path = EnvPath(L"APPDATA", L"\\CustomHeadset\\diagnostic.json");
    WIN32_FILE_ATTRIBUTE_DATA a;
    g_driver.present = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) != 0;
    if (!g_driver.present) { g_driver.updating = false; return; }
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const auto u64 = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    g_driver.updating = u64(nowFt) - u64(a.ftLastWriteTime) < 2ull * 10000000ull;   // written within 2 s
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    const size_t v = s.find("\"valid\":");
    if (v == std::string::npos) return;   // mid-write
    const size_t t = s.find_first_not_of(" \t", v + 8);
    g_driver.valid = t != std::string::npos && s.compare(t, 4, "true") == 0;
    size_t at = 0;
    g_driver.lx = JsonNumberAfter(s, 0, "\"angleX\":", &at);
    g_driver.ly = JsonNumberAfter(s, at == std::string::npos ? 0 : at, "\"angleY\":", &at);
    g_driver.rx = JsonNumberAfter(s, at == std::string::npos ? 0 : at, "\"angleX\":", &at);
    g_driver.ry = JsonNumberAfter(s, at == std::string::npos ? 0 : at, "\"angleY\":", &at);
    if (g_driver.lx != 0 || g_driver.ly != 0 || g_driver.rx != 0 || g_driver.ry != 0 || !g_driver.valid) g_driver.nonZeroAt = now;
    // Only meaningful with eyes detected: with nobody wearing the headset the gaze is (0, 0) by design.
    const bool empty = g_driver.valid && g_pimax.eyes == 1 && now - g_driver.nonZeroAt > 2.0;
    if (empty != g_driver.empty) {
        g_driver.empty = empty;
        Event(Clock(), empty ? L"Driver: 'valid' but the gaze is EMPTY (0, 0) -- Pimax sends no real data"
                             : L"Driver: gaze has real values again");
    }
    const int valid = g_driver.updating && g_driver.valid && !empty ? 1 : 0;
    if (valid != g_driver.lastValid) {
        if (g_driver.lastValid != -1 || !valid)
            Event(Clock(), valid ? L"Driver: receiving gaze from Pimax" : L"Driver: NOT receiving gaze");
        g_driver.lastValid = valid;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Layer 3: SteamVR (OpenVR, background application)

using InitInternal2Fn = uint32_t(__cdecl*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using ShutdownInternalFn = void(__cdecl*)();
using GetGenericInterfaceFn = void*(__cdecl*)(const char*, vr::EVRInitError*);

enum class VrState { NotRunning, NoGaze, Frozen, Live };

struct SteamVr {
    HMODULE api = nullptr;
    InitInternal2Fn init = nullptr;
    ShutdownInternalFn shutdown = nullptr;
    GetGenericInterfaceFn get = nullptr;
    vr::IVRSystem* system = nullptr;
    vr::IVROverlay* overlay = nullptr;   // only when connected as an overlay application
    vr::VROverlayHandle_t dash = 0, thumb = 0, badge = 0;
    bool worn = false, wornKnown = false;
    double wornChangedAt = 0;
    int proximity = -1;          // the headset reports a proximity sensor: 1 yes, 0 no, -1 not asked yet
    double lastMotionAt = -1e9;  // last time the headset moved like a head (no proximity sensor)
    double nextTry = 0;
    std::wstring error;
    vr::HmdVector2_t ndc[2] = {};
    bool valid = false;
    double changedAt = 0;
    std::deque<double> changes;     // times of gaze changes in the last second
    VrState state = VrState::NotRunning;
    bool reported = false;
} g_vr;

bool LoadOpenVrApi() {
    if (g_vr.api) return true;
    const std::wstring vrpath = EnvPath(L"LOCALAPPDATA", L"\\openvr\\openvrpaths.vrpath");
    std::ifstream f(vrpath);
    if (!f) { g_vr.error = L"SteamVR isn't installed (no openvrpaths.vrpath)"; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    size_t k = s.find("\"runtime\"");
    if (k == std::string::npos || (k = s.find('[', k)) == std::string::npos || (k = s.find('"', k)) == std::string::npos) {
        g_vr.error = L"no SteamVR runtime registered";
        return false;
    }
    std::string path;
    for (size_t i = k + 1; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        path += s[i];
    }
    g_vr.api = LoadLibraryExW((Widen(path) + L"\\bin\\win64\\openvr_api.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_vr.api) { g_vr.error = L"couldn't load SteamVR's openvr_api.dll"; return false; }
    g_vr.init = reinterpret_cast<InitInternal2Fn>(GetProcAddress(g_vr.api, "VR_InitInternal2"));
    g_vr.shutdown = reinterpret_cast<ShutdownInternalFn>(GetProcAddress(g_vr.api, "VR_ShutdownInternal"));
    g_vr.get = reinterpret_cast<GetGenericInterfaceFn>(GetProcAddress(g_vr.api, "VR_GetGenericInterface"));
    return g_vr.init && g_vr.shutdown && g_vr.get;
}

std::wstring Fmt2(const wchar_t* fmt, int v) {
    wchar_t b[300];
    swprintf_s(b, fmt, v);
    return b;
}

void VrSetState(VrState s) {
    if (s == g_vr.state && g_vr.reported) return;
    const bool wasLive = g_vr.state == VrState::Live && g_vr.reported;
    g_vr.state = s;
    g_vr.reported = true;
    const wchar_t* text = s == VrState::Live ? L"SteamVR: gaze LIVE"
                        : s == VrState::Frozen ? L"SteamVR: gaze FROZEN (tracker stopped updating)"
                        : s == VrState::NoGaze ? L"SteamVR: running, NO gaze"
                        : L"SteamVR: not running";
    Event(Clock(), text);
    if (wasLive && g_soundOnLoss) MessageBeep(MB_ICONHAND);
}

void VrPoll() {
    const double now = NowS();
    if (!g_vr.system) {
        if (now < g_vr.nextTry) return;
        g_vr.nextTry = now + 3.0;
        if (!LoadOpenVrApi()) { VrSetState(VrState::NotRunning); return; }
        vr::EVRInitError err = vr::VRInitError_None;
        // A background connection first: it fails when SteamVR isn't running (and never starts it). Then, SteamVR being
        // up, reconnect as an overlay application for the dashboard tab and the badge (fall back to background).
        g_vr.init(&err, vr::VRApplication_Background, nullptr);
        if (err != vr::VRInitError_None) {
            g_vr.error = L"SteamVR not running (OpenVR error " + std::to_wstring(static_cast<int>(err)) + L")";
            VrSetState(VrState::NotRunning);
            return;
        }
        g_vr.shutdown();
        g_vr.init(&err, vr::VRApplication_Overlay, nullptr);
        if (err == vr::VRInitError_None) {
            g_vr.system = static_cast<vr::IVRSystem*>(g_vr.get(vr::IVRSystem_Version, &err));
            g_vr.overlay = static_cast<vr::IVROverlay*>(g_vr.get(vr::IVROverlay_Version, &err));
            if (!g_vr.overlay) Event(Clock(), L"SteamVR: no overlay interface -- status only on the desktop");
        } else {
            Event(Clock(), L"SteamVR: overlay connection refused (OpenVR error " + std::to_wstring(static_cast<int>(err)) + L") -- background only");
            err = vr::VRInitError_None;
            g_vr.init(&err, vr::VRApplication_Background, nullptr);
            if (err == vr::VRInitError_None) g_vr.system = static_cast<vr::IVRSystem*>(g_vr.get(vr::IVRSystem_Version, &err));
        }
        if (err != vr::VRInitError_None || !g_vr.system) {
            g_vr.system = nullptr;
            g_vr.overlay = nullptr;
            g_vr.error = L"SteamVR not running (OpenVR error " + std::to_wstring(static_cast<int>(err)) + L")";
            VrSetState(VrState::NotRunning);
            return;
        }
        g_vr.dash = g_vr.thumb = g_vr.badge = 0;
        g_vr.wornKnown = false;
        g_vr.proximity = -1;
        g_vr.error.clear();
    }
    vr::VREvent_t ev;
    while (g_vr.system->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_Quit) {
            g_vr.system = nullptr;
            g_vr.overlay = nullptr;
            g_vr.dash = g_vr.thumb = g_vr.badge = 0;
            g_vr.wornKnown = false;
            g_vr.shutdown();
            g_vr.nextTry = now + 5.0;
            VrSetState(VrState::NotRunning);
            return;
        }
    }
    vr::HmdVector2_t ndc[2] = {};
    g_vr.valid = g_vr.system->GetEyeTrackedFoveationCenter(&ndc[0], &ndc[1]);
    if (!g_vr.valid || memcmp(ndc, g_vr.ndc, sizeof ndc) != 0) {
        if (g_vr.valid) g_vr.changes.push_back(now);
        memcpy(g_vr.ndc, ndc, sizeof ndc);
        g_vr.changedAt = now;
    }
    while (!g_vr.changes.empty() && now - g_vr.changes.front() > 1.0) g_vr.changes.pop_front();
    VrSetState(!g_vr.valid ? VrState::NoGaze : now - g_vr.changedAt > 1.0 ? VrState::Frozen : VrState::Live);
    // Worn. With a proximity sensor: SteamVR's activity level. Without one (2026-10-09: the activity level said "worn"
    // with the headset on the desk, so every post-reconnect freeze looked like a fault and auto-restart kept restarting
    // the tracker): a head never holds perfectly still -- moving (over ~0.6 deg/s or 4 mm/s) in the last 3 s = worn.
    // Where SteamVR's proximity comes from: CustomHeadsetOpenVR's dreamAir.proximitySensorType (settings.json; source
    // Config.h: 0 none, 1 hardware -- the Pimax runtime's HmdMounted --, 2 stationary dimming = from movement (default),
    // 3 always on, 4 always off). Only 1 is a real sensor; re-read every 10 s (the driver applies it live).
    static double nextProxRead = 0;
    if (now >= nextProxRead) {
        nextProxRead = now + 10.0;
        int type = 2;
        std::ifstream sf(EnvPath(L"APPDATA", L"\\CustomHeadset\\settings.json"));
        if (sf) {
            std::stringstream ss;
            ss << sf.rdbuf();
            const std::string js = ss.str();
            const size_t da = js.find("\"dreamAir\"");
            const size_t pk = da == std::string::npos ? std::string::npos : js.find("\"proximitySensorType\"", da);
            const size_t next = da == std::string::npos ? std::string::npos : js.find("\"dreamAirSE\"", da);
            if (pk != std::string::npos && (next == std::string::npos || pk < next)) {
                const size_t c = js.find(':', pk);
                if (c != std::string::npos) type = atoi(js.c_str() + c + 1);
            }
        }
        const int hw = type == 1 ? 1 : 0;
        if (hw != g_vr.proximity) {
            g_vr.proximity = hw;
            Event(Clock(), hw ? L"Headset: proximity from the hardware sensor (CustomHeadset proximitySensorType 1) -- 'worn' from SteamVR"
                              : Fmt2(L"Headset: CustomHeadset proximitySensorType %d is not the hardware sensor -- 'worn' judged from head rotation", type));
        }
    }
    // Both, whatever the headset reports: the Dream Air claims a proximity sensor but SteamVR said "worn" with it on the
    // desk (17:40), so head movement is required as well.
    {
        vr::TrackedDevicePose_t pose{};
        g_vr.system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
        if (pose.bPoseIsValid) {
            const vr::HmdVector3_t& w = pose.vAngularVelocity;
            const float ang = std::sqrt(w.v[0] * w.v[0] + w.v[1] * w.v[1] + w.v[2] * w.v[2]);
            // Rotation only (2026-10-09 calibration: on the desk 0.3-1.3 deg/s, worn and sitting still 2.2-4.3 deg/s;
            // linear jitter was larger on the desk than on the head).
            if (ang > 0.0314f) g_vr.lastMotionAt = now;   // 1.8 deg/s
        }
    }
    const bool moving = now - g_vr.lastMotionAt < 3.0;
    // Pimax Play's mounted flag when it has a reading; else SteamVR's hardware proximity; else movement.
    const bool worn = g_mount.mounted >= 0 ? g_mount.mounted == 1
        : g_vr.proximity == 1 ? g_vr.system->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd) == vr::k_EDeviceActivityLevel_UserInteraction
        : moving;
    if (!g_vr.wornKnown) {
        g_vr.wornKnown = true;
        g_vr.worn = worn;
        g_vr.wornChangedAt = now;
        Event(Clock(), worn ? L"Headset: worn" : L"Headset: not worn");
    } else if (worn != g_vr.worn) {
        if (now - g_vr.wornChangedAt > 1.0) {
            g_vr.worn = worn;
            Event(Clock(), worn ? L"Headset: worn" : L"Headset: not worn");
        }
    } else {
        g_vr.wornChangedAt = now;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Window

bool g_topmost = true;
HFONT g_big = nullptr, g_font = nullptr, g_small = nullptr;

struct Verdict { COLORREF color; std::wstring text; };

// Automatic recovery (2026-10-10; replaces the frozen-gaze auto-restart, which guessed from the gaze and "worn" and once
// restarted a tracker that was still starting). Pimax Play restarts the Tobii runtime 3 s after every "HMD first
// connected" -- SteamVR starting the headset included -- and on this PC the new runtime can't open the tracker about 30 %
// of the time on the author's PC (ACCESS_DENIED, the old instance still holding it); pi_server then logs "tobi api failed" and never tries
// again. That line is the trigger: nothing is starting any more, so a restart can't break anything. Waits until Pimax's
// launcher has been quiet (no restart of its own in progress), leaves eye tracking alone if its switch is off in Pimax
// Play, and tries up to 3 times, 20 s apart (each is a new roll of the same race). A toggles it; on by default.
struct AutoRecover {
    bool on = true;
    double frozenSince = 0;      // gaze frozen with the headset worn since (the 15 s red delay)
    double gaveUpAt = 0;         // when pi_server's "gave up" state was first seen (0: not in it)
    double lastAttempt = -1e9;
    int attempts = 0;
    bool exhausted = false;      // kAutoAttempts restarts didn't bring it back
    bool userOff = false;        // the eye tracking switch is off in Pimax Play: not ours to turn on
} g_auto;
constexpr int kAutoAttempts = 3;
constexpr double kAutoSpacingS = 20.0, kAutoSettleS = 5.0, kLauncherQuietS = 12.0;
bool g_restartAuto = false;      // the running restart is an automatic one (it then respects the switch being off)

// Red only after 15 s: putting the headset on, the tracker takes a moment to deliver gaze; a real freeze lasts.
bool FrozenWhileWornLong() {
    return g_auto.frozenSince > 0 && NowS() - g_auto.frozenSince >= 15.0;
}

// Worn, for the verdict: the eye tracker seeing eyes (pi_server's "User eye detected info changed to 0/1") whenever Pimax
// reports it. 2026-10-10: Pimax's proximity sensor stayed "mounted" from 16:01:33 on (no more SetMounted lines after a
// Pimax service restart -- the proximity ticket), so with the headset on the desk the gaze counted as "frozen while
// worn": yellow for 15 s, then red, every time it was taken off. Without an eye reading yet: the proximity sensor /
// SteamVR / head movement, as before.
bool WornForVerdict() {
    if (g_pimax.eyes >= 0) return g_pimax.eyes == 1;
    return g_vr.wornKnown && g_vr.worn;
}

bool FrozenWhileWorn() {
    return g_vr.system && WornForVerdict() && (g_vr.state == VrState::Frozen || g_vr.state == VrState::NoGaze);
}

// Green: gaze reaching SteamVR. Red: Pimax's service stopped, Pimax lost / gave up on the tracker (worn or not), or the
// tracker sees eyes but SteamVR's gaze has been frozen for 15 s. Yellow: headset off, waiting for the first gaze,
// eye tracking switched off in Pimax Play. Grey: SteamVR not running (and nothing wrong on Pimax's side).
Verdict Judge() {
    if (g_vr.state == VrState::Live) return { RGB(30, 140, 60), L"EYE TRACKING OK -- gaze is reaching SteamVR" };
    if (g_pimax.state == PimaxState::Stopped)
        return { RGB(190, 40, 40), L"Pimax service stopped: start Pimax Play" };
    if (g_pimax.state == PimaxState::GaveUp && g_auto.userOff)
        return { RGB(200, 130, 0), L"Eye tracking is switched off in Pimax Play" };
    if (g_pimax.state == PimaxState::GaveUp) {
        if (g_auto.exhausted)
            return { RGB(190, 40, 40), L"Pimax lost the eye tracker -- automatic recovery failed 3 times: Dashboard > Eye tracking > Restart" };
        if (g_auto.on) return { RGB(190, 40, 40), L"Pimax lost the eye tracker (" + g_pimax.since + L") -- recovering automatically" };
        return { RGB(190, 40, 40), L"Pimax lost the eye tracker (" + g_pimax.since + L"): press Restart eye tracking" };
    }
    if (g_pimax.state == PimaxState::Lost)
        return { RGB(190, 40, 40), L"Pimax lost the eye tracker (" + g_pimax.since + L") -- it's trying to reconnect" };
    if (FrozenWhileWorn() && !FrozenWhileWornLong())
        return { RGB(200, 130, 0), L"Waiting for the eye tracker (no gaze yet with the headset on)" };
    if (FrozenWhileWorn())
        return { RGB(190, 40, 40), L"Eye tracking FROZEN with the headset on: SteamVR dashboard > Eye tracking > Restart" };
    if (g_vr.system && !WornForVerdict()) return { RGB(200, 130, 0), L"Headset not worn -- eye tracking idle" };
    if (g_pimax.state == PimaxState::Reconnected && g_pimax.eyes != 1)
        return { RGB(200, 130, 0), L"Pimax reconnected the eye tracker -- put the headset on to confirm gaze" };
    if (g_pimax.eyes != 1) return { RGB(200, 130, 0), L"Eyes not detected -- headset off?" };
    if (g_vr.state == VrState::NotRunning) return { RGB(110, 110, 110), L"SteamVR not running" };
    if (!(g_driver.updating && g_driver.valid) || g_driver.empty)
        return { RGB(190, 40, 40), L"The headset driver isn't getting gaze from Pimax: press Restart eye tracking" };
    return { RGB(190, 40, 40), L"SteamVR isn't passing the gaze on" };
}

void Text(HDC dc, int x, int y, HFONT f, COLORREF c, const std::wstring& s) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t b[256];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(b, fmt, ap);
    va_end(ap);
    return b;
}

void Paint(HWND wnd, HDC target) {
    RECT rc;
    GetClientRect(wnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 30));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    const COLORREF fg = RGB(225, 228, 232), dim = RGB(140, 146, 155), good = RGB(90, 200, 120), bad = RGB(240, 90, 90),
                   warn = RGB(240, 180, 60);

    // Verdict banner
    const Verdict v = Judge();
    RECT banner{ 0, 0, rc.right, 44 };
    HBRUSH vb = CreateSolidBrush(v.color);
    FillRect(dc, &banner, vb);
    DeleteObject(vb);
    Text(dc, 12, 12, g_font, RGB(255, 255, 255), v.text);

    int y = 58;
    // 1. Pimax
    Text(dc, 12, y, g_big, fg, L"1  Pimax runtime (eye tracker)");
    y += 24;
    const COLORREF pc = g_pimax.state == PimaxState::Connected ? good : g_pimax.state == PimaxState::Unknown ? dim : bad;
    Text(dc, 30, y, g_font, pc, std::wstring(PimaxText(g_pimax.state)) + (g_pimax.since.empty() ? L"" : L"  (since " + g_pimax.since + L")"));
    y += 20;
    std::wstring eyes = g_pimax.eyes < 0 ? L"eyes: unknown" : g_pimax.eyes ? L"eyes: detected" : L"eyes: NOT detected";
    std::wstring fps = g_pimax.fps < 0 ? L"tracker rate: --" : Fmt(L"tracker rate: %.0f Hz (at %s)", g_pimax.fps, g_pimax.fpsAt.c_str());
    Text(dc, 30, y, g_font, g_pimax.eyes == 0 ? warn : fg, eyes + L"    " + fps);
    y += 30;

    // 2. Driver
    Text(dc, 12, y, g_big, fg, L"2  Headset driver (CustomHeadsetOpenVR)");
    y += 24;
    if (!g_driver.present) Text(dc, 30, y, g_font, dim, L"no diagnostic.json -- driver not installed?");
    else if (!g_driver.updating) Text(dc, 30, y, g_font, dim, L"not updating -- SteamVR / driver not running");
    else if (g_driver.empty) Text(dc, 30, y, g_font, bad, L"reports 'valid' but the gaze is EMPTY (0, 0) -- no real data from Pimax");
    else Text(dc, 30, y, g_font, g_driver.valid ? good : bad,
              g_driver.valid ? Fmt(L"receiving gaze   L (%+.2f, %+.2f)  R (%+.2f, %+.2f) rad", g_driver.lx, g_driver.ly, g_driver.rx, g_driver.ry)
                             : std::wstring(L"NOT receiving gaze from Pimax"));
    y += 30;

    // 3. SteamVR
    Text(dc, 12, y, g_big, fg, L"3  SteamVR (what games read)");
    y += 24;
    const COLORREF vc = g_vr.state == VrState::Live ? good : g_vr.state == VrState::NotRunning ? dim : bad;
    std::wstring vs = g_vr.state == VrState::Live ? Fmt(L"gaze LIVE, %d updates/s (polled at ~60 Hz)", static_cast<int>(g_vr.changes.size()))
                    : g_vr.state == VrState::Frozen ? Fmt(L"gaze FROZEN for %.0f s", NowS() - g_vr.changedAt)
                    : g_vr.state == VrState::NoGaze ? std::wstring(L"running, NO gaze")
                    : (g_vr.error.empty() ? std::wstring(L"not running") : g_vr.error);
    Text(dc, 30, y, g_font, vc, vs);
    y += 24;

    // Gaze plot: both eyes' foveation centres in NDC.
    const int box = 150, bx = 30, by = y;
    HPEN frame = CreatePen(PS_SOLID, 1, RGB(80, 86, 96));
    HGDIOBJ oldPen = SelectObject(dc, frame);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, bx, by, bx + box, by + box);
    MoveToEx(dc, bx + box / 2, by, nullptr); LineTo(dc, bx + box / 2, by + box);
    MoveToEx(dc, bx, by + box / 2, nullptr); LineTo(dc, bx + box, by + box / 2);
    SelectObject(dc, oldPen);
    DeleteObject(frame);
    if (g_vr.state == VrState::Live || g_vr.state == VrState::Frozen) {
        const COLORREF col[2] = { RGB(90, 170, 255), RGB(255, 120, 200) };
        for (int e = 0; e < 2; ++e) {
            const int px = bx + static_cast<int>((g_vr.ndc[e].v[0] + 1.0f) * 0.5f * box);
            const int py = by + static_cast<int>((1.0f - g_vr.ndc[e].v[1]) * 0.5f * box);
            HBRUSH b = CreateSolidBrush(g_vr.state == VrState::Live ? col[e] : dim);
            SelectObject(dc, b);
            SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, px - 6, py - 6, px + 6, py + 6);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            DeleteObject(b);
        }
    }
    SelectObject(dc, oldBrush);
    Text(dc, bx + box + 16, by, g_small, dim, L"blue: left eye");
    Text(dc, bx + box + 16, by + 16, g_small, dim, L"pink: right eye");
    Text(dc, bx + box + 16, by + 32, g_small, dim, L"(each eye's view, centre = straight ahead)");
    if (g_vr.state == VrState::Live || g_vr.state == VrState::Frozen)
        Text(dc, bx + box + 16, by + 56, g_small, dim, Fmt(L"L (%+.2f, %+.2f)  R (%+.2f, %+.2f)", g_vr.ndc[0].v[0], g_vr.ndc[0].v[1],
                                                           g_vr.ndc[1].v[0], g_vr.ndc[1].v[1]));
    y = by + box + 16;

    // Events
    Text(dc, 12, y, g_big, fg, L"Events");
    y += 22;
    for (const std::wstring& e : g_events) {
        Text(dc, 30, y, g_small, fg, e);
        y += 17;
    }
    Text(dc, 12, rc.bottom - 22, g_small, dim,
         std::wstring(L"T: always on top ") + (g_topmost ? L"(on)" : L"(off)") + L"    S: sound when lost " +
         (g_soundOnLoss ? L"(on)" : L"(off)") + L"    A: auto-recovery " + (g_auto.on ? L"(on)" : L"(off)") + L"    R: restart");

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------------------------------------------
// Restart eye tracking (the Pimax Play switch, off then on)

constexpr UINT kMsgRestartEvent = WM_APP + 1;   // lParam: new std::wstring (event text)
constexpr UINT kMsgRestartDone = WM_APP + 2;
constexpr int kRestartButtonId = 101;
HWND g_restartButton = nullptr;
bool g_restarting = false;

DWORD WINAPI RestartThread(LPVOID param) {
    HWND wnd = static_cast<HWND>(param);
    auto say = [wnd](const std::wstring& s) { PostMessageW(wnd, kMsgRestartEvent, 0, reinterpret_cast<LPARAM>(new std::wstring(s))); };
    using Init_t = int(__cdecl*)();
    using SetInt_t = int(__cdecl*)(char*, int);
    using GetInt_t = int(__cdecl*)(char*, int*);
    static HMODULE dll = nullptr;
    static bool inited = false;
    if (!dll) {
        const std::wstring path = EnvPath(L"ProgramFiles", L"\\Pimax\\Runtime\\PiPlayService.dll");
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) {
            say(L"restart: could not load " + path + Fmt(L" (error %lu)", GetLastError()));
            PostMessageW(wnd, kMsgRestartDone, 0, 0);
            return 0;
        }
    }
    const auto init = reinterpret_cast<Init_t>(GetProcAddress(dll, "PiServiceInit"));
    const auto setInt = reinterpret_cast<SetInt_t>(GetProcAddress(dll, "PiService_SetIntConfig"));
    const auto getInt = reinterpret_cast<GetInt_t>(GetProcAddress(dll, "PiService_GetIntConfig"));
    if (!init || !setInt || !getInt) {
        say(L"restart: PiPlayService.dll doesn't have the expected functions (Pimax update?)");
        PostMessageW(wnd, kMsgRestartDone, 0, 0);
        return 0;
    }
    if (!inited) {
        int r = -1;
        for (int i = 0; i < 3 && r != 0; ++i) { r = init(); if (r != 0) Sleep(500); }
        if (r != 0) {
            say(Fmt(L"restart: PiServiceInit failed (%d) -- is Pimax Play's service running?", r));
            PostMessageW(wnd, kMsgRestartDone, 0, 0);
            return 0;
        }
        inited = true;
    }
    char key[] = "piplay_enable_eye_tracking";
    int was = -1;
    getInt(key, &was);
    if (g_restartAuto && was == 0) {   // switched off in Pimax Play: that's why pi_server let go -- leave it
        say(L"automatic recovery: eye tracking is switched off in Pimax Play -- leaving it off");
        PostMessageW(wnd, kMsgRestartDone, 1, 0);
        return 0;
    }
    say(Fmt(L"restart: eye tracking was %s -- switching off", was == 1 ? L"on" : was == 0 ? L"off" : L"unknown"));
    int r = setInt(key, 0);
    if (r != 0) say(Fmt(L"restart: switching off returned %d", r));
    Sleep(5000);
    r = setInt(key, 1);
    int now = -1;
    getInt(key, &now);
    say(r == 0 && now == 1 ? std::wstring(L"restart: eye tracking switched back on")
                           : Fmt(L"restart: switching on returned %d (now %d)", r, now));
    // And, as Pimax Play does whenever the switch goes on (launchEyeService): have the Pimax service launcher start the
    // Tobii runtime again -- the switch alone only power-cycles the tracker's USB device; pi_server, which had given up
    // on the runtime, doesn't retry (2026-10-07 20:23: USB sounds, no reconnect). Dream Air SE would also pass its K10
    // config file; the Dream Air (K11) takes none.
    const std::wstring launcher = EnvPath(L"ProgramFiles", L"\\Pimax\\Runtime\\launcher.exe");
    std::wstring cmd = L"\"" + launcher + L"\" PiServiceLauncher sendCommand active_tobii_runtime";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    const std::wstring dir = EnvPath(L"ProgramFiles", L"\\Pimax\\Runtime");
    if (CreateProcessW(launcher.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        say(Fmt(L"restart: asked the Pimax service to start the Tobii runtime (launcher exit %lu) -- reconnects in a few seconds", code));
    } else {
        say(Fmt(L"restart: could not run launcher.exe (error %lu)", GetLastError()));
    }
    PostMessageW(wnd, kMsgRestartDone, 0, 0);
    return 0;
}

void StartRestart(HWND wnd, bool automatic = false) {
    if (g_restarting) return;
    g_restarting = true;
    g_restartAuto = automatic;
    if (!automatic) g_auto.userOff = false;   // a restart by hand turns the switch back on anyway
    if (g_restartButton) EnableWindow(g_restartButton, FALSE);
    Event(Clock(), L"restarting eye tracking (Pimax eye tracking switch off, 5 s, on)");
    HANDLE t = CreateThread(nullptr, 0, RestartThread, wnd, 0, nullptr);
    if (t) CloseHandle(t);
    else { g_restarting = false; if (g_restartButton) EnableWindow(g_restartButton, TRUE); }
}

void PlaceButton(HWND wnd) {
    RECT rc;
    GetClientRect(wnd, &rc);
    if (g_restartButton) MoveWindow(g_restartButton, rc.right - 200, 56, 186, 30, TRUE);
}

// ---------------------------------------------------------------------------------------------------------------
// Taskbar: colour of the (pinned) button and a restart button in its hover preview

constexpr int kThumbRestartId = 201;
ITaskbarList3* g_taskbar = nullptr;
UINT g_msgTaskbarCreated = 0;
int g_tbLevel = -1;   // 0 grey, 1 green, 2 yellow, 3 red, 4 restarting
HICON g_dot[4] = {};

// A 16x16 filled circle (alpha-blended edge) for the taskbar overlay.
HICON MakeDot(COLORREF c) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 16;
    bi.bmiHeader.biHeight = -16;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP colour = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!colour || !bits) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const double d = std::sqrt((x - 7.5) * (x - 7.5) + (y - 7.5) * (y - 7.5));
            double a = 7.5 - d;
            a = a < 0 ? 0 : (a > 1 ? 1 : a);
            const bool rim = d > 5.8;   // dark rim so the dot reads on any taskbar colour
            const uint32_t r = rim ? GetRValue(c) / 3 : GetRValue(c), g = rim ? GetGValue(c) / 3 : GetGValue(c), b = rim ? GetBValue(c) / 3 : GetBValue(c);
            px[y * 16 + x] = (static_cast<uint32_t>(a * 255.0) << 24) | (r << 16) | (g << 8) | b;
        }
    }
    HBITMAP mask = CreateBitmap(16, 16, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, colour };
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(colour);
    return h;
}

void TaskbarInit(HWND wnd) {
    if (!g_taskbar && FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_taskbar)))) return;
    if (FAILED(g_taskbar->HrInit())) { g_taskbar->Release(); g_taskbar = nullptr; return; }
    if (!g_dot[0]) {
        g_dot[1] = MakeDot(RGB(40, 200, 80));
        g_dot[2] = MakeDot(RGB(240, 180, 30));
        g_dot[3] = MakeDot(RGB(230, 50, 50));
    }
    THUMBBUTTON b{};
    b.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    b.iId = kThumbRestartId;
    b.hIcon = g_dot[3];
    wcscpy_s(b.szTip, L"Restart eye tracking");
    b.dwFlags = THBF_ENABLED;
    g_taskbar->ThumbBarAddButtons(wnd, 1, &b);
    g_tbLevel = -1;   // re-apply the colour
}

void TaskbarUpdate(HWND wnd, const Verdict& v) {
    if (!g_taskbar) return;
    int level;
    if (g_restarting) level = 4;
    else if (v.color == RGB(30, 140, 60)) level = 1;
    else if (v.color == RGB(200, 130, 0)) level = 2;
    else if (v.color == RGB(110, 110, 110)) level = 0;
    else level = 3;
    if (level == g_tbLevel) return;
    g_tbLevel = level;
    static const TBPFLAG kState[5] = { TBPF_NOPROGRESS, TBPF_NORMAL, TBPF_PAUSED, TBPF_ERROR, TBPF_INDETERMINATE };
    g_taskbar->SetProgressState(wnd, kState[level]);
    if (level >= 1 && level <= 3) g_taskbar->SetProgressValue(wnd, 100, 100);
    static const wchar_t* kDesc[5] = { L"SteamVR not running", L"Eye tracking OK", L"Eye tracking: check the headset", L"Eye tracking lost",
                                       L"Restarting eye tracking" };
    g_taskbar->SetOverlayIcon(wnd, level >= 1 && level <= 3 ? g_dot[level] : nullptr, kDesc[level]);
    SetWindowTextW(wnd, (std::wstring(L"Eye tracking monitor -- ") + kDesc[level]).c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// Automatic recovery and the SteamVR overlays

// Seconds since Pimax's launcher last wrote its log (%LOCALAPPDATA%\Pimax\PiService\launcher_<date>_<hour>.log): it logs
// every command it passes on, e.g. Pimax Play's active_tobii_runtime -- a restart in progress. Large if there's no log.
double LauncherIdleS() {
    const std::wstring dir = EnvPath(L"LOCALAPPDATA", L"\\Pimax\\PiService\\");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"launcher_*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 1e9;
    FILETIME newest{};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) newest = fd.ftLastWriteTime;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const ULONGLONG a = (static_cast<ULONGLONG>(newest.dwHighDateTime) << 32) | newest.dwLowDateTime;
    const ULONGLONG b = (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
    return b > a ? static_cast<double>(b - a) / 1e7 : 0.0;
}

void AutoRestartTick(HWND wnd) {
    const double now = NowS();
    if (FrozenWhileWorn()) {
        if (g_auto.frozenSince == 0) g_auto.frozenSince = now;
    } else {
        g_auto.frozenSince = 0;
    }
    if (g_pimax.state != PimaxState::GaveUp) {
        g_auto.gaveUpAt = 0;
        if (g_pimax.state == PimaxState::Connected && (g_auto.attempts || g_auto.exhausted || g_auto.userOff)) {
            if (g_auto.attempts) Event(Clock(), Fmt(L"automatic recovery: the eye tracker is back (after %d restart%s)", g_auto.attempts, g_auto.attempts == 1 ? L"" : L"s"));
            g_auto.attempts = 0;
            g_auto.exhausted = false;
            g_auto.userOff = false;
        }
        return;
    }
    if (g_restarting) return;   // a restart switches eye tracking off for 5 s, and pi_server gives up on that too
    if (g_auto.gaveUpAt == 0) g_auto.gaveUpAt = now;
    if (!g_auto.on || g_auto.userOff || g_auto.exhausted) return;
    if (now - g_auto.gaveUpAt < kAutoSettleS || now - g_auto.lastAttempt < kAutoSpacingS) return;
    if (LauncherIdleS() < kLauncherQuietS) return;   // Pimax Play (or someone) just sent the launcher a command
    if (g_auto.attempts >= kAutoAttempts) {
        g_auto.exhausted = true;
        Event(Clock(), Fmt(L"automatic recovery: %d restarts didn't bring the eye tracker back -- stopped (Restart by hand, or restart Pimax Play)", kAutoAttempts));
        MessageBeep(MB_ICONHAND);
        return;
    }
    ++g_auto.attempts;
    g_auto.lastAttempt = now;
    Event(Clock(), Fmt(L"automatic recovery: Pimax gave up on the eye tracker (%s) -- restarting eye tracking (attempt %d of %d)",
                       g_pimax.since.c_str(), g_auto.attempts, kAutoAttempts));
    StartRestart(wnd, true);
}

// Draws into a 32-bit top-down DIB and returns RGBA for SetOverlayRaw. Pixels left in the key colour are transparent.
constexpr COLORREF kKey = RGB(255, 0, 255);
// `scale` draws the same layout into a smaller bitmap (w, h become the scaled size): SetOverlayRaw takes at most about
// 1 MB, and the 900x360 panel (1.3 MB) left the dashboard tab empty (2026-10-09).
std::vector<uint8_t> RenderRgba(int& w, int& h, const std::function<void(HDC)>& draw, float scale = 1.0f) {
    w = static_cast<int>(w * scale + 0.5f);
    h = static_cast<int>(h * scale + 0.5f);
    std::vector<uint8_t> out;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { DeleteDC(dc); return out; }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT all{ 0, 0, w, h };
    HBRUSH key = CreateSolidBrush(kKey);
    FillRect(dc, &all, key);
    DeleteObject(key);
    SetBkMode(dc, TRANSPARENT);
    if (scale != 1.0f) {
        SetGraphicsMode(dc, GM_ADVANCED);
        const XFORM xf{ scale, 0.0f, 0.0f, scale, 0.0f, 0.0f };
        SetWorldTransform(dc, &xf);
    }
    draw(dc);
    GdiFlush();
    out.resize(static_cast<size_t>(w) * h * 4);
    const auto* px = static_cast<const uint8_t*>(bits);   // B G R x
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const uint8_t b = px[i * 4], g = px[i * 4 + 1], r = px[i * 4 + 2];
        const bool transparent = r == 255 && g == 0 && b == 255;
        out[i * 4] = r; out[i * 4 + 1] = g; out[i * 4 + 2] = b; out[i * 4 + 3] = transparent ? 0 : 255;
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

void FillRound(HDC dc, const RECT& r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void DrawTextIn(HDC dc, HFONT f, COLORREF c, RECT r, const std::wstring& s, UINT flags) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, flags);
    SelectObject(dc, of);
}

constexpr int kPanelW = 900, kPanelH = 360, kBadgeW = 640, kBadgeH = 110;
HFONT g_vrBig = nullptr, g_vrFont = nullptr, g_vrSmall = nullptr;

// Overlay images (SetOverlayRaw). Each call takes one of ~200 shared memory blocks, which come back only as the
// compositor shows the image: re-sent 4x a second with nothing showing them (no game running) they ran out after ~67 s
// and every later image was refused -- the dashboard tab went empty (2026-10-09, vrclient_<exe name>.txt
// "Refusing to create memory block because 201 blocks are already outstanding"). So an image is sent only when it
// changed, and the panel and tab icon only while the dashboard is open. (D3D11 textures instead were tried the same
// day: with the monitor holding a D3D11 device the game crashed at startup in the NVIDIA driver, every launch.)
struct OverlayImage {
    vr::VROverlayHandle_t handle = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> last;
};
OverlayImage g_images[4];

void OverlayImagesReset() {   // new overlay handles (SteamVR (re)connected): send everything again
    for (OverlayImage& im : g_images) im = OverlayImage{};
}

void SetRaw(vr::VROverlayHandle_t h, std::vector<uint8_t>& px, int w, int ht) {
    if (!h || px.empty()) return;
    OverlayImage* im = nullptr;
    for (OverlayImage& s : g_images) if (s.handle == h) im = &s;
    if (!im) for (OverlayImage& s : g_images) if (!s.handle) { im = &s; im->handle = h; break; }
    if (!im) return;
    if (im->w == w && im->h == ht && im->last == px) return;   // unchanged: SteamVR still has it
    const vr::EVROverlayError e = g_vr.overlay->SetOverlayRaw(h, px.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(ht), 4);
    static bool reported = false;
    if (e != vr::VROverlayError_None) {
        if (!reported) {
            reported = true;
            Event(Clock(), Fmt(L"SteamVR: an overlay image (%dx%d) was refused, error %d", w, ht, static_cast<int>(e)));
        }
        return;   // not kept as sent: tried again next time
    }
    im->w = w;
    im->h = ht;
    im->last = px;
}

void OverlayTick(HWND wnd, const Verdict& v) {
    if (!g_vr.overlay) return;
    vr::IVROverlay* o = g_vr.overlay;
    if (!g_vr.dash) {
        if (o->CreateDashboardOverlay("jcooray.pimaxeyetrackingmonitor", "Eye tracking", &g_vr.dash, &g_vr.thumb) != vr::VROverlayError_None) {
            g_vr.dash = g_vr.thumb = 0;
            Event(Clock(), L"SteamVR: couldn't create the dashboard tab");
            g_vr.overlay = nullptr;
            return;
        }
        o->SetOverlayWidthInMeters(g_vr.dash, 2.0f);
        o->SetOverlayInputMethod(g_vr.dash, vr::VROverlayInputMethod_Mouse);
        const vr::HmdVector2_t scale{ { static_cast<float>(kPanelW), static_cast<float>(kPanelH) } };
        o->SetOverlayMouseScale(g_vr.dash, &scale);
        if (o->CreateOverlay("jcooray.pimaxeyetrackingmonitor.badge", "Eye tracking alert", &g_vr.badge) == vr::VROverlayError_None) {
            o->SetOverlayWidthInMeters(g_vr.badge, 0.32f);
            o->SetOverlaySortOrder(g_vr.badge, 200);
            vr::HmdMatrix34_t m{};   // 1 m ahead, a little below the view centre
            m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
            m.m[1][3] = -0.22f;
            m.m[2][3] = -1.0f;
            o->SetOverlayTransformTrackedDeviceRelative(g_vr.badge, vr::k_unTrackedDeviceIndex_Hmd, &m);
        } else {
            g_vr.badge = 0;
        }
        OverlayImagesReset();
        Event(Clock(), L"SteamVR: dashboard tab \"Eye tracking\" and the in-view badge ready");
    }
    // Clicks on the dashboard panel: the right-hand part is the restart button (x only -- no y-axis convention to trust).
    vr::VREvent_t ev;
    while (o->PollNextOverlayEvent(g_vr.dash, &ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_MouseButtonUp && ev.data.mouse.x > kPanelW * 0.62f) {
            Event(Clock(), L"restart requested from the SteamVR dashboard");
            StartRestart(wnd);
        }
    }
    static double next = 0;
    static COLORREF lastThumb = 0xFFFFFFFF;
    const double now = NowS();
    if (now < next) return;
    next = now + 0.25;
    if (v.color != lastThumb && o->IsDashboardVisible()) {   // the icon is only seen with the dashboard open
        lastThumb = v.color;
        int tw = 64, th = 64;
        std::vector<uint8_t> t = RenderRgba(tw, th, [&](HDC dc) {
            RECT r{ 6, 6, 58, 58 };
            HBRUSH b = CreateSolidBrush(g_restarting ? RGB(60, 120, 230) : v.color);
            HGDIOBJ ob = SelectObject(dc, b);
            SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, ob);
            DeleteObject(b);
        });
        SetRaw(g_vr.thumb, t, tw, th);
    }
    if (o->IsDashboardVisible() && o->IsOverlayVisible(g_vr.dash)) {
        int pw = kPanelW, ph = kPanelH;   // drawn at 0.8: 720x288, 0.83 MB (the mouse keeps the 900x360 layout's scale)
        std::vector<uint8_t> px = RenderRgba(pw, ph, [&](HDC dc) {
            RECT all{ 0, 0, kPanelW, kPanelH };
            FillRound(dc, all, RGB(28, 28, 32), 24);
            DrawTextIn(dc, g_vrBig, RGB(235, 235, 235), RECT{ 24, 16, 560, 60 }, L"Eye tracking", DT_LEFT | DT_SINGLELINE);
            FillRound(dc, RECT{ 24, 66, 540, 150 }, v.color, 16);
            DrawTextIn(dc, g_vrFont, RGB(255, 255, 255), RECT{ 36, 70, 530, 146 }, v.text, DT_LEFT | DT_WORDBREAK);
            const std::wstring lines =
                std::wstring(L"Headset: ") + (g_pimax.eyes == 1 ? L"worn (eyes detected)" : g_pimax.eyes == 0 ? L"not worn (no eyes detected)"
                                   : !g_vr.wornKnown ? L"?" : g_vr.worn ? L"worn" : L"not worn") +
                L"\nSteamVR gaze: " + (g_vr.state == VrState::Live ? Fmt(L"live, %d updates/s", static_cast<int>(g_vr.changes.size()))
                                       : g_vr.state == VrState::Frozen ? Fmt(L"frozen for %.0f s", NowS() - g_vr.changedAt)
                                       : g_vr.state == VrState::NoGaze ? std::wstring(L"none") : std::wstring(L"SteamVR not running")) +
                L"\nHeadset driver: " + (g_driver.updating && g_driver.valid ? L"receiving gaze" : L"no gaze") +
                L"\nAuto-recovery: " + (!g_auto.on ? std::wstring(L"off (A on the desktop window)")
                                        : g_auto.userOff ? std::wstring(L"on -- eye tracking is switched off in Pimax Play")
                                        : g_auto.exhausted ? std::wstring(L"3 restarts didn't help -- press Restart")
                                        : g_auto.attempts ? Fmt(L"on -- restart %d of 3", g_auto.attempts)
                                        : std::wstring(L"on (when Pimax gives up on the tracker)"));
            DrawTextIn(dc, g_vrSmall, RGB(200, 200, 200), RECT{ 28, 166, 560, 350 }, lines, DT_LEFT);
            const RECT btn{ static_cast<LONG>(kPanelW * 0.62f) + 10, 24, kPanelW - 24, kPanelH - 24 };
            FillRound(dc, btn, g_restarting ? RGB(70, 70, 80) : RGB(55, 105, 200), 20);
            const LONG mid = (btn.top + btn.bottom) / 2;
            if (g_restarting) {
                DrawTextIn(dc, g_vrBig, RGB(255, 255, 255), btn, L"Restarting...", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {   // two single-line halves (DT_VCENTER only centres one line)
                DrawTextIn(dc, g_vrBig, RGB(255, 255, 255), RECT{ btn.left, btn.top, btn.right, mid }, L"Restart", DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
                DrawTextIn(dc, g_vrBig, RGB(255, 255, 255), RECT{ btn.left, mid, btn.right, btn.bottom }, L"eye tracking", DT_CENTER | DT_TOP | DT_SINGLELINE);
            }
        }, 0.8f);
        SetRaw(g_vr.dash, px, pw, ph);
    }
    if (g_vr.badge) {
        const bool show = (FrozenWhileWorn() && FrozenWhileWornLong()) || g_restarting;   // automatic ones included
        if (show) {
            const std::wstring text = g_restarting ? (g_restartAuto ? L"Eye tracking lost -- recovering automatically..." : L"Eye tracking -- restarting...")
                                     : g_pimax.state == PimaxState::Stopped ? L"Eye tracking: Pimax service stopped"
                                     : g_auto.exhausted ? L"Eye tracking lost -- recovery failed: Dashboard > Eye tracking > Restart"
                                     : g_pimax.state == PimaxState::GaveUp && g_auto.on && !g_auto.userOff ? L"Eye tracking lost -- recovering in a moment"
                                     : L"Eye tracking frozen -- Dashboard: Eye tracking > Restart";
            int bw = kBadgeW, bh = kBadgeH;
            std::vector<uint8_t> px = RenderRgba(bw, bh, [&](HDC dc) {
                FillRound(dc, RECT{ 0, 0, kBadgeW, kBadgeH }, g_restarting ? RGB(40, 90, 190) : RGB(180, 35, 35), 30);
                DrawTextIn(dc, g_vrFont, RGB(255, 255, 255), RECT{ 16, 0, kBadgeW - 16, kBadgeH }, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            });
            SetRaw(g_vr.badge, px, bw, bh);
            o->ShowOverlay(g_vr.badge);
        } else {
            o->HideOverlay(g_vr.badge);
        }
    }
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {   // the taskbar button exists (also after Explorer restarts)
        TaskbarInit(wnd);
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            g_restartButton = CreateWindowExW(0, L"BUTTON", L"Restart eye tracking", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10,
                                              wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRestartButtonId)), nullptr, nullptr);
            SendMessageW(g_restartButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            PlaceButton(wnd);
            return 0;
        case WM_SIZE:
            PlaceButton(wnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kRestartButtonId) { StartRestart(wnd); SetFocus(wnd); }
            if (HIWORD(wp) == THBN_CLICKED && LOWORD(wp) == kThumbRestartId) StartRestart(wnd);
            return 0;
        case kMsgRestartEvent: {
            std::wstring* s = reinterpret_cast<std::wstring*>(lp);
            Event(Clock(), *s);
            delete s;
            return 0;
        }
        case kMsgRestartDone:
            g_restarting = false;
            if (wp == 1) g_auto.userOff = true;   // automatic recovery found the switch off
            if (g_restartButton) EnableWindow(g_restartButton, TRUE);
            return 0;
        case WM_TIMER:
            PimaxPoll();
            MountPoll();
            DriverPoll();
            VrPoll();
            AutoRestartTick(wnd);
            {
                const Verdict v = Judge();
                TaskbarUpdate(wnd, v);
                OverlayTick(wnd, v);
            }
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wp == 'T') {
                g_topmost = !g_topmost;
                SetWindowPos(wnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            } else if (wp == 'S') {
                g_soundOnLoss = !g_soundOnLoss;
            } else if (wp == 'R') {
                StartRestart(wnd);
            } else if (wp == 'A') {
                g_auto.on = !g_auto.on;
                Event(Clock(), g_auto.on ? L"automatic recovery ON" : L"automatic recovery off");
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            Paint(wnd, dc);
            EndPaint(wnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (g_vr.overlay && g_vr.dash) g_vr.overlay->DestroyOverlay(g_vr.dash);
            if (g_vr.overlay && g_vr.badge) g_vr.overlay->DestroyOverlay(g_vr.badge);
            if (g_vr.system && g_vr.shutdown) g_vr.shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    // One monitor at a time (Windows startup and a manual start would otherwise both connect to SteamVR).
    CreateMutexW(nullptr, TRUE, L"Local\\PimaxEyeTrackingMonitorSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"PimaxEyeTrackingMonitor", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    g_big = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_vrBig = CreateFontW(-40, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    g_vrFont = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    g_vrSmall = CreateFontW(-26, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Icon resource 1 (PimaxEyeTrackingMonitor.rc: eye_tracking.ico), at the system's large and small icon sizes.
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = L"PimaxEyeTrackingMonitor";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Eye tracking monitor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                               CW_USEDEFAULT, 720, 760, nullptr, nullptr, inst, nullptr);
    Event(Clock(), L"monitor started");
    ShowWindow(wnd, show);
    SetTimer(wnd, 1, 16, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
