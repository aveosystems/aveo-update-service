/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <windows.h>
#include <aclapi.h>
#include <stdlib.h>
#include <shlwapi.h>
#include <tlhelp32.h>

// Used for DNLEN and UNLEN
#include <lm.h>

//#include <nsWindowsHelpers.h>
//#include "mozilla/UniquePtr.h"

#include "serviceinstall.h"
//#include "servicebase.h"
#include "updatehelper.h"
//#include "shellapi.h"
//#include "readstrings.h"
//#include "updatererrors.h"
//#include "commonupdatedir.h"
#include "updatecommon.h"

#pragma comment(lib, "version.lib")

// This uninstall key is defined originally in updateservice_installer.nsi
#define MAINT_UNINSTALL_KEY                                                    \
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AveoSystemsUpdateService"

/**
 * Obtains the version number from the specified PE file's version information
 * Version Format: A.B.C.D (Example 10.0.0.300)
 *
 * @param  path The path of the file to check the version on
 * @param  A    The first part of the version number
 * @param  B    The second part of the version number
 * @param  C    The third part of the version number
 * @param  D    The fourth part of the version number
 * @return TRUE if successful
 */
static BOOL GetVersionNumberFromPath(LPWSTR path, DWORD& A, DWORD& B, DWORD& C,
                                     DWORD& D) {
  DWORD fileVersionInfoSize = GetFileVersionInfoSizeW(path, 0);
  std::unique_ptr<char[]> fileVersionInfo(new char[fileVersionInfoSize]);
  if (!GetFileVersionInfoW(path, 0, fileVersionInfoSize,
                           fileVersionInfo.get())) {
    LOG_WARN(
        ("Could not obtain file info of old service.  (%lu)", GetLastError()));
    return FALSE;
  }

  VS_FIXEDFILEINFO* fixedFileInfo =
      reinterpret_cast<VS_FIXEDFILEINFO*>(fileVersionInfo.get());
  UINT size;
  if (!VerQueryValueW(fileVersionInfo.get(), L"\\",
                      reinterpret_cast<LPVOID*>(&fixedFileInfo), &size)) {
    LOG_WARN(("Could not query file version info of old service.  (%lu)",
              GetLastError()));
    return FALSE;
  }

  A = HIWORD(fixedFileInfo->dwFileVersionMS);
  B = LOWORD(fixedFileInfo->dwFileVersionMS);
  C = HIWORD(fixedFileInfo->dwFileVersionLS);
  D = LOWORD(fixedFileInfo->dwFileVersionLS);
  return TRUE;
}


/**
 * Installs or upgrades the SVC_NAME service.
 * If an existing service is already installed, we replace it with the
 * currently running process.
 *
 * @param  action The action to perform.
 * @return TRUE if the service was installed/upgraded
 */
