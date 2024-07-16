/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <shlobj.h>
#include <shlwapi.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <shellapi.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "rpcrt4.lib")

#include "workmonitor.h"
#include "serviceinstall.h"
#include "updatecommon.h"
#include "servicebase.h"
#include "registrycertificates.h"
#include "uachelper.h"
#include "updatehelper.h"
#include "pathhash.h"
#include "updatererrors.h"
#include "updateutils_win.h"

// Wait 15 minutes for an update operation to run at most.
// Updates usually take less than a minute so this seems like a
// significantly large and safe amount of time to wait.
static const int TIME_TO_WAIT_ON_UPDATER = 15 * 60 * 1000;
BOOL PathGetSiblingFilePath(LPWSTR destinationBuffer, LPCWSTR siblingFilePath,
                            LPCWSTR newFileName);

/**
 * Gets the installation directory from the arguments passed to updater.exe.
 *
 * @param argcTmp    The argc value normally sent to updater.exe
 * @param argvTmp    The argv value normally sent to updater.exe
 * @param aResultDir Buffer to hold the installation directory.
 */
static BOOL GetInstallationDir(int argcTmp, LPWSTR* argvTmp,
                               WCHAR aResultDir[MAX_PATH + 1]) {
  if (argcTmp < 2) {
    return FALSE;
  }

  wcsncpy_s(aResultDir, MAX_PATH + 1, argvTmp[1], MAX_PATH);
  WCHAR* backSlash = wcsrchr(aResultDir, L'\\');
  // Make sure that the path does not include trailing backslashes
  if (backSlash && (backSlash[1] == L'\0')) {
    *backSlash = L'\0';
  }

  return TRUE;
}

/**
 * Runs an update process as the service using the SYSTEM account.
 *
 * @param  argc           The number of arguments in argv
 * @param  argv           The arguments normally passed to updater.exe
 *                        argv[0] must be the path to updater.exe
 * @param  processStarted Set to TRUE if the process was started.
 * @return TRUE if the update process was run had a return code of 0.
 */
BOOL StartUpdateProcess(int argc, LPWSTR* argv, LPCWSTR installDir,
                        BOOL& processStarted) {
  processStarted = FALSE;

  LOG(("Starting update process as the service in session 0."));
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;

  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);
  si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\Default");  // -Wwritable-strings

  // The updater command line is of the form:
  // updater.exe /S /D=<install path>

  wchar_t switches[7] = L"/S /D=";
  auto args = MakeCommandLine(argc - 1, argv + 1);

  wchar_t format[] = L"%ls %ls%ls";
  // + 1 to allocated length for terminating null and + 1 for space between
  size_t len = wcslen(argv[0]) + wcslen(switches) + wcslen(args.get()) + 2;
  auto cmdLine = std::make_unique<wchar_t[]>(len);
  mywcsprintf(cmdLine.get(), len, format, argv[0], switches, args.get());

  // Setting the desktop to blank will ensure no GUI is displayed
  si.lpDesktop = const_cast<LPWSTR>(L"");  // -Wwritable-strings
  si.dwFlags |= STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;

  LOG(("Starting %ls with cmdline: %ls", argv[0], cmdLine.get()));
  processStarted =
      CreateProcessW(argv[0], cmdLine.get(), nullptr, nullptr, FALSE,
                     CREATE_DEFAULT_ERROR_MODE, nullptr, nullptr, &si, &pi);

  BOOL updateWasSuccessful = FALSE;
  if (processStarted) {
    BOOL processTerminated = FALSE;
    BOOL noProcessExitCode = FALSE;
    // Wait for the updater process to finish
    LOG(("Process was started... waiting on result."));
    DWORD waitRes = WaitForSingleObject(pi.hProcess, TIME_TO_WAIT_ON_UPDATER);
    if (WAIT_TIMEOUT == waitRes) {
      // We waited a long period of time for updater.exe and it never finished
      // so kill it.
      TerminateProcess(pi.hProcess, 1);
      processTerminated = TRUE;
    } else {
      // Check the return code of updater.exe to make sure we get 0
      DWORD returnCode;
      if (GetExitCodeProcess(pi.hProcess, &returnCode)) {
        LOG(("Process finished with return code %lu.", returnCode));
        // updater returns 0 if successful.
        updateWasSuccessful = (returnCode == 0);
      } else {
        LOG_WARN(("Process finished but could not obtain return code."));
        noProcessExitCode = TRUE;
      }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    DWORD lastError = GetLastError();
    LOG_WARN(
        ("Could not create process as current user, "
         "updaterPath: %ls; cmdLine: %ls.  (%lu)",
         argv[0], cmdLine.get(), lastError));
  }

  return updateWasSuccessful;
}

