// FrameLock launcher: pick a game, inject the hook DLL, set FPS/output, Record/Stop.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cerrno>
#include <climits>
#include "../shared.h"
#include "../hook/fps_parse.h"

#ifdef _WIN64
#define DLL_NAME L"FrameLockHook64.dll"
#define APP_TITLE L"FrameLock Recorder (64-bit games)"
#else
#define DLL_NAME L"FrameLockHook32.dll"
#define APP_TITLE L"FrameLock Recorder (32-bit games)"
#endif

enum { IDC_PROC = 101, IDC_REFRESH, IDC_LAUNCH, IDC_FPS, IDC_OUT, IDC_BROWSE, IDC_ATTACH, IDC_RECORD, IDC_STATUS, IDC_REALTIME, IDC_TEST };
static HWND g_wnd, hProc, hFps, hOut, hRecord, hStatus, hRealtime;
static std::vector<DWORD> g_pids;
static DWORD g_pid = 0; static HANDLE g_hProcess = nullptr;
static HANDLE g_map = nullptr; static FLShared* g_sh = nullptr;
static DWORD g_launchPid = 0; static ULONGLONG g_launchTime = 0;

static void Msg(const wchar_t* t) { MessageBoxW(g_wnd, t, L"FrameLock", MB_OK | MB_ICONINFORMATION); }

