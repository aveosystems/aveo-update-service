// Start the AveoSystemsUpdate service and pass in the path to the updater file to execute

#include <iostream>
#include <windows.h>
#include <shlwapi.h>

#include "serviceinstall.h"
#include "updatecommon.h"

#define ERROR_NOT_ENOUGH_ARGS -1
#define ERROR_UPDATER_PATH_INVALID -2
#define ERROR_REGISTRY_KEY_INVALID -3
#define ERROR_REGISTRY_PATH_INVALID -4
#define ERROR_SERVICE_ALREADY_STARTED -5

void log(const wchar_t* msg) {
    std::wcout << msg << L"\n";
}

void logError(const wchar_t* msg) {
    std::wcerr << msg << L"\n";
}

void logError(const wchar_t* msg, LONG error) {
    std::wcerr << msg << L"  (" << error << L")" << L"\n";
}

DWORD logLastError(const wchar_t* msg) {
    std::wcerr << msg << L" (" << GetLastError() << L")\n";
    return GetLastError();
}

int wmain(int argc, wchar_t* argv[])
{
    SERVICE_STATUS_PROCESS ssStatus;
    DWORD dwBytesNeeded;

    if (argc < 3) {
        logError(L"Not enough arguments");
        return ERROR_NOT_ENOUGH_ARGS;
    }

    if (!IsValidFullPath(argv[1])) {
       std::wcerr << argv[1] << L" is not a valid full path\n";
       return ERROR_UPDATER_PATH_INVALID;
    }

    std::wcout << L"Updater path: " << argv[1] << L"\n";
    std::wcout << L"Registry key: " << argv[2] << L"\n";

    // Look in supplied registry key to determine existing installation.
    // We'll force the 64-bit view of the registry just in case we ever
    // have a 32-bit version of the app installed on the same machine.
    // The KEY_WOW64_64KEY flag is ignored on 32-bit machines.
    wchar_t installPath[MAX_PATH + 1];
    HKEY baseKey;
    LONG retCode = RegOpenKeyExW(HKEY_LOCAL_MACHINE, argv[2], 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &baseKey);
    if (retCode != ERROR_SUCCESS) {
        logError(L"Could not open registry key.", retCode);
        return ERROR_REGISTRY_KEY_INVALID;
    }
    else {
        DWORD regValType;
        DWORD regPathSize = MAX_PATH + 1;
        retCode = RegGetValueW(baseKey, nullptr, nullptr, RRF_RT_REG_SZ, 
            &regValType, installPath, &regPathSize);
        if (retCode != ERROR_SUCCESS) {
            logError(L"Could not get registry key value.", retCode);
            RegCloseKey(baseKey);
            return ERROR_REGISTRY_KEY_INVALID;
        }
        else {
            std::wcout << L"Registry path = " << installPath << L"\n";
            if (!IsValidFullPath(installPath)) {
                std::wcerr << installPath << L" is not a valid full path\n";
                RegCloseKey(baseKey);
                return ERROR_REGISTRY_PATH_INVALID;
            }
        }
        RegCloseKey(baseKey);
    }

    autoServiceHandle schSCManager(
        OpenSCManager(nullptr, nullptr,
            SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
    if (!schSCManager.get()) {
        return logLastError(L"Could not open service manager");
    }

    // permissions requested must match those in serviceinstall.cpp exactly
    autoServiceHandle schService(OpenService(
        schSCManager.get(), L"AveoSystemsUpdate",
        SERVICE_START | SERVICE_STOP | GENERIC_READ));
    if (!schService.get())
    {
        return logLastError(L"Could not open update service");
    }

    if (!QueryServiceStatusEx(
        schService.get(),
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&ssStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &dwBytesNeeded))
    {
        return logLastError(L"Could not query service status");
    }

    if (ssStatus.dwCurrentState != SERVICE_STOPPED &&
        ssStatus.dwCurrentState != SERVICE_STOP_PENDING)
    {
        logError(L"Could not start the service because it is already started");
        return ERROR_SERVICE_ALREADY_STARTED;
    }

    const wchar_t* args[] = {
        L"software-update",
        argv[1],
        installPath
    };

    // Wait at most 5 seconds trying to start the service in case of errors
    // like ERROR_SERVICE_DATABASE_LOCKED or ERROR_SERVICE_REQUEST_TIMEOUT.
    const DWORD maxWaitMS = 5000;
    DWORD currentWaitMS = 0;
    DWORD lastError = ERROR_SUCCESS;
    while (currentWaitMS < maxWaitMS) {
        BOOL result = StartServiceW(schService.get(), 3, args);
        if (result) {
            lastError = ERROR_SUCCESS;
            break;
        }
        else {
            lastError = GetLastError();
        }
        Sleep(100);
        currentWaitMS += 100;
    }

    if (lastError != ERROR_SUCCESS)
    {
        return logLastError(L"Start service failed");
    }

    log(L"Service start pending...");
    return lastError;
}