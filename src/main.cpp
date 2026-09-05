#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <string.h>

#ifdef REVLOADER_STANDALONE
#include "launcher.h"
#endif

/* RevEmu loader for cstrike.exe. Default: set up Steam and CreateProcess
 * ProcName (hl.exe). With REVLOADER_STANDALONE: same Steam setup, then run
 * GoldSrc in this process via hl's HlLauncher_Run (sources from -DHL_DIR).
 * With REVLOADER_LAUNCHER_DLLS, optional [Loader] Dlls= in rev.ini is
 * forwarded as repeated -dll flags. */

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

#ifdef REVLOADER_LAUNCHER_DLLS
static const char *SkipSpaces(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static const char *NextToken(const char *p, char *out, size_t outSize)
{
    size_t n = 0;

    p = SkipSpaces(p);
    if (*p == '\0') {
        out[0] = '\0';
        return p;
    }
    if (*p == '"') {
        p++;
        while (*p != '\0' && *p != '"' && n + 1 < outSize) {
            out[n++] = *p++;
        }
        if (*p == '"') {
            p++;
        }
    } else {
        while (*p != '\0' && *p != ' ' && *p != '\t' && n + 1 < outSize) {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return p;
}

static int DllNameIsSafe(const char *name)
{
    size_t len;
    const char *p;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '"' || *p == '\'') {
            return 0;
        }
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    if (strstr(name, "..") != NULL) {
        return 0;
    }
    len = strlen(name);
    if (len < 5 || _stricmp(name + len - 4, ".dll") != 0) {
        return 0;
    }
    return 1;
}

static int CmdlineHasDll(const char *cmd, const char *name)
{
    const char *p = cmd;
    char tok[MAX_PATH];
    char got[MAX_PATH];

    while (*p != '\0') {
        p = NextToken(p, tok, sizeof(tok));
        if (tok[0] == '\0') {
            break;
        }
        if (_stricmp(tok, "-dll") != 0) {
            continue;
        }
        p = NextToken(p, got, sizeof(got));
        if (_stricmp(got, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int AppendDllsFromIni(char *cmd, size_t cmdSize, const char *iniPath)
{
    char list[1024];
    char name[MAX_PATH];
    const char *p;
    size_t n;

    GetPrivateProfileStringA("Loader", "Dlls", "", list, sizeof(list), iniPath);
    p = list;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        n = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' && n + 1 < sizeof(name)) {
            name[n++] = *p++;
        }
        name[n] = '\0';
        if (!DllNameIsSafe(name)) {
            Fail("Invalid Dlls entry in rev.ini (basename only, .dll in the game folder).");
            return 0;
        }
        if (CmdlineHasDll(cmd, name)) {
            continue;
        }
        AppendArg(cmd, cmdSize, "-dll");
        AppendArg(cmd, cmdSize, name);
    }
    return 1;
}
#endif

static int HasArg(const char *cmd, const char *arg)
{
    const char *p = cmd;
    size_t n = strlen(arg);

    while ((p = strstr(p, arg)) != NULL) {
        if (p == cmd || p[-1] == ' ' || p[-1] == '\t') {
            char end = p[n];
            if (end == '\0' || end == ' ' || end == '\t') {
                return 1;
            }
        }
        p += n;
    }
    return 0;
}

static void AppendLaunchTail(char *cmd, size_t cmdSize, const char *procName)
{
    const char *rest = procName;

    if (rest[0] == '"') {
        rest = strchr(rest + 1, '"');
        if (rest == NULL) {
            return;
        }
        rest++;
    } else {
        while (*rest != '\0' && *rest != ' ' && *rest != '\t') {
            rest++;
        }
    }
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    if (*rest != '\0') {
        AppendArg(cmd, cmdSize, rest);
    }
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

    ipc->lockEvent = CreateEventA(NULL, TRUE, FALSE, STEAM_IPC_EVENT_NAME);
    if (ipc->lockEvent == NULL) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "Unable to CreateEvent: %i", (int)GetLastError());
        Fail(msg);
        UnmapViewOfFile(ipc->view);
        CloseHandle(ipc->mapping);
        memset(ipc, 0, sizeof(*ipc));
        return 0;
    }

    /* Real Steam signals this once its bootstrap/IPC state is ready; code
     * that waits on it (steam_api/steamclient init) blocks until it is set.
     * We are the "Steam is up" stand-in, so signal it immediately -- manual-
     * reset so every later waiter (not just the first) sees it as ready. */
    SetEvent(ipc->lockEvent);

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
#ifndef REVLOADER_STANDALONE
        if (procName[0] == '\0') {
            Fail("ProcName value not found on command line or in rev.ini. Please edit the file.");
            return 1;
        }
#endif
    }

#ifndef REVLOADER_STANDALONE
    if (extraArgs[0] != '\0') {
        AppendArg(procName, sizeof(procName), extraArgs);
    }
#endif

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

#ifdef REVLOADER_STANDALONE
    {
        char engineCmd[4096];
        char rawCmdLine[4096];
        int rc;

        _snprintf(rawCmdLine, sizeof(rawCmdLine), "%s", GetCommandLineA());
        rawCmdLine[sizeof(rawCmdLine) - 1] = '\0';

        _snprintf(engineCmd, sizeof(engineCmd), "%s", GetCommandLineA());
        engineCmd[sizeof(engineCmd) - 1] = '\0';
        if (procName[0] != '\0' && !HasArg(engineCmd, "-game")) {
            AppendLaunchTail(engineCmd, sizeof(engineCmd), procName);
        }
        if (!HasArg(engineCmd, "-game")) {
            AppendArg(engineCmd, sizeof(engineCmd), "-game cstrike");
        }
#ifdef REVLOADER_LAUNCHER_DLLS
        if (!AppendDllsFromIni(engineCmd, sizeof(engineCmd), iniPath)) {
            WriteSteamAppId(dir, appId);
            TeardownSteamIpc(&ipc);
            return 1;
        }
#endif

        /* If this process's own OS-level command line has no -game (the
         * normal case: user double-clicked cstrike.exe with no arguments),
         * running the engine in this same process leaves GetCommandLineA()
         * without -game. Code inside hw.dll/steam_api that reads the real
         * process command line directly -- not the string we pass to
         * engine->Run() below -- then sees no mod and falls back to the
         * base game's "valve" paths for some things (observed: downloaded
         * maps landing in valve_downloads instead of cstrike_downloads,
         * so the engine never finds them and re-downloads every time).
         * Fix: re-exec ourselves with the real, full command line so the
         * process that actually runs the engine has -game in its true
         * argv, and just wait for that one. */
        if (!HasArg(rawCmdLine, "-game")) {
            char selfPath[MAX_PATH];
            char selfCmd[4096];

            GetModuleFileNameA(NULL, selfPath, sizeof(selfPath));
            selfPath[sizeof(selfPath) - 1] = '\0';

            _snprintf(selfCmd, sizeof(selfCmd), "\"%s\"", selfPath);
            selfCmd[sizeof(selfCmd) - 1] = '\0';
            AppendLaunchTail(selfCmd, sizeof(selfCmd), engineCmd);

            rc = RunChild(selfCmd) ? 0 : 1;
            WriteSteamAppId(dir, appId);
            TeardownSteamIpc(&ipc);
            return rc;
        }

        rc = HlLauncher_Run(instance, engineCmd);
        WriteSteamAppId(dir, appId);
        TeardownSteamIpc(&ipc);
        return rc;
    }
#else
    if (procName[0] == '\0') {
        Fail("ProcName value not found on command line or in rev.ini. Please edit the file.");
        WriteSteamAppId(dir, appId);
        TeardownSteamIpc(&ipc);
        return 1;
    }
#ifdef REVLOADER_LAUNCHER_DLLS
    if (!AppendDllsFromIni(procName, sizeof(procName), iniPath)) {
        WriteSteamAppId(dir, appId);
        TeardownSteamIpc(&ipc);
        return 1;
    }
#endif
    if (!RunChild(procName)) {
        WriteSteamAppId(dir, appId);
        TeardownSteamIpc(&ipc);
        return 1;
    }

    WriteSteamAppId(dir, appId);
    TeardownSteamIpc(&ipc);
    return 0;
#endif
}