struct WinInfo { std::map<DWORD, std::wstring>* m; };
static BOOL CALLBACK EnumWinProc(HWND w, LPARAM lp) {
    auto m = (std::map<DWORD, std::wstring>*)lp;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t t[256]; if (!GetWindowTextW(w, t, 256) || !t[0]) return TRUE;
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() && !m->count(pid)) (*m)[pid] = t;
    return TRUE;
}
static void RefreshProcs() {
    std::map<DWORD, std::wstring> wins; EnumWindows(EnumWinProc, (LPARAM)&wins);
    SendMessageW(hProc, CB_RESETCONTENT, 0, 0); g_pids.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe)) do {
        auto it = wins.find(pe.th32ProcessID); if (it == wins.end()) continue;
        wchar_t line[512]; swprintf_s(line, L"%s  (PID %lu)  -  %s", pe.szExeFile, pe.th32ProcessID, it->second.c_str());
        SendMessageW(hProc, CB_ADDSTRING, 0, (LPARAM)line); g_pids.push_back(pe.th32ProcessID);
    } while (Process32NextW(snap, &pe));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
}
static bool ParseFps(const wchar_t* s, LONG& num, LONG& den) {
    int64_t n = 0, d = 0;
    if (!flfps::Parse(s, n, d) || n > LONG_MAX || d > LONG_MAX) return false;
    num = static_cast<LONG>(n);
    den = static_cast<LONG>(d);
    return true;
}
static bool OpenShared(DWORD pid) {
    if (g_sh) { UnmapViewOfFile(g_sh); g_sh = nullptr; } if (g_map) { CloseHandle(g_map); g_map = nullptr; }
    wchar_t name[64]; swprintf_s(name, FL_SHARED_FMT, pid);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FLShared), name);
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (!g_map) return false;
    g_sh = (FLShared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FLShared));
    if (!g_sh) { CloseHandle(g_map); g_map = nullptr; return false; }
    if (!existed || g_sh->magic != FL_MAGIC) { ZeroMemory(g_sh, sizeof(FLShared)); g_sh->magic = FL_MAGIC; g_sh->fpsNum = 24; g_sh->fpsDen = 1; }
    return true;
}
static bool Inject(DWORD pid, std::wstring& err) {
    wchar_t dll[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, dll, MAX_PATH)) { err = L"GetModuleFileNameW failed."; return false; }
    wchar_t* slash = wcsrchr(dll, L'\\');
    if (!slash) { err = L"Cannot resolve the launcher folder."; return false; }
    *(slash + 1) = 0;
    if (wcscat_s(dll, DLL_NAME) != 0 || GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        err = L"Hook DLL not found next to the launcher."; return false;
    }
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!p) { err = L"Cannot open the game process. Run FrameLock with sufficient permissions."; return false; }
    BOOL tw = FALSE, sw = FALSE;
    if (!IsWow64Process(p, &tw) || !IsWow64Process(GetCurrentProcess(), &sw)) {
        CloseHandle(p); err = L"Cannot determine process architecture."; return false;
    }
    if (tw != sw) { CloseHandle(p); err = sw ? L"This game is 64-bit: use FrameLock64.exe." : L"This game is 32-bit: use FrameLock32.exe."; return false; }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto ll = kernel ? (LPTHREAD_START_ROUTINE)GetProcAddress(kernel, "LoadLibraryW") : nullptr;
    if (!ll) { CloseHandle(p); err = L"LoadLibraryW was not found in kernel32.dll."; return false; }
    size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { CloseHandle(p); err = L"Allocating memory in the game process failed."; return false; }
    SIZE_T written = 0;
    if (!WriteProcessMemory(p, mem, dll, bytes, &written) || written != bytes) {
        VirtualFreeEx(p, mem, 0, MEM_RELEASE); CloseHandle(p);
        err = L"Writing the DLL path into the game process failed."; return false;
    }
    HANDLE t = CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr);
    if (!t) {
        VirtualFreeEx(p, mem, 0, MEM_RELEASE); CloseHandle(p);
        err = L"CreateRemoteThread failed (anti-cheat or protected process?)."; return false;
    }
    // The remote thread may still be reading `mem` until LoadLibraryW returns. Never time out and free it early.
    DWORD wait = WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    BOOL gotCode = wait == WAIT_OBJECT_0 && GetExitCodeThread(t, &code);
    CloseHandle(t);
    if (wait != WAIT_OBJECT_0 || !gotCode) {
        // Keep the remote allocation alive on an abnormal wait failure to avoid a use-after-free in LoadLibraryW.
        CloseHandle(p); err = L"Waiting for the remote loader thread failed."; return false;
    }
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    if (!code) { CloseHandle(p); err = L"LoadLibraryW failed inside the game (possibly blocked by anti-cheat)."; return false; }
    if (g_hProcess) CloseHandle(g_hProcess);
    g_hProcess = p;
    return true;
}
static void Attach(DWORD pid) {
    if (!OpenShared(pid)) { Msg(L"Shared memory creation failed."); return; }
    std::wstring err;
    if (!Inject(pid, err)) { Msg(err.c_str()); return; }
    g_pid = pid;
}
static bool ApplySettings() {
    wchar_t fps[64]; GetWindowTextW(hFps, fps, 64);
    LONG n, d; if (!ParseFps(fps, n, d)) { Msg(L"Invalid FPS. Examples: 24, 30, 60, 23.976, 60000/1001"); return false; }
    wchar_t out[MAX_PATH]; GetWindowTextW(hOut, out, MAX_PATH);
    if (!out[0]) { Msg(L"Choose an output folder."); return false; }
    int r = SHCreateDirectoryExW(nullptr, out, nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS) { Msg(L"Cannot create the output folder."); return false; }
    g_sh->fpsNum = n; g_sh->fpsDen = d; wcsncpy_s(g_sh->outDir, out, _TRUNCATE);
    InterlockedExchange(&g_sh->mode, SendMessageW(hRealtime, BM_GETCHECK, 0, 0) == BST_CHECKED ? FL_MODE_REALTIME : FL_MODE_LOCKED);
    return true;
}
static void ToggleRecord() {
    if (!g_sh || !g_pid) { Msg(L"Attach to a game first."); return; }
    if (!g_sh->hooksReady) { Msg(L"Hooks are not ready yet. Wait a second and try again."); return; }
    if (!g_sh->wantRecord) { if (ApplySettings()) InterlockedExchange(&g_sh->wantRecord, 1); }
    else InterlockedExchange(&g_sh->wantRecord, 0);
}
static void StartTest() {
    if (!g_sh || !g_pid) { Msg(L"Attach to a game first."); return; }
    if (!g_sh->hooksReady) { Msg(L"Hooks are not ready yet. Wait a second and try again."); return; }
    if (g_sh->wantRecord || g_sh->isRecording) { Msg(L"Stop the current recording first."); return; }
    if (ApplySettings()) { InterlockedExchange(&g_sh->testFrames, 120); InterlockedExchange(&g_sh->wantRecord, 1); }
}
static BOOL CALLBACK FindPidWin(HWND w, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    if (pid == g_launchPid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(bool*)lp = true; return FALSE; }
    return TRUE;
}
// phase opus55: Vulkan capture runs as an implicit Vulkan layer inside the hook DLL. Write its manifest next to the
// DLL and register it for the current user. The layer is gated by FRAMELOCK_VK_CAPTURE=1, which is set only for the
// process started by LaunchGame(), so other Vulkan apps are unaffected even though the registration persists.
// review4: BOTH layer architectures are registered whenever both hook DLLs sit next to the launcher, so a 32-bit
// Vulkan game started from FrameLock64.exe (or a 32-bit launcher stub that starts a 64-bit game, and vice versa) still
// gets the matching layer. HKCU\Software\Khronos\Vulkan\ImplicitLayers is read by both the 32-bit and the 64-bit
// Vulkan loader (HKCU\Software is not WOW64-redirected), so one key serves both. Manifest file_format_version 1.2.1
// adds "library_arch", which lets current loaders skip the wrong-bitness manifest without calling LoadLibrary; older
// loaders try LoadLibrary, fail with ERROR_BAD_EXE_FORMAT and skip the implicit layer (non-fatal). Layer names differ
// per bitness, so the two manifests never shadow each other.
struct VkLayerDesc { const wchar_t* dll; const wchar_t* manifest; const char* name; const char* arch; };
static const VkLayerDesc kVkLayers[2] = {
    { L"FrameLockHook64.dll", L"FrameLock_VkLayer64.json", "VK_LAYER_FRAMELOCK_capture64", "64" },
    { L"FrameLockHook32.dll", L"FrameLock_VkLayer32.json", "VK_LAYER_FRAMELOCK_capture32", "32" },
};
#ifdef _WIN64
static const int kOwnLayer = 0;
#else
static const int kOwnLayer = 1;
#endif
static std::string JsonEscapeUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
    std::string o;
    for (char c : u) { if (c == '\\' || c == '"') o += '\\'; o += c; }
    return o;
}
static bool WriteLayerManifest(const std::wstring& dir, const VkLayerDesc& ly, std::wstring& json, std::wstring& err) {
    std::wstring dll = dir + ly.dll;
    json = dir + ly.manifest;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) { err = std::wstring(ly.dll) + L" not found next to the launcher"; return false; }
    std::string body = std::string("{\n  \"file_format_version\": \"1.2.1\",\n  \"layer\": {\n    \"name\": \"") + ly.name +
        "\",\n    \"type\": \"GLOBAL\",\n    \"library_path\": \"" + JsonEscapeUtf8(dll) +
        "\",\n    \"library_arch\": \"" + ly.arch +
        "\",\n    \"api_version\": \"1.3.0\",\n    \"implementation_version\": \"1\",\n"
        "    \"description\": \"FrameLock frame-locked capture (active only for games started from FrameLock)\",\n"
        "    \"enable_environment\": { \"FRAMELOCK_VK_CAPTURE\": \"1\" },\n"
        "    \"disable_environment\": { \"DISABLE_FRAMELOCK_VK_CAPTURE\": \"1\" }\n  }\n}\n";
    HANDLE f = CreateFileW(json.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = std::wstring(L"cannot write ") + ly.manifest + L" next to the launcher (folder read-only?)"; return false; }
    DWORD wr = 0; BOOL ok = WriteFile(f, body.data(), (DWORD)body.size(), &wr, nullptr); CloseHandle(f);
    if (!ok || wr != body.size()) { err = std::wstring(L"writing ") + ly.manifest + L" failed"; return false; }
    return true;
}
// Returns true if the layer for the launcher's own bitness is registered (required). The other bitness is registered
// best-effort when its DLL is present; a note is put in 'err' if it is missing so the user can be told.
static bool RegisterVulkanLayer(std::wstring& err) {
    wchar_t dirBuf[MAX_PATH]; GetModuleFileNameW(nullptr, dirBuf, MAX_PATH);
    if (wchar_t* s = wcsrchr(dirBuf, L'\\')) *(s + 1) = 0;
    const std::wstring dir = dirBuf;
    std::wstring json[2], werr[2]; bool wrote[2] = { false, false };
    for (int i = 0; i < 2; i++) wrote[i] = WriteLayerManifest(dir, kVkLayers[i], json[i], werr[i]);
    if (!wrote[kOwnLayer]) { err = werr[kOwnLayer]; return false; }
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Khronos\\Vulkan\\ImplicitLayers", 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) { err = L"cannot open HKCU Vulkan ImplicitLayers key"; return false; }
    // review3/4: remove stale FrameLock entries left behind when the FrameLock folder was moved or deleted. Only values
    // whose name ends in "\FrameLock_VkLayer64.json" or "\FrameLock_VkLayer32.json", that are not one of the current
    // manifests, AND whose file no longer exists are deleted, so other FrameLock copies and unrelated layers are never
    // touched. Best effort.
    {
        std::vector<std::wstring> stale;
        std::vector<wchar_t> name(32768);
        for (DWORD i = 0;; i++) {
            DWORD len = (DWORD)name.size();
            LONG e = RegEnumValueW(k, i, name.data(), &len, nullptr, nullptr, nullptr, nullptr);
            if (e == ERROR_NO_MORE_ITEMS) break;
            if (e != ERROR_SUCCESS) { if (i > 100000) break; continue; }   // e.g. ERROR_MORE_DATA: skip this value
            std::wstring n(name.data(), len);
            bool ours = false;
            for (int j = 0; j < 2; j++) {
                const std::wstring suffix = std::wstring(L"\\") + kVkLayers[j].manifest;
                if (n.size() > suffix.size() && _wcsicmp(n.c_str() + n.size() - suffix.size(), suffix.c_str()) == 0) ours = true;
            }
            if (ours && _wcsicmp(n.c_str(), json[0].c_str()) != 0 && _wcsicmp(n.c_str(), json[1].c_str()) != 0 &&
                GetFileAttributesW(n.c_str()) == INVALID_FILE_ATTRIBUTES)
                stale.push_back(n);
        }
        for (const auto& n : stale) RegDeleteValueW(k, n.c_str());   // delete after enumeration (indices shift)
    }
    DWORD zero = 0;   // 0 = enabled (still gated by enable_environment)
    LONG r = RegSetValueExW(k, json[kOwnLayer].c_str(), 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
    const int other = 1 - kOwnLayer;
    if (wrote[other]) RegSetValueExW(k, json[other].c_str(), 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));   // best effort
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) { err = L"cannot register the Vulkan layer manifest"; return false; }
    if (!wrote[other]) err = werr[other];   // informational only (return value stays true)
    return true;
}

