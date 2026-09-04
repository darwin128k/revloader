#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <string.h>

/* Temporary replacement for this install's cstrike.exe (RevEmu RevLoader,
 * 2014). Not part of vellum -- disposable once a real overlay launcher exists.
 *
 * hl.exe is the game process. This exe sets up RevEmu (steam.dll + IPC +
 * ActiveProcess registry) and CreateProcess's ProcName from rev.ini
 * (default: "hl.exe -game cstrike"). Must stay 32-bit and alive until hl.exe
 * exits. */

#define STEAM_IPC_MAPPING_NAME "Local\\SteamStart_SharedMemFile"
#define STEAM_IPC_EVENT_NAME   "Local\\SteamStart_SharedMemLock"
#define STEAM_IPC_SIZE         0x400
#define ACTIVE_PROCESS_KEY     "Software\\Valve\\Steam\\ActiveProcess"

struct SteamIpc {
    HANDLE mapping;
    void *view;
    HANDLE lockEvent;
};

static void Fail(const char *text)
{
    MessageBoxA(NULL, text, "Error", MB_OK | MB_ICONERROR);
}

static void JoinPath(char *out, size_t outSize, const char *dir, const char *file)
{
    size_t n = strlen(dir);
    if (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) {
        _snprintf(out, outSize, "%s%s", dir, file);
    } else {
        _snprintf(out, outSize, "%s\\%s", dir, file);
    }
    out[outSize - 1] = '\0';
}

static void DirFromModulePath(char *dir, size_t dirSize)
{
    char *slash;

    GetModuleFileNameA(NULL, dir, (DWORD)dirSize);
    dir[dirSize - 1] = '\0';
    slash = strrchr(dir, '\\');
    if (slash != NULL) {
        slash[1] = '\0'; /* keep trailing backslash, same as RevLoader */
    }
}

static void WideToAnsi(const wchar_t *wide, char *out, size_t outSize)
{
    WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)outSize, NULL, NULL);
    out[outSize - 1] = '\0';
}

static void AppendArg(char *cmd, size_t cmdSize, const char *arg)
{
    size_t n = strlen(cmd);
    if (n > 0 && n + 1 < cmdSize) {
        cmd[n++] = ' ';
        cmd[n] = '\0';
    }
    _snprintf(cmd + n, cmdSize - n, "%s", arg);
    cmd[cmdSize - 1] = '\0';
}

/* steam_api!SteamAPI_IsSteamRunning: OpenProcess(ActiveProcess\pid) and
 * GetExitCodeProcess == STILL_ACTIVE. It must see a live process before
 * hl.exe/hw.dll calls SteamAPI_Init -- writing the child's pid after
 * CreateProcess races that. This process stays alive across WaitForSingleObject,
 * so our own pid is the stable "Steam is running" stand-in.
 *
 * SteamAPI_Init then LoadLibrary's ActiveProcess\SteamClientDll and calls
 * CreateInterface("SteamClient012"). steam.dll does not implement that
 * interface; RevEmu's steamclient.dll does. */
static void WriteActiveProcess(DWORD pid, const char *steamClientDll)
{
    HKEY key = NULL;
    DWORD disp = 0;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, ACTIVE_PROCESS_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
        if (RegCreateKeyExA(HKEY_CURRENT_USER, ACTIVE_PROCESS_KEY, 0, NULL, 0, KEY_WRITE,
                            NULL, &key, &disp) != ERROR_SUCCESS) {
            return;
        }
    }
    RegSetValueExA(key, "pid", 0, REG_DWORD, (const BYTE *)&pid, sizeof(pid));
    if (steamClientDll != NULL && steamClientDll[0] != '\0') {
        RegSetValueExA(key, "SteamClientDll", 0, REG_SZ,
                       (const BYTE *)steamClientDll, (DWORD)strlen(steamClientDll) + 1);
    }
    RegCloseKey(key);
}

static int SetupSteamIpc(SteamIpc *ipc)
{
    memset(ipc, 0, sizeof(*ipc));

    ipc->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                      STEAM_IPC_SIZE, STEAM_IPC_MAPPING_NAME);
    if (ipc->mapping == NULL) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "Unable to CreateFileMapping: %i", (int)GetLastError());
        Fail(msg);
        return 0;
    }

    ipc->view = MapViewOfFile(ipc->mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (ipc->view == NULL) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "Unable to MapViewOfFile: %i", (int)GetLastError());
        Fail(msg);
        CloseHandle(ipc->mapping);
        ipc->mapping = NULL;
        return 0;
    }

    ipc->lockEvent = CreateEventA(NULL, FALSE, FALSE, STEAM_IPC_EVENT_NAME);
    if (ipc->lockEvent == NULL) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "Unable to CreateEvent: %i", (int)GetLastError());
        Fail(msg);
        UnmapViewOfFile(ipc->view);
        CloseHandle(ipc->mapping);
        memset(ipc, 0, sizeof(*ipc));
        return 0;
    }

    return 1;
}