/**
 * Validates a file as an official updater.
 *
 * @param updater     Path to the updater to validate
 * @param installDir  Path to the application installation
 *                    being updated
 *
 * @return true if updater is the path to a valid updater
 */
static bool UpdaterIsValid(LPWSTR updater, LPWSTR installDir) {
    LOG(("Checking updater validity: %ls", updater));
  // Make sure the path to the updater to use for the update is local.
  // We do this check to make sure that file locking is available for
  // race condition security checks.
  BOOL isLocal = FALSE;
  if (!IsLocalFile(updater, isLocal) || !isLocal) {
    LOG_WARN(("Filesystem in path %ls is not supported (%lu)", updater,
              GetLastError()));
    return false;
  }

  autoHandle noWriteLock(CreateFileW(updater, GENERIC_READ, FILE_SHARE_READ,
                                       nullptr, OPEN_EXISTING, 0, nullptr));
  if (INVALID_HANDLE_VALUE == noWriteLock.get()) {
    LOG_WARN(("Could not set no write sharing access on file: %ls  (%lu)",
              updater, GetLastError()));
    return false;
  }

  // Check to make sure the updater.exe module has the unique updater identity.
  // This is a security measure to make sure that the signed executable that
  // we will run is actually an updater.
  bool result = true;
  autoModuleHandle updaterModule(LoadLibraryEx(updater, nullptr, LOAD_LIBRARY_AS_DATAFILE));
  if (!updaterModule.get()) {
    LOG_WARN(("updater.exe module could not be loaded. (%lu)", GetLastError()));
    return false;
  }
  HRSRC hRes = FindResourceA(
      updaterModule.get(),
      MAKEINTRESOURCEA(IDS_UPDATER_IDENTITY),
      MAKEINTRESOURCEA(IDS_UPDATER_IDENTITY));
  if (!hRes) {
      LOG_WARN(("Error finding installer identity  (%ld)", GetLastError()));
      return false;
  }
  HGLOBAL hResInfo = LoadResource(updaterModule.get(), hRes);
  if (!hResInfo) {
      LOG_WARN(("Error loading installer identity  (%ld)", GetLastError()));
      return false;
  }
  DWORD size = SizeofResource(updaterModule.get(), hRes);
  if (size == 0) {
      LOG_WARN(("Error getting size of installer identity  (%ld)", GetLastError()));
      return false;
  }
  LPVOID hResData = LockResource(hResInfo);
  if (!hResData)
  {
      LOG_WARN(("Error locking installer identity   (%ld)", GetLastError()));
      return false;
  }
  auto charData = std::make_unique<char[]>(size);
  charData.get()[size - 1] = '\0';
  DWORD i = 0;
  while (i < size) {
      charData.get()[i] = static_cast<char*>(hResData)[i];
      i++;
  }
  if (strcmp(charData.get(), UPDATER_IDENTITY_STRING)) {
      LOG_WARN(("The updater.exe identity string is not valid."));
      return false;
  }

  LOG(("The updater.exe application contains the Aveo Systems updater identity."));
  
  return DoesBinaryMatchAllowedCertificates(installDir, updater);
}

/**
 * Processes a software update command
 *
 * @param  argc           The number of arguments in argv
 * @param  argv           The arguments normally passed to updater.exe
 *                        argv[0] must be the path to updater.exe
 *
 * @return TRUE if the update was successful.
 */