static void LaunchGame() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = { sizeof(of) }; of.hwndOwner = g_wnd; of.lpstrFilter = L"Game executable\0*.exe\0"; of.lpstrFile = file; of.nMaxFile = MAX_PATH; of.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, file); if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    std::wstring cmd = L"\"" + std::wstring(file) + L"\"";
    // review4: warn about a bitness mismatch BEFORE starting the game. D3D/OpenGL hooking needs DLL injection, which
    // only works from the launcher of the same bitness. (The Vulkan layer of both bitnesses is registered, so a
    // mismatched .exe that is merely a launcher stub can still be started and its real game attached afterwards.)
    DWORD binType = 0;
    if (GetBinaryTypeW(file, &binType)) {
#ifdef _WIN64
        const bool mismatch = binType == SCS_32BIT_BINARY;
        const wchar_t* mmText = L"This game executable is 32-bit, but this is the 64-bit launcher, which cannot inject into 32-bit processes.\n\n"
            L"Use FrameLock32.exe -> 'Launch game .exe and attach' for this game.\n\n"
            L"Start it from here anyway? (Only useful if this .exe is a launcher stub that starts a 64-bit game; then use Attach on the real game process.)";
#else
        const bool mismatch = binType == SCS_64BIT_BINARY;
        const wchar_t* mmText = L"This game executable is 64-bit, but this is the 32-bit launcher, which cannot inject into 64-bit processes.\n\n"
            L"Use FrameLock64.exe -> 'Launch game .exe and attach' for this game.\n\n"
            L"Start it from here anyway? (Only useful if this .exe is a launcher stub that starts a 32-bit game; then use Attach on the real game process.)";
#endif
        if (mismatch && MessageBoxW(g_wnd, mmText, L"FrameLock", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    std::wstring vkErr;
    bool vkOk = RegisterVulkanLayer(vkErr);   // non-fatal: only Vulkan capture depends on it
    if (vkOk) SetEnvironmentVariableW(L"FRAMELOCK_VK_CAPTURE", L"1");   // inherited by the game only (cleared below)
    // review4: start suspended so the per-PID shared memory exists before the game executes its first instruction.
    // The hook DLL (loaded by the Vulkan loader at vkCreateInstance, or injected later) initialises its core only when
    // Local\FrameLock_<pid> already exists; child processes that inherit FRAMELOCK_VK_CAPTURE therefore stay dormant
    // (pure pass-through, no clock hooks, no hotkeys) until FrameLock attaches to them. See dllmain.cpp InitThread.
    BOOL started = CreateProcessW(file, &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir, &si, &pi);
    if (vkOk) SetEnvironmentVariableW(L"FRAMELOCK_VK_CAPTURE", nullptr);
    if (!started) { Msg(L"Could not start the game."); return; }
    bool shOk = OpenShared(pi.dwProcessId);
    ResumeThread(pi.hThread);
    if (!shOk) Msg(L"Game started, but FrameLock shared memory could not be created for it; Vulkan capture will stay inactive. Use Attach once the game window is visible.");
    if (!vkOk) Msg((L"Game started, but the Vulkan capture layer could not be registered (" + vkErr + L"). Direct3D/OpenGL capture is unaffected; Vulkan games will not be captured.").c_str());
    // (vkErr may hold an informational note, e.g. the other-bitness hook DLL is missing; not shown to avoid a popup per launch)
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    g_launchPid = pi.dwProcessId; g_launchTime = GetTickCount64();
}
// While idle, keep the DLL's FPS / folder / mode in sync with the UI so a take started with F9 inside the game
// uses exactly the FPS shown here (otherwise frames stepped at one rate could be encoded at another).
static void SyncSettingsQuiet() {
    if (!g_sh || !g_pid || g_sh->wantRecord || g_sh->isRecording) return;
    wchar_t fps[64]; GetWindowTextW(hFps, fps, 64);
    LONG n, d;
    if (ParseFps(fps, n, d) && (g_sh->fpsNum != n || g_sh->fpsDen != d)) { g_sh->fpsNum = n; g_sh->fpsDen = d; }
    wchar_t out[MAX_PATH]; GetWindowTextW(hOut, out, MAX_PATH);
    if (out[0] && wcscmp(out, g_sh->outDir)) wcsncpy_s(g_sh->outDir, out, _TRUNCATE);
    LONG mode = SendMessageW(hRealtime, BM_GETCHECK, 0, 0) == BST_CHECKED ? FL_MODE_REALTIME : FL_MODE_LOCKED;
    if (g_sh->mode != mode) InterlockedExchange(&g_sh->mode, mode);
}
static void UpdateStatus() {
    SyncSettingsQuiet();
    if (g_launchPid) {
        bool found = false; EnumWindows(FindPidWin, (LPARAM)&found);
        if (found && GetTickCount64() - g_launchTime > 3000) { DWORD p = g_launchPid; g_launchPid = 0; Attach(p); }
        else if (GetTickCount64() - g_launchTime > 120000) { g_launchPid = 0; Msg(L"Game window not found. If it uses a launcher, start it normally and use Attach."); }
    }
    wchar_t txt[2048];
    if (g_hProcess && WaitForSingleObject(g_hProcess, 0) == WAIT_OBJECT_0) { CloseHandle(g_hProcess); g_hProcess = nullptr; g_pid = 0; }
    if (!g_sh || !g_pid) { swprintf_s(txt, L"Not attached. Start your game, select it above and press Attach.%s", g_launchPid ? L"\r\nWaiting for launched game window..." : L""); }
    else {
        swprintf_s(txt, L"Attached to PID %lu   |   Hooks: %s   |   API: %s   |   Resolution: %ld x %ld\r\n"
            L"State: %s (%s)   |   Frames committed: %lld   saved: %lld   |   Stall warnings: %lld   |   Last HRESULT: 0x%08X\r\nTake folder: %s\r\n%s",
            g_pid, g_sh->hooksReady ? L"active" : L"loading", g_sh->api[0] ? g_sh->api : L"(waiting for first frame)", g_sh->width, g_sh->height,
            g_sh->isRecording ? L"RECORDING" : L"idle", g_sh->mode == FL_MODE_REALTIME ? L"real-time" : L"locked clock", (long long)g_sh->framesCommitted, (long long)g_sh->framesSaved, (long long)g_sh->slipEvents, (unsigned)g_sh->lastHr, g_sh->takeDir, g_sh->status);
    }
    wchar_t old[2048]; GetWindowTextW(hStatus, old, 2048); if (wcscmp(old, txt)) SetWindowTextW(hStatus, txt);
    const wchar_t* lbl = (g_sh && (g_sh->wantRecord || g_sh->isRecording)) ? L"\u25A0  STOP  (F9)" : L"\u25CF  RECORD  (F9)";
    wchar_t cur[64]; GetWindowTextW(hRecord, cur, 64); if (wcscmp(cur, lbl)) SetWindowTextW(hRecord, lbl);
}
static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}
static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = w;
        Ctl(L"STATIC", L"Game:", 0, 12, 16, 90, 20, 0);
        hProc = Ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 105, 12, 400, 300, IDC_PROC);
        Ctl(L"BUTTON", L"Refresh", WS_TABSTOP, 512, 11, 80, 24, IDC_REFRESH);
        Ctl(L"BUTTON", L"Attach", WS_TABSTOP, 598, 11, 80, 24, IDC_ATTACH);
        Ctl(L"BUTTON", L"Launch game .exe and attach...", WS_TABSTOP, 105, 42, 250, 24, IDC_LAUNCH);
        Ctl(L"STATIC", L"FPS:", 0, 12, 80, 90, 20, 0);
        hFps = Ctl(L"EDIT", L"24", ES_AUTOHSCROLL | WS_TABSTOP, 105, 77, 120, 24, IDC_FPS);
        Ctl(L"STATIC", L"(24, 30, 60, 23.976, 60000/1001 ...)   JPEG quality: 100% (4:4:4), native resolution", 0, 235, 80, 450, 20, 0);
        Ctl(L"STATIC", L"Output folder:", 0, 12, 114, 90, 20, 0);
        wchar_t def[MAX_PATH] = {};
        const DWORD profileLen = GetEnvironmentVariableW(L"USERPROFILE", def, MAX_PATH);
        if (profileLen > 0 && profileLen < MAX_PATH) {
            const size_t suffixLen = wcslen(L"\\Videos\\FrameLock");
            const size_t baseLen = wcslen(def);
            if (baseLen + suffixLen >= MAX_PATH) def[0] = 0;
            else memcpy(def + baseLen, L"\\Videos\\FrameLock", (suffixLen + 1) * sizeof(wchar_t));
        }
        if (!def[0]) {
            const DWORD tempLen = GetTempPathW(MAX_PATH, def);
            const size_t suffixLen = wcslen(L"FrameLock");
            const size_t baseLen = def[0] ? wcslen(def) : MAX_PATH;
            if (tempLen == 0 || tempLen >= MAX_PATH || baseLen + suffixLen >= MAX_PATH) wcscpy_s(def, L"C:\\FrameLock");
            else memcpy(def + baseLen, L"FrameLock", (suffixLen + 1) * sizeof(wchar_t));
        }
        hOut = Ctl(L"EDIT", def, ES_AUTOHSCROLL | WS_TABSTOP, 105, 111, 487, 24, IDC_OUT);
        Ctl(L"BUTTON", L"Browse...", WS_TABSTOP, 598, 111, 80, 24, IDC_BROWSE);
        hRecord = Ctl(L"BUTTON", L"\u25CF  RECORD  (F9)", WS_TABSTOP, 105, 148, 380, 44, IDC_RECORD);
        Ctl(L"BUTTON", L"Self-test: 120 frames (F10)", WS_TABSTOP, 492, 148, 186, 44, IDC_TEST);
        hRealtime = Ctl(L"BUTTON", L"Real-time capture (NOT frame-locked: saves only frames the game presents; encode at measured FPS)", BS_AUTOCHECKBOX | WS_TABSTOP, 105, 198, 573, 20, IDC_REALTIME);
        hStatus = Ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, 12, 226, 666, 110, IDC_STATUS);
        RefreshProcs(); SetTimer(w, 1, 250, nullptr);
        return 0; }
    case WM_TIMER: UpdateStatus(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH: RefreshProcs(); break;
        case IDC_ATTACH: { int i = (int)SendMessageW(hProc, CB_GETCURSEL, 0, 0); if (i < 0 || i >= (int)g_pids.size()) Msg(L"Select the game process first."); else Attach(g_pids[i]); break; }
        case IDC_LAUNCH: LaunchGame(); break;
        case IDC_RECORD: ToggleRecord(); break;
        case IDC_TEST: StartTest(); break;
        case IDC_BROWSE: {
            BROWSEINFOW bi = {}; bi.hwndOwner = w; bi.lpszTitle = L"Output folder for frames"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) { wchar_t p[MAX_PATH]; if (SHGetPathFromIDListW(pidl, p)) SetWindowTextW(hOut, p); CoTaskMemFree(pidl); }
            break; }
        }
        return 0;
    case WM_DESTROY:
        if (g_sh && g_sh->wantRecord) InterlockedExchange(&g_sh->wantRecord, 0);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"FrameLockMain";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, APP_TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 706, 390, nullptr, nullptr, hi, nullptr);
    ShowWindow(w, show);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0;
}
