#include <windows.h>
#include <string>
#include <vector>

static const wchar_t* SERVICE_NAME = L"TrayIconPromoter";
static const wchar_t* SERVICE_DISPLAY = L"Tray Icon Promoter";
static const wchar_t* SERVICE_DESC = L"Ensures all system tray icons remain visible and are never hidden in the overflow menu.";

// Path appended to each user's SID key under HKEY_USERS
static const wchar_t* NOTIFY_SUBKEY = L"Control Panel\\NotifyIconSettings";

static const DWORD POLL_INTERVAL_MS = 10000; // 10 seconds

static SERVICE_STATUS        g_ServiceStatus = {};
static SERVICE_STATUS_HANDLE g_StatusHandle = nullptr;
static HANDLE                g_StopEvent = nullptr;

// -----------------------------------------------------------------------
// Returns true if a SID string looks like a real user account.
// Filters out .DEFAULT, S-1-5-18 (LocalSystem), S-1-5-19 (LocalService),
// S-1-5-20 (NetworkService), and "_Classes" suffix hives.
// -----------------------------------------------------------------------
static bool IsRealUserSid(const std::wstring& sid)
{
    if (sid.empty())                              return false;
    if (sid[0] != L'S')                           return false; // must start with S-
    if (sid == L".DEFAULT")                       return false;
    if (sid == L"S-1-5-18")                       return false;
    if (sid == L"S-1-5-19")                       return false;
    if (sid == L"S-1-5-20")                       return false;
    if (sid.size() > 8 &&
        sid.substr(sid.size() - 8) == L"_Classes") return false;
    return true;
}

// -----------------------------------------------------------------------
// Fix IsPromoted for every NotifyIconSettings subkey under one user hive.
// hiveRoot is an open handle to HKEY_USERS\<SID>
// -----------------------------------------------------------------------
static void PromoteIconsForUser(HKEY hiveRoot)
{
    HKEY hParent = nullptr;
    LONG ret = RegOpenKeyExW(hiveRoot, NOTIFY_SUBKEY, 0,
        KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hParent);
    if (ret != ERROR_SUCCESS) return; // key doesn't exist for this user yet

    wchar_t subkeyName[512];
    DWORD   index = 0;

    while (true)
    {
        DWORD nameLen = _countof(subkeyName);
        ret = RegEnumKeyExW(hParent, index++, subkeyName, &nameLen,
            nullptr, nullptr, nullptr, nullptr);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS)       continue;

        HKEY hSub = nullptr;
        if (RegOpenKeyExW(hParent, subkeyName, 0,
            KEY_READ | KEY_SET_VALUE, &hSub) != ERROR_SUCCESS)
            continue;

        DWORD value = 0;
        DWORD dataSize = sizeof(value);
        DWORD type = 0;
        LONG  qret = RegQueryValueExW(hSub, L"IsPromoted", nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &dataSize);

        if (qret != ERROR_SUCCESS || type != REG_DWORD || value != 1)
        {
            DWORD one = 1;
            RegSetValueExW(hSub, L"IsPromoted", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&one), sizeof(one));
        }

        RegCloseKey(hSub);
    }

    RegCloseKey(hParent);
}

// -----------------------------------------------------------------------
// Enumerate all loaded user hives under HKEY_USERS and promote each one.
// -----------------------------------------------------------------------
static void PromoteAllIcons()
{
    wchar_t sidName[256];
    DWORD   index = 0;

    while (true)
    {
        DWORD nameLen = _countof(sidName);
        LONG  ret = RegEnumKeyExW(HKEY_USERS, index++, sidName, &nameLen,
            nullptr, nullptr, nullptr, nullptr);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS)       continue;

        if (!IsRealUserSid(sidName)) continue;

        HKEY hUserHive = nullptr;
        if (RegOpenKeyExW(HKEY_USERS, sidName, 0,
            KEY_READ, &hUserHive) != ERROR_SUCCESS)
            continue;

        PromoteIconsForUser(hUserHive);
        RegCloseKey(hUserHive);
    }
}

