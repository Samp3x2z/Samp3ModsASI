// dlssg_sm86_settings.asi
// Keeps the SM86 INI synchronized with the CET helper mod.
// Build as a separate x64 ASI and place beside dlssg_sm86_counter.asi.
#include <windows.h>
#include <cwchar>

static wchar_t g_realIni[MAX_PATH];
static wchar_t g_mirrorIni[MAX_PATH];

static bool Stat(const wchar_t* path, FILETIME* time, ULONGLONG* size) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
    *time = data.ftLastWriteTime;
    *size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return true;
}

static void CopyKeepTime(const wchar_t* src, const wchar_t* dst, const FILETIME& time) {
    if (!CopyFileW(src, dst, FALSE)) return;
    HANDLE file = CreateFileW(dst, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        SetFileTime(file, nullptr, nullptr, &time);
        CloseHandle(file);
    }
}

static bool Settled(const FILETIME& time, ULONGLONG size) {
    if (size == 0) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG current = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    const ULONGLONG modified = ((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime;
    return current >= modified && (current - modified) > 3000000ULL;
}

static void SyncIni() {
    FILETIME realTime, mirrorTime;
    ULONGLONG realSize = 0, mirrorSize = 0;
    const bool hasReal = Stat(g_realIni, &realTime, &realSize);
    const bool hasMirror = Stat(g_mirrorIni, &mirrorTime, &mirrorSize);

    if (!hasReal || !Settled(realTime, realSize)) return;

    if (!hasMirror) {
        CopyKeepTime(g_realIni, g_mirrorIni, realTime);
        return;
    }

    const LONG comparison = CompareFileTime(&realTime, &mirrorTime);
    if (comparison > 0) CopyKeepTime(g_realIni, g_mirrorIni, realTime);
    else if (comparison < 0 && Settled(mirrorTime, mirrorSize))
        CopyKeepTime(g_mirrorIni, g_realIni, mirrorTime);
}

static bool DirOf(wchar_t* path) {
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

static DWORD WINAPI Worker(LPVOID) {
    for (;;) {
        SyncIni();
        Sleep(500);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        wchar_t pluginDir[MAX_PATH];
        if (!GetModuleFileNameW(module, pluginDir, MAX_PATH) || !DirOf(pluginDir))
            return TRUE;

        lstrcpyW(g_realIni, pluginDir);
        lstrcatW(g_realIni, L"dlssg_sm86.ini");

        // Fallback: INI beside the game executable, one directory above plugins.
        if (GetFileAttributesW(g_realIni) == INVALID_FILE_ATTRIBUTES) {
            wchar_t parent[MAX_PATH];
            lstrcpyW(parent, pluginDir);
            const size_t length = lstrlenW(parent);
            if (length > 1) {
                parent[length - 1] = 0;
                if (DirOf(parent)) {
                    lstrcatW(parent, L"dlssg_sm86.ini");
                    if (GetFileAttributesW(parent) != INVALID_FILE_ATTRIBUTES)
                        lstrcpyW(g_realIni, parent);
                }
            }
        }

        lstrcpyW(g_mirrorIni, pluginDir);
        lstrcatW(g_mirrorIni, L"cyber_engine_tweaks\\mods\\FrameGenSM86\\dlssg_sm86.ini");

        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) {
            SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(thread);
        }
    }
    return TRUE;
}