BOOL ProcessSoftwareUpdateCommand(DWORD argc, LPWSTR* argv) {
  BOOL result = TRUE;
  if (argc < 2) {
    LOG_WARN(("Not enough command line parameters specified."));
    return FALSE;
  }

  WCHAR installDir[MAX_PATH + 1] = {L'\0'};
  if (!GetInstallationDir(argc, argv, installDir)) {
    LOG_WARN(("Could not get the installation directory"));
    return FALSE;
  }

  if (UpdaterIsValid(argv[0], installDir)) {
    BOOL updateProcessWasStarted = FALSE;
    if (StartUpdateProcess(argc, argv, installDir, updateProcessWasStarted)) {
      LOG(("updater.exe was launched and run successfully!"));
      LogFlush();

      // We might not execute code after StartServiceUpdate because
      // the service installer will stop the service if it is running.
      StartServiceUpdate();
    } else {
      result = FALSE;
      LOG_WARN(("Error running update process.  (%lu)",
                GetLastError()));
      LogFlush();
    }
  } else {
    result = FALSE;
    LOG_WARN(
        ("Could not start process due to certificate check error on "
         "updater.exe.  (%lu)",
         GetLastError()));
  }

  return result;
}

/**
 * Obtains the updater path alongside a subdir of the service binary.
 * The purpose of this function is to return a path that is likely high
 * integrity and therefore more safe to execute code from.
 *
 * @param serviceUpdaterPath Out parameter for the path where the updater
 *                           should be copied to.
 * @return TRUE if a file path was obtained.
 */
BOOL GetSecureUpdaterPath(WCHAR serviceUpdaterPath[MAX_PATH + 1]) {
  if (!GetModuleFileNameW(nullptr, serviceUpdaterPath, MAX_PATH)) {
    LOG_WARN(
        ("Could not obtain module filename when attempting to "
         "use a secure updater path.  (%lu)",
         GetLastError()));
    return FALSE;
  }

  if (!PathRemoveFileSpecW(serviceUpdaterPath)) {
    LOG_WARN(
        ("Couldn't remove file spec when attempting to use a secure "
         "updater path.  (%lu)",
         GetLastError()));
    return FALSE;
  }

  if (!PathAppendSafe(serviceUpdaterPath, L"update")) {
    LOG_WARN(
        ("Couldn't append file spec when attempting to use a secure "
         "updater path.  (%lu)",
         GetLastError()));
    return FALSE;
  }

  CreateDirectoryW(serviceUpdaterPath, nullptr);

  if (!PathAppendSafe(serviceUpdaterPath, L"updater.exe")) {
    LOG_WARN(
        ("Couldn't append file spec when attempting to use a secure "
         "updater path.  (%lu)",
         GetLastError()));
    return FALSE;
  }

  return TRUE;
}

/**
 * Deletes the passed in updater path and the associated updater.ini file.
 *
 * @param serviceUpdaterPath The path to delete.
 * @return TRUE if a file was deleted.
 */
BOOL DeleteSecureUpdater(WCHAR serviceUpdaterPath[MAX_PATH + 1]) {
  BOOL result = FALSE;
  if (serviceUpdaterPath[0]) {
    result = DeleteFileW(serviceUpdaterPath);
    if (!result && GetLastError() != ERROR_PATH_NOT_FOUND &&
        GetLastError() != ERROR_FILE_NOT_FOUND) {
      LOG_WARN(("Could not delete service updater path: '%ls'.",
                serviceUpdaterPath));
    }

    WCHAR updaterINIPath[MAX_PATH + 1] = {L'\0'};
    if (PathGetSiblingFilePath(updaterINIPath, serviceUpdaterPath,
                               L"updater.ini")) {
      result = DeleteFileW(updaterINIPath);
      if (!result && GetLastError() != ERROR_PATH_NOT_FOUND &&
          GetLastError() != ERROR_FILE_NOT_FOUND) {
        LOG_WARN(("Could not delete service updater INI path: '%ls'.",
                  updaterINIPath));
      }
    }
  }
  return result;
}

/**
 * Executes a service command.
 *
 * @param argc The number of arguments in argv
 * @param argv The service command line arguments, argv[0] is automatically
 *             included by Windows and argv[1] is the service command.
 *
 * @return FALSE if there was an error executing the service command.
 */