// -----------------------------------------------------------------------
// Service control handler
// -----------------------------------------------------------------------
static VOID WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_StopEvent);
        break;
    default:
        break;
    }
}

// -----------------------------------------------------------------------
// Service main
// -----------------------------------------------------------------------
static VOID WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) return;

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwWin32ExitCode = NO_ERROR;
    g_ServiceStatus.dwCheckPoint = 1;
    g_ServiceStatus.dwWaitHint = 3000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    g_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_StopEvent)
    {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // Watch HKEY_USERS for any subkey/value changes (catches all user hives)
    HANDLE hRegEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (hRegEvent)
    {
        RegNotifyChangeKeyValue(HKEY_USERS,
            TRUE,  // watch entire subtree
            REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
            hRegEvent,
            TRUE); // asynchronous
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // Immediate pass on startup
    PromoteAllIcons();

    HANDLE waitHandles[2] = { g_StopEvent, hRegEvent };
    DWORD  handleCount = hRegEvent ? 2 : 1;

    while (true)
    {
        DWORD reason = WaitForMultipleObjects(handleCount, waitHandles,
            FALSE, POLL_INTERVAL_MS);
        if (reason == WAIT_OBJECT_0)
            break; // stop event

        // Registry changed or poll timer fired — fix everything
        PromoteAllIcons();

        // Re-arm the notification
        if (hRegEvent)
        {
            ResetEvent(hRegEvent);
            RegNotifyChangeKeyValue(HKEY_USERS,
                TRUE,
                REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                hRegEvent,
                TRUE);
        }
    }

    if (hRegEvent) CloseHandle(hRegEvent);
    CloseHandle(g_StopEvent);

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

// -----------------------------------------------------------------------
// Install / Uninstall
// -----------------------------------------------------------------------
static bool InstallService()
{
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;

    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) return false;

    SC_HANDLE hSvc = CreateServiceW(
        hSCM,
        SERVICE_NAME,
        SERVICE_DISPLAY,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        exePath,
        nullptr, nullptr, nullptr,
        nullptr, nullptr); // LocalSystem

    if (!hSvc) { CloseServiceHandle(hSCM); return false; }

    SERVICE_DESCRIPTIONW sd{};
    sd.lpDescription = const_cast<wchar_t*>(SERVICE_DESC);
    ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_DESCRIPTION, &sd);

    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 5000  },
        { SC_ACTION_RESTART, 10000 },
        { SC_ACTION_RESTART, 30000 }
    };
    SERVICE_FAILURE_ACTIONSW sfa{};
    sfa.dwResetPeriod = 86400;
    sfa.cActions = 3;
    sfa.lpsaActions = actions;
    ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);

    StartServiceW(hSvc, 0, nullptr);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return true;
}

static bool UninstallService()
{
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = OpenServiceW(hSCM, SERVICE_NAME,
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!hSvc) { CloseServiceHandle(hSCM); return false; }

    SERVICE_STATUS ss{};
    ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
    for (int i = 0; i < 50; i++)
    {
        QueryServiceStatus(hSvc, &ss);
        if (ss.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }

    DeleteService(hSvc);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return true;
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR lpCmdLine, int)
{
    std::wstring cmd(lpCmdLine ? lpCmdLine : L"");

    if (cmd.find(L"/install") != std::wstring::npos)
    {
        if (InstallService())
            MessageBoxW(nullptr, L"Service installed and started successfully.",
                SERVICE_DISPLAY, MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(nullptr, L"Failed to install service.\nRun as Administrator.",
                SERVICE_DISPLAY, MB_OK | MB_ICONERROR);
        return 0;
    }

    if (cmd.find(L"/uninstall") != std::wstring::npos)
    {
        if (UninstallService())
            MessageBoxW(nullptr, L"Service uninstalled successfully.",
                SERVICE_DISPLAY, MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(nullptr, L"Failed to uninstall service.\nRun as Administrator.",
                SERVICE_DISPLAY, MB_OK | MB_ICONERROR);
        return 0;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<wchar_t*>(SERVICE_NAME), ServiceMain },
        { nullptr, nullptr }
    };
    StartServiceCtrlDispatcherW(table);
    return 0;
}