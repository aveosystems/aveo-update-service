#ifndef SERVICE_INSTALL_H_
#define SERVICE_INSTALL_H_

#include <memory>

#define SVC_DISPLAY_NAME L"Aveo Systems Update Service"
#define SVC_DESCRIPTION L"This service supports automatic updates for Mira Connect."

enum SvcInstallAction { UpgradeSvc, InstallSvc, ForceInstallSvc };
BOOL SvcInstall(SvcInstallAction action);
BOOL SvcUninstall();
BOOL StopService();
BOOL SetUserAccessServiceDACL(SC_HANDLE hService);
DWORD SetUserAccessServiceDACL(SC_HANDLE hService, PACL& pNewAcl,
                               PSECURITY_DESCRIPTOR psd);

struct ServiceHandleDeleter {
    typedef SC_HANDLE pointer;
    void operator()(SC_HANDLE handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseServiceHandle(handle);
        }
    }
};
typedef std::unique_ptr<SC_HANDLE, ServiceHandleDeleter> autoServiceHandle;

struct FreeSidDeleter {
    void operator()(void* aPtr) { ::FreeSid(aPtr); }
};
typedef std::unique_ptr<void, FreeSidDeleter> UniqueSidPtr;

#endif // SERVICE_INSTALL_H_