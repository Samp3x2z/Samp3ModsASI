// counter_sm86.asi
// Reads SM86 frame timing, calculates the generated FPS value, and publishes it
// for FPSCounter's CET script. Build as a separate x64 ASI beside the settings ASI.
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cwchar>

static wchar_t g_realTiming[MAX_PATH];
static wchar_t g_mirrorFPS[MAX_PATH];

static ULONGLONG g_timingOffset = 0;
static ULONGLONG g_lastSample = 0;
static ULONGLONG g_lastClear = 0;
static std::string g_pending;
static double g_fps = 0.0;

static bool ParseNumber(const std::string& text, double* value) {
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    *value = parsed;
    return true;
}

static void WriteFPS(double value) {
    if (value < 0.0 || value >= 1000.0) value = 0.0;

    wchar_t tempPath[MAX_PATH];
    lstrcpyW(tempPath, g_mirrorFPS);
    wchar_t* dot = wcsrchr(tempPath, L'.');
    if (dot) lstrcpyW(dot, L".tmp");
    else lstrcatW(tempPath, L".tmp");

    char buffer[64];
    const int length = std::snprintf(buffer, sizeof(buffer), "%.2f\n", value);
    if (length <= 0) return;

    HANDLE file = CreateFileW(tempPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(file, buffer, (DWORD)length, &written, nullptr);
    CloseHandle(file);
    MoveFileExW(tempPath, g_mirrorFPS, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static void ProcessTimingLine(const std::string& line) {
    if (line.empty()) return;
    if (line.rfind("# ==== session", 0) == 0) {
        g_fps = 0.0;
        g_lastSample = 0;
        return;
    }
    if (line[0] == '#') return;

    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string token;
    while (stream >> token) fields.push_back(token);
    if (fields.size() < 6) return;

    double dt = 0.0, fgMs = 0.0, generatedValue = 0.0;
    if (!ParseNumber(fields[2], &dt) ||
        !ParseNumber(fields[4], &fgMs) ||
        !ParseNumber(fields[5], &generatedValue)) return;

    const int generated = (int)generatedValue;
    if (dt < 2.0 || dt > 250.0 || fgMs <= 0.0 ||
        generated < 0 || generated > 5) return;

    const double sample = (1000.0 / dt) * (generated + 1.0);
    if (sample <= 0.0 || sample >= 1000.0) return;

    if (g_fps <= 0.0) g_fps = sample;
    else g_fps += 0.25 * (sample - g_fps);

    g_lastSample = GetTickCount64();
}

static void PollTimingFile() {
    HANDLE file = CreateFileW(g_realTiming, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        WriteFPS(0.0);
        return;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return;
    }

    // The log may be truncated by its owner or by our one-minute cleanup.
    if (size.QuadPart < 0 || (ULONGLONG)size.QuadPart < g_timingOffset) {
        g_timingOffset = 0;
        g_pending.clear();
        g_fps = 0.0;
        g_lastSample = 0;
    }

    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)g_timingOffset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return;
    }

    char buffer[65536];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        g_timingOffset += read;
        g_pending.append(buffer, read);

        size_t newline = 0;
        while ((newline = g_pending.find('\n')) != std::string::npos) {
            std::string line = g_pending.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ProcessTimingLine(line);
            g_pending.erase(0, newline + 1);
        }
        if (read < sizeof(buffer)) break;
    }

    CloseHandle(file);

    if (g_pending.size() > 8192) g_pending.clear();
    if (g_lastSample != 0 && GetTickCount64() - g_lastSample > 1500)
        g_fps = 0.0;

    WriteFPS(g_fps);
}

static bool ClearTimingFile() {
    HANDLE file = CreateFileW(g_realTiming, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    const bool cleared = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) &&
                         SetEndOfFile(file) &&
                         FlushFileBuffers(file);
    CloseHandle(file);

    if (cleared) {
        // Reset the reader state so newly appended data starts at byte zero.
        g_timingOffset = 0;
        g_pending.clear();
        g_fps = 0.0;
        g_lastSample = 0;
        WriteFPS(0.0);
    }
    return cleared;
}

static bool DirOf(wchar_t* path) {
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

static void JoinLogPath(const wchar_t* pluginDir, const wchar_t* relativeDir) {
    if (relativeDir[1] == L':' || relativeDir[0] == L'\\') {
        lstrcpyW(g_realTiming, relativeDir);
    } else {
        lstrcpyW(g_realTiming, pluginDir);
        lstrcatW(g_realTiming, relativeDir);
    }
    const size_t length = lstrlenW(g_realTiming);
    if (length && g_realTiming[length - 1] != L'\\') lstrcatW(g_realTiming, L"\\");
    lstrcatW(g_realTiming, L"frame_timing.txt");
}

static DWORD WINAPI Worker(LPVOID) {
    g_lastClear = GetTickCount64();
    for (;;) {
        PollTimingFile();

        const ULONGLONG now = GetTickCount64();
        if (now - g_lastClear >= 60000ULL) {
            ClearTimingFile();
            g_lastClear = now;
        }

        Sleep(100);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        wchar_t pluginDir[MAX_PATH];
        if (!GetModuleFileNameW(module, pluginDir, MAX_PATH) || !DirOf(pluginDir))
            return TRUE;

        wchar_t iniPath[MAX_PATH];
        lstrcpyW(iniPath, pluginDir);
        lstrcatW(iniPath, L"dlssg_sm86.ini");

        if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) {
            wchar_t parent[MAX_PATH];
            lstrcpyW(parent, pluginDir);
            const size_t length = lstrlenW(parent);
            if (length > 1) {
                parent[length - 1] = 0;
                if (DirOf(parent)) {
                    lstrcatW(parent, L"dlssg_sm86.ini");
                    if (GetFileAttributesW(parent) != INVALID_FILE_ATTRIBUTES)
                        lstrcpyW(iniPath, parent);
                }
            }
        }

        wchar_t loggingDir[MAX_PATH] = L"dlssg_sm86\\logs";
        GetPrivateProfileStringW(L"Logging", L"Directory", L"dlssg_sm86\\logs",
                                 loggingDir, MAX_PATH, iniPath);
        JoinLogPath(pluginDir, loggingDir);

        lstrcpyW(g_mirrorFPS, pluginDir);
        lstrcatW(g_mirrorFPS, L"cyber_engine_tweaks\\mods\\FPSCounter\\sm86_fps.txt");

        wchar_t fpsDir[MAX_PATH];
        lstrcpyW(fpsDir, g_mirrorFPS);
        if (DirOf(fpsDir)) CreateDirectoryW(fpsDir, nullptr);

        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) {
            SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(thread);
        }
    }
    return TRUE;
}