static void TeardownSteamIpc(SteamIpc *ipc)
{
    if (ipc->lockEvent != NULL) {
        CloseHandle(ipc->lockEvent);
    }
    if (ipc->view != NULL) {
        UnmapViewOfFile(ipc->view);
    }
    if (ipc->mapping != NULL) {
        CloseHandle(ipc->mapping);
    }
    memset(ipc, 0, sizeof(*ipc));
}

#define DEFAULT_STEAM_APPID "10"

static int ReadSteamAppId(const char *dir, char *out, size_t outSize)
{
    char path[MAX_PATH];
    FILE *f;
    size_t n;

    JoinPath(path, sizeof(path), dir, "steam_appid.txt");
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    n = fread(out, 1, outSize - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return n > 0;
}

/* hw.dll unlinks steam_appid.txt when SteamAppId is already in the
 * environment (Steam-style launch). Rewrite it so the next start still
 * has an AppId without a warning dialog. */
static void WriteSteamAppId(const char *dir, const char *appId)
{
    char path[MAX_PATH];
    FILE *f;

    if (appId == NULL || appId[0] == '\0') {
        return;
    }
    JoinPath(path, sizeof(path), dir, "steam_appid.txt");
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%s\n", appId);
    fclose(f);
}

static int RunChild(char *cmdLine)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Unable to execute command %s (%i)", cmdLine, (int)GetLastError());
        Fail(msg);
        return 0;
    }

    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    return 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show)
{
    char dir[MAX_PATH];
    char iniPath[MAX_PATH];
    char procName[1024];
    char extraArgs[1024];
    char appId[256];
    char steamDll[MAX_PATH];
    char steamClient[MAX_PATH];
    char iniClient[256];
    SteamIpc ipc;
    int argc = 0;
    LPWSTR *argvW;
    int i;

    (void)instance;
    (void)prev;
    (void)cmd;
    (void)show;

    DirFromModulePath(dir, sizeof(dir));
    SetCurrentDirectoryA(dir);
    JoinPath(iniPath, sizeof(iniPath), dir, "rev.ini");

    procName[0] = '\0';
    extraArgs[0] = '\0';
    appId[0] = '\0';
    steamDll[0] = '\0';
    steamClient[0] = '\0';

    argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW != NULL) {
        for (i = 1; i < argc; i++) {
            if (_wcsicmp(argvW[i], L"-launch") == 0 && i + 1 < argc) {
                WideToAnsi(argvW[++i], procName, sizeof(procName));
            } else if (_wcsicmp(argvW[i], L"-appid") == 0 && i + 1 < argc) {
                WideToAnsi(argvW[++i], appId, sizeof(appId));
            } else {
                char arg[512];
                WideToAnsi(argvW[i], arg, sizeof(arg));
                AppendArg(extraArgs, sizeof(extraArgs), arg);
            }
        }
        LocalFree(argvW);
    }

    if (procName[0] == '\0') {
        GetPrivateProfileStringA("Loader", "ProcName", "", procName, sizeof(procName), iniPath);
        if (procName[0] == '\0') {
            Fail("ProcName value not found on command line or in rev.ini. Please edit the file.");
            return 1;
        }
    }

    if (extraArgs[0] != '\0') {
        AppendArg(procName, sizeof(procName), extraArgs);
    }

    if (appId[0] == '\0' && !ReadSteamAppId(dir, appId, sizeof(appId))) {
        strncpy(appId, DEFAULT_STEAM_APPID, sizeof(appId) - 1);
        appId[sizeof(appId) - 1] = '\0';
    }

    SetEnvironmentVariableA("SteamGameId", appId);
    SetEnvironmentVariableA("SteamAppId", appId);
    WriteSteamAppId(dir, appId);

    iniClient[0] = '\0';
    GetPrivateProfileStringA("Loader", "SteamClientDll", "", iniClient, sizeof(iniClient), iniPath);
    if (iniClient[0] != '\0') {
        if (strchr(iniClient, '\\') != NULL || strchr(iniClient, '/') != NULL) {
            strncpy(steamDll, iniClient, sizeof(steamDll) - 1);
            steamDll[sizeof(steamDll) - 1] = '\0';
        } else {
            JoinPath(steamDll, sizeof(steamDll), dir, iniClient);
        }
    } else {
        JoinPath(steamDll, sizeof(steamDll), dir, "steam.dll");
    }
    JoinPath(steamClient, sizeof(steamClient), dir, "steamclient.dll");

    if (!SetupSteamIpc(&ipc)) {
        return 1;
    }

    if (LoadLibraryA(steamDll) == NULL) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Can't find steam.dll relative to executable path %s", dir);
        Fail(msg);
        TeardownSteamIpc(&ipc);
        return 1;
    }

    if (GetFileAttributesA(steamClient) == INVALID_FILE_ATTRIBUTES) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Can't find steamclient.dll relative to executable path %s", dir);
        Fail(msg);
        TeardownSteamIpc(&ipc);
        return 1;
    }

    WriteActiveProcess(GetCurrentProcessId(), steamClient);

    if (!RunChild(procName)) {
        WriteSteamAppId(dir, appId);
        TeardownSteamIpc(&ipc);
        return 1;
    }

    WriteSteamAppId(dir, appId);
    TeardownSteamIpc(&ipc);
    return 0;
}
