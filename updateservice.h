#ifndef UPDATE_SERVICE_H_
#define UPDATE_SERVICE_H_

#include <windows.h>

void WINAPI SvcMain(DWORD dwArgc, LPWSTR* lpszArgv);
void WINAPI SvcCtrlHandler(DWORD dwCtrl);
void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode,
    DWORD dwWaitHint);

#endif  // UPDATE_SERVICE_H_