BOOL ExecuteServiceCommand(int argc, LPWSTR* argv) {
  for (int i = 0; i < argc; i++) {
      LOG(("arg[%d] = %ls", i, argv[i]));
  }
  if (argc < 2) {
    LOG_WARN(
        ("Not enough command line arguments to execute a service command"));
    return FALSE;
  }

  // The tests work by making sure the log has changed, so we put a
  // unique ID in the log.
  WCHAR uuidString[MAX_PATH + 1] = {L'\0'};
  if (GetUUIDString(uuidString)) {
    LOG(("Executing service command %ls, ID: %ls", argv[1], uuidString));
  } else {
    // The ID is only used by tests, so failure to allocate it isn't fatal.
    LOG(("Executing service command %ls", argv[1]));
  }

  BOOL result = FALSE;
  if (!lstrcmpi(argv[1], L"software-update")) {
    if (argc <= 3 || !IsValidFullPath(argv[3])
    ) {
      LOG_WARN(
          ("The install directory path is not valid for this application."));
      return FALSE;
    }

    // Use the passed in command line arguments for the path to updater.exe. 
    // Then we copy that updater.exe to the directory of the
    // update service so that a low integrity process cannot
    // replace the updater.exe at any point and use that for the update.
    // It also makes DLL injection attacks harder.
    WCHAR installDir[MAX_PATH + 1] = {L'\0'};
    if (!GetInstallationDir(argc - 2, argv + 2, installDir)) {
      LOG_WARN(("Could not get the installation directory"));
      return FALSE;
    }
    LOG(("installDir = %ls", installDir));

    if (!DoesFallbackKeyExist()) {
      WCHAR updateServiceKey[MAX_PATH + 1];
      if (CalculateRegistryPathFromFilePath(installDir,
                                            updateServiceKey)) {
        LOG(("Checking for update service registry key: '%ls'",
             updateServiceKey));
        HKEY baseKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, updateServiceKey, 0,
                          KEY_READ | KEY_WOW64_64KEY,
                          &baseKey) != ERROR_SUCCESS) {
          LOG_WARN(("The update service registry key does not exist."));
          return FALSE;
        }
        RegCloseKey(baseKey);
      } else {
        return FALSE;
      }
    }
    result = UpdaterIsValid(argv[2], installDir);

    if (result) {
      WCHAR secureUpdaterPath[MAX_PATH + 1] = {L'\0'};
      if (result) {
        result = GetSecureUpdaterPath(secureUpdaterPath);  // Does its own logging
      }
      if (result) {
        LOG(("Using this path for updating: %ls", secureUpdaterPath));
        DeleteSecureUpdater(secureUpdaterPath);
        result = CopyFileW(argv[2], secureUpdaterPath, FALSE);
      }

      if (!result) {
        LOG_WARN(
            ("Could not copy path to secure location.  (%lu)", GetLastError()));
      } else {
        // Verify that the updater.exe that we will be executing from the
        // secure path is the same as the source we copied from.
        BOOL updaterIsCorrect;
        if (!VerifySameFiles(argv[2], secureUpdaterPath, updaterIsCorrect)) {
            LOG_WARN(
                ("Error checking if the updaters are the same.\n"
                    "Path 1: %ls\nPath 2: %ls",
                    argv[2], secureUpdaterPath));
            return false;
        }

        if (!updaterIsCorrect) {
            LOG_WARN(
                ("The updaters do not match, updater will not run.\n"
                    "Path 1: %ls\nPath 2: %ls",
                    argv[2], secureUpdaterPath));
            return false;
        }

        LOG(
            ("updater.exe was compared successfully to the installation directory"
                " updater.exe."));

        // We obtained the path, copied it successfully, and verified the copy,
        // so update the path to use for the service update.
        argv[2] = secureUpdaterPath;
        result = ProcessSoftwareUpdateCommand(argc - 2, argv + 2);
        DeleteSecureUpdater(secureUpdaterPath);
      }
    }
    // We might not reach here if the service install succeeded
    // because the service self updates itself and the service
    // installer will stop the service.
  } else {
    LOG_WARN(("Service command not recognized: %ls.", argv[1]));
    // result is already set to FALSE
  }

  LOG(("Service command %ls complete with result: %ls.", argv[1],
       (result ? L"Success" : L"Failure")));
  return result;
}