BOOL SvcInstall(SvcInstallAction action) {
  // Get a handle to the local computer SCM database with full access rights.
  autoServiceHandle schSCManager(
      OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
  if (!schSCManager.get()) {
    LOG_WARN(("Could not open service manager.  (%lu)", GetLastError()));
    return FALSE;
  }

  WCHAR newServiceBinaryPath[MAX_PATH + 1];
  if (!GetModuleFileNameW(
          nullptr, newServiceBinaryPath,
          sizeof(newServiceBinaryPath) / sizeof(newServiceBinaryPath[0]))) {
    LOG_WARN(
        ("Could not obtain module filename when attempting to "
         "install service.  (%lu)",
         GetLastError()));
    return FALSE;
  }

  // Check if we already have the service installed.
  autoServiceHandle schService(
      OpenServiceW(schSCManager.get(), SVC_NAME, SERVICE_ALL_ACCESS));
  DWORD lastError = GetLastError();
  if (!schService.get() && ERROR_SERVICE_DOES_NOT_EXIST != lastError) {
    // The service exists but we couldn't open it
    LOG_WARN(("Could not open service.  (%lu)", GetLastError()));
    return FALSE;
  }

  if (schService.get()) {
    // The service exists but it may not have the correct permissions.
    // This could happen if the permissions were not set correctly originally
    // or have been changed after the installation.  This will reset the
    // permissions back to allow limited user accounts.
    if (!SetUserAccessServiceDACL(schService.get())) {
      LOG_WARN(
          ("Could not reset security ACE on service handle. It might not be "
           "possible to start the service. This error should never "
           "happen.  (%lu)",
           GetLastError()));
    }

    // The service exists and we opened it
    DWORD bytesNeeded;
    if (!QueryServiceConfigW(schService.get(), nullptr, 0, &bytesNeeded) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      LOG_WARN(
          ("Could not determine buffer size for query service config.  (%lu)",
           GetLastError()));
      return FALSE;
    }

    // Get the service config information, in particular we want the binary
    // path of the service.
    std::unique_ptr<char[]> serviceConfigBuffer(new char[bytesNeeded]);
    if (!QueryServiceConfigW(
            schService.get(),
            reinterpret_cast<QUERY_SERVICE_CONFIGW*>(serviceConfigBuffer.get()),
            bytesNeeded, &bytesNeeded)) {
      LOG_WARN(("Could open service but could not query service config.  (%lu)",
                GetLastError()));
      return FALSE;
    }
    QUERY_SERVICE_CONFIGW& serviceConfig =
        *reinterpret_cast<QUERY_SERVICE_CONFIGW*>(serviceConfigBuffer.get());

    // Ensure the service path is not quoted. We own this memory and know it to
    // be large enough for the quoted path, so it is large enough for the
    // unquoted path.  This function cannot fail.
    PathUnquoteSpacesW(serviceConfig.lpBinaryPathName);

    LOG(("new service path = %ls", newServiceBinaryPath));
    LOG(("existing service path = %ls", serviceConfig.lpBinaryPathName));

    // Obtain the existing updateservice file's version number and
    // the new file's version number.  Versions are in the format of
    // A.B.C.D.
    DWORD existingA, existingB, existingC, existingD;
    DWORD newA, newB, newC, newD;
    BOOL obtainedExistingVersionInfo =
        GetVersionNumberFromPath(serviceConfig.lpBinaryPathName, existingA,
                                 existingB, existingC, existingD);
    if (!GetVersionNumberFromPath(newServiceBinaryPath, newA, newB, newC,
                                  newD)) {
      LOG_WARN(("Could not obtain version number from new path"));
      return FALSE;
    }

    LOG(("new service version = %ld.%ld.%ld.%ld", newA, newB, newC, newD));
    LOG(("existing service version = %ld.%ld.%ld.%ld", existingA, existingB, existingC, existingD));

    // Check if we need to replace the old binary with the new one
    // If we couldn't get the old version info then we assume we should
    // replace it.
    if (ForceInstallSvc == action || !obtainedExistingVersionInfo ||
        (existingA < newA) || (existingA == newA && existingB < newB) ||
        (existingA == newA && existingB == newB && existingC < newC) ||
        (existingA == newA && existingB == newB && existingC == newC &&
         existingD < newD)) {
      schService.reset();
      if (!StopService()) {
        return FALSE;
      }

      if (!wcscmp(newServiceBinaryPath, serviceConfig.lpBinaryPathName)) {
        LOG(
            ("File is already in the correct location, no action needed for "
             "upgrade.  The path is: \"%ls\"",
             newServiceBinaryPath));
        return TRUE;
      }

      BOOL result = TRUE;

      // Attempt to copy the new binary over top the existing binary.
      // If there is an error we try to move it out of the way and then
      // copy it in.  First try the safest / easiest way to overwrite the file.
      if (!CopyFileW(newServiceBinaryPath, serviceConfig.lpBinaryPathName,
                     FALSE)) {
        LOG_WARN(
            ("Could not overwrite old service binary file.  (%lu)",
             GetLastError()));

        // We rename the last 3 filename chars in an unsafe way.  Manually
        // verify there are more than 3 chars for safe failure in MoveFileExW.
        const size_t len = wcslen(serviceConfig.lpBinaryPathName);
        if (len > 3) {
          // Calculate the temp file path that we're moving the file to. This
          // is the same as the proper service path but with a .old extension.
          LPWSTR oldServiceBinaryTempPath = new WCHAR[len + 1];
          memset(oldServiceBinaryTempPath, 0, (len + 1) * sizeof(WCHAR));
          wcsncpy_s(oldServiceBinaryTempPath, len + 1, serviceConfig.lpBinaryPathName,
                  len);
          // Rename the last 3 chars to 'old'
          wcsncpy_s(oldServiceBinaryTempPath + len - 3, 4, L"old", 3);

          // Move the current (old) service file to the temp path.
          if (MoveFileExW(serviceConfig.lpBinaryPathName,
                          oldServiceBinaryTempPath,
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // The old binary is moved out of the way, copy in the new one.
            if (!CopyFileW(newServiceBinaryPath, serviceConfig.lpBinaryPathName,
                           FALSE)) {
              // It is best to leave the old service binary in this condition.
              LOG_WARN(
                  ("The new service binary could not be copied in."
                   " The service will not be upgraded."));
              result = FALSE;
            } else {
              LOG(
                  ("The new service binary was copied in by first moving the"
                   " old one out of the way."));
            }

            // Attempt to get rid of the old service temp path.
            if (DeleteFileW(oldServiceBinaryTempPath)) {
              LOG(("The old temp service path was deleted: %ls.",
                   oldServiceBinaryTempPath));
            } else {
              // The old temp path could not be removed.  It will be removed
              // the next time the user can't copy the binary in or on
              // uninstall.
              LOG_WARN(("The old temp service path was not deleted."));
            }
          } else {
            // It is best to leave the old service binary in this condition.
            LOG_WARN(
                ("Could not move old service file out of the way from:"
                 " \"%ls\" to \"%ls\". Service will not be upgraded.  (%lu)",
                 serviceConfig.lpBinaryPathName, oldServiceBinaryTempPath,
                 GetLastError()));
            result = FALSE;
          }
          delete[] oldServiceBinaryTempPath;
        } else {
          // It is best to leave the old service binary in this condition.
          LOG_WARN(
              ("Service binary path was less than 3, service will"
               " not be updated.  This should never happen."));
          result = FALSE;
        }
      } else {
        LOG(("The new service binary was copied in."));
      }

      // We made a copy of ourselves to the existing location.
      // The tmp file (the process of which we are executing right now) will be
      // left over.  Attempt to delete the file on the next reboot.
      if (MoveFileExW(newServiceBinaryPath, nullptr,
                      MOVEFILE_DELAY_UNTIL_REBOOT)) {
        LOG(("Deleting the old file path on the next reboot: %ls.",
             newServiceBinaryPath));
      } else {
        LOG_WARN(("Call to delete the old file path failed: %ls.",
                  newServiceBinaryPath));
      }

      return result;
    }

    // We don't need to copy ourselves to the existing location.
    // The tmp file (the process of which we are executing right now) will be
    // left over.  Attempt to delete the file on the next reboot.
    MoveFileExW(newServiceBinaryPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    // nothing to do, we already have a newer service installed
    return TRUE;
  }

  // If the service does not exist and we are upgrading, don't install it.
  if (UpgradeSvc == action) {
    // The service does not exist and we are upgrading, so don't install it
    return TRUE;
  }

  // Quote the path only if it contains spaces.
  PathQuoteSpacesW(newServiceBinaryPath);
  // The service does not already exist so create the service as on demand
  schService.reset(CreateServiceW(
      schSCManager.get(), SVC_NAME, SVC_DISPLAY_NAME, SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
      newServiceBinaryPath, nullptr, nullptr, nullptr, nullptr, nullptr));
  if (!schService) {
    LOG_WARN(
        ("Could not create Windows service. "
         "This error should never happen since a service install "
         "should only be called when elevated.  (%lu)",
         GetLastError()));
    return FALSE;
  }

  SERVICE_DESCRIPTION description;
  description.lpDescription = const_cast<LPWSTR>(SVC_DESCRIPTION);
  if (!ChangeServiceConfig2W(schService.get(),
      SERVICE_CONFIG_DESCRIPTION, &description)) {
      // This shouldn't fail, but it's not fatal if it does
      LOG_WARN(("Could not change service description.  (%lu)",
          GetLastError()));
  }

  if (!SetUserAccessServiceDACL(schService.get())) {
    LOG_WARN(
        ("Could not set security ACE on service handle, the service will not "
         "be able to be started from unelevated processes. "
         "This error should never happen.  (%lu)",
         GetLastError()));
  }

  return TRUE;
}

/**
 * Stops the update service.
 *
 * @return TRUE if successful.
 */
BOOL StopService() {
  // Get a handle to the local computer SCM database with full access rights.
  autoServiceHandle schSCManager(
      OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
  if (!schSCManager) {
    LOG_WARN(("Could not open service manager.  (%lu)", GetLastError()));
    return FALSE;
  }

  // Open the service
  autoServiceHandle schService(
      OpenServiceW(schSCManager.get(), SVC_NAME, SERVICE_ALL_ACCESS));
  if (!schService) {
    LOG_WARN(("Could not open service.  (%lu)", GetLastError()));
    return FALSE;
  }

  LOG(("Sending stop request..."));
  SERVICE_STATUS status;
  SetLastError(ERROR_SUCCESS);
  if (!ControlService(schService.get(), SERVICE_CONTROL_STOP, &status) &&
      GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
    LOG_WARN(("Error sending stop request.  (%lu)", GetLastError()));
  }

  schSCManager.reset();
  schService.reset();

  LOG(("Waiting for service stop..."));
  DWORD lastState = WaitForServiceStop(SVC_NAME, 30);

  // The service can be in a stopped state but the exe still in use
  // so make sure the process is really gone before proceeding
  WaitForProcessExit(L"updateservice.exe", 30);
  LOG(("Done waiting for service stop, last service state: %lu", lastState));

  return lastState == SERVICE_STOPPED;
}

/**
 * Uninstalls the update service.
 *
 * @return TRUE if successful.
 */
BOOL SvcUninstall() {
  // Get a handle to the local computer SCM database with full access rights.
  autoServiceHandle schSCManager(
      OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
  if (!schSCManager) {
    LOG_WARN(("Could not open service manager.  (%lu)", GetLastError()));
    return FALSE;
  }

  // Open the service
  autoServiceHandle schService(
      OpenServiceW(schSCManager.get(), SVC_NAME, SERVICE_ALL_ACCESS));
  if (!schService) {
    LOG_WARN(("Could not open service.  (%lu)", GetLastError()));
    return FALSE;
  }

  // Stop the service so it deletes faster and so the uninstaller
  // can actually delete its EXE.
  DWORD totalWaitTime = 0;
  SERVICE_STATUS status;
  static const int maxWaitTime = 1000 * 60;  // Never wait more than a minute
  if (ControlService(schService.get(), SERVICE_CONTROL_STOP, &status)) {
    do {
      Sleep(status.dwWaitHint);
      totalWaitTime += (status.dwWaitHint + 10);
      if (status.dwCurrentState == SERVICE_STOPPED) {
        break;
      } else if (totalWaitTime > maxWaitTime) {
        break;
      }
    } while (QueryServiceStatus(schService.get(), &status));
  }

  // Delete the service or mark it for deletion
  BOOL deleted = DeleteService(schService.get());
  if (!deleted) {
    deleted = (GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE);
  }

  return deleted;
}

/**
 * Sets the access control list for user access for the specified service.
 *
 * @param  hService The service to set the access control list on
 * @return TRUE if successful
 */
BOOL SetUserAccessServiceDACL(SC_HANDLE hService) {
  PACL pNewAcl = nullptr;
  PSECURITY_DESCRIPTOR psd = nullptr;
  DWORD lastError = SetUserAccessServiceDACL(hService, pNewAcl, psd);
  if (pNewAcl) {
    LocalFree((HLOCAL)pNewAcl);
  }
  if (psd) {
    LocalFree((LPVOID)psd);
  }
  return ERROR_SUCCESS == lastError;
}

/**
 * Sets the access control list for user access for the specified service.
 *
 * @param  hService  The service to set the access control list on
 * @param  pNewAcl   The out param ACL which should be freed by caller
 * @param  psd       out param security descriptor, should be freed by caller
 * @return ERROR_SUCCESS if successful
 */
DWORD
SetUserAccessServiceDACL(SC_HANDLE hService, PACL& pNewAcl,
                         PSECURITY_DESCRIPTOR psd) {
  // Get the current security descriptor needed size
  DWORD needed = 0;
  if (!QueryServiceObjectSecurity(hService, DACL_SECURITY_INFORMATION, &psd, 0,
                                  &needed)) {
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      LOG_WARN(("Could not query service object security size.  (%lu)",
                GetLastError()));
      return GetLastError();
    }

    DWORD size = needed;
    psd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, size);
    if (!psd) {
      LOG_WARN(
          ("Could not allocate security descriptor.  (%lu)", GetLastError()));
      return ERROR_INSUFFICIENT_BUFFER;
    }

    // Get the actual security descriptor now
    if (!QueryServiceObjectSecurity(hService, DACL_SECURITY_INFORMATION, psd,
                                    size, &needed)) {
      LOG_WARN(
          ("Could not allocate security descriptor.  (%lu)", GetLastError()));
      return GetLastError();
    }
  }

  // Get the current DACL from the security descriptor.
  PACL pacl = nullptr;
  BOOL bDaclPresent = FALSE;
  BOOL bDaclDefaulted = FALSE;
  if (!GetSecurityDescriptorDacl(psd, &bDaclPresent, &pacl, &bDaclDefaulted)) {
    LOG_WARN(("Could not obtain DACL.  (%lu)", GetLastError()));
    return GetLastError();
  }

  PSID sidBuiltinUsers;
  DWORD SIDSize = SECURITY_MAX_SID_SIZE;
  sidBuiltinUsers = LocalAlloc(LMEM_FIXED, SIDSize);
  if (!sidBuiltinUsers) {
    LOG_WARN(("Could not allocate SID memory.  (%lu)", GetLastError()));
    return GetLastError();
  }
  UniqueSidPtr uniqueSidBuiltinUsers(sidBuiltinUsers);

  if (!CreateWellKnownSid(WinBuiltinUsersSid, nullptr, sidBuiltinUsers,
                          &SIDSize)) {
    DWORD lastError = GetLastError();
    LOG_WARN(("Could not create BI\\Users SID.  (%lu)", lastError));
    return lastError;
  }

  PSID sidInteractive;
  SIDSize = SECURITY_MAX_SID_SIZE;
  sidInteractive = LocalAlloc(LMEM_FIXED, SIDSize);
  if (!sidInteractive) {
    LOG_WARN(("Could not allocate SID memory.  (%lu)", GetLastError()));
    return GetLastError();
  }
  UniqueSidPtr uniqueSidInteractive(sidInteractive);

  if (!CreateWellKnownSid(WinInteractiveSid, nullptr, sidInteractive,
                          &SIDSize)) {
    DWORD lastError = GetLastError();
    LOG_WARN(("Could not create Interactive SID.  (%lu)", lastError));
    return lastError;
  }

  PSID sidLocalService;
  SIDSize = SECURITY_MAX_SID_SIZE;
  sidLocalService = LocalAlloc(LMEM_FIXED, SIDSize);
  if (!sidLocalService) {
      LOG_WARN(("Could not allocate SID memory.  (%lu)", GetLastError()));
      return GetLastError();
  }
  UniqueSidPtr uniqueSidLocalService(sidLocalService);

  if (!CreateWellKnownSid(WinLocalServiceSid, nullptr, sidLocalService,
      &SIDSize)) {
      DWORD lastError = GetLastError();
      LOG_WARN(("Could not create Local Service SID.  (%lu)", lastError));
      return lastError;
  }

  const size_t eaCount = 3;
  EXPLICIT_ACCESS ea[eaCount];
  ZeroMemory(ea, sizeof(ea));
  ea[0].grfAccessMode = REVOKE_ACCESS;
  ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea[0].Trustee.ptstrName = static_cast<LPWSTR>(sidBuiltinUsers);
  ea[1].grfAccessPermissions = SERVICE_START | SERVICE_STOP | GENERIC_READ;
  ea[1].grfAccessMode = SET_ACCESS;
  ea[1].grfInheritance = NO_INHERITANCE;
  ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea[1].Trustee.ptstrName = static_cast<LPWSTR>(sidInteractive);
  ea[2].grfAccessPermissions = SERVICE_START | SERVICE_STOP | GENERIC_READ;
  ea[2].grfAccessMode = SET_ACCESS;
  ea[2].grfInheritance = NO_INHERITANCE;
  ea[2].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea[2].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea[2].Trustee.ptstrName = static_cast<LPWSTR>(sidLocalService);

  DWORD lastError = SetEntriesInAclW(eaCount, ea, pacl, &pNewAcl);
  if (ERROR_SUCCESS != lastError) {
    LOG_WARN(("Could not set entries in ACL.  (%lu)", lastError));
    return lastError;
  }

  // Initialize a new security descriptor.
  SECURITY_DESCRIPTOR sd;
  if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
    LOG_WARN(
        ("Could not initialize security descriptor.  (%lu)", GetLastError()));
    return GetLastError();
  }

  // Set the new DACL in the security descriptor.
  if (!SetSecurityDescriptorDacl(&sd, TRUE, pNewAcl, FALSE)) {
    LOG_WARN(
        ("Could not set security descriptor DACL.  (%lu)", GetLastError()));
    return GetLastError();
  }

  // Set the new security descriptor for the service object.
  if (!SetServiceObjectSecurity(hService, DACL_SECURITY_INFORMATION, &sd)) {
    LOG_WARN(("Could not set object security.  (%lu)", GetLastError()));
    return GetLastError();
  }

  // Woohoo, raise the roof
  LOG(("User access was set successfully on the service."));
  return ERROR_SUCCESS;
}
