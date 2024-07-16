#if defined(XP_WIN)
#  include <windows.h>
#  include <shlobj.h>
#  ifndef RRF_SUBKEY_WOW6464KEY
#    define RRF_SUBKEY_WOW6464KEY 0x00010000
#  endif

// This struct isn't in any SDK header, so this definition was copied from:
// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/content/ntifs/ns-ntifs-_reparse_data_buffer
typedef struct _REPARSE_DATA_BUFFER {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    } DUMMYUNIONNAME;
} REPARSE_DATA_BUFFER, * PREPARSE_DATA_BUFFER;
#endif

#include <stdio.h>
#include <stdarg.h>
// Needed for PathAppendW
#include <shlwapi.h>

#include "updatecommon.h"
#ifdef XP_WIN
#  include "updatehelper.h"
#endif

UpdateLog::UpdateLog() : logFP(nullptr), mDstFilePath(L"\0") {}

void UpdateLog::Init(TCHAR* logFilePath) {
  if (logFP) {
    return;
  }

  // When the path is over the length limit disable logging by not opening the
  // file and not setting logFP.
  size_t dstFilePathLen = wcslen(logFilePath);
  if (dstFilePathLen > 0 && dstFilePathLen < MAXPATHLEN - 1) {
    wcsncpy_s(mDstFilePath, MAXPATHLEN, logFilePath, MAXPATHLEN - 1);
#if defined(XP_WIN) || defined(XP_MACOSX)
    _wfopen_s(&logFP, mDstFilePath, L"w");
#else
    // On platforms that have an updates directory in the installation directory
    // (e.g. platforms other than Windows and Mac) the update log is written to
    // a temporary file and then to the update log file. This is needed since
    // the installation directory is moved during a replace request. This can be
    // removed when the platform's updates directory is located outside of the
    // installation directory.
    logFP = tmpfile();
#endif
  }
}

void UpdateLog::Finish() {
  if (!logFP) {
    return;
  }

  fclose(logFP);
  logFP = nullptr;
}

void UpdateLog::Flush() {
  if (!logFP) {
    return;
  }

  fflush(logFP);
}

void UpdateLog::Printf(const char* fmt, ...) {
  if (!logFP) {
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  vfprintf(logFP, fmt, ap);
  fprintf(logFP, "\n");
  va_end(ap);
#if defined(XP_WIN) && defined(_DEBUG)
  // When the updater crashes on Windows the log file won't be flushed and this
  // can make it easier to debug what is going on.
  fflush(logFP);
#endif
}

void UpdateLog::WarnPrintf(const char* fmt, ...) {
  if (!logFP) {
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  fprintf(logFP, "*** Warning: ");
  vfprintf(logFP, fmt, ap);
  fprintf(logFP, "***\n");
  va_end(ap);
#if defined(XP_WIN) && defined(_DEBUG)
  // When the updater crashes on Windows the log file won't be flushed and this
  // can make it easier to debug what is going on.
  fflush(logFP);
#endif
}

#ifdef XP_WIN
// Deleter for use with unique_ptr
struct CoTaskMemFreeDeleter {
    void operator()(void* aPtr) { ::CoTaskMemFree(aPtr); }
};

/**
 * Determine if a path contains symlinks or junctions to disallowed locations
 *
 * @param fullPath  The full path to check.
 * @return true if the path contains invalid links or on errors,
 *         false if the check passes and the path can be used
 */
bool PathContainsInvalidLinks(wchar_t* const fullPath) {
  wchar_t pathCopy[MAXPATHLEN + 1] = L"";
  wcsncpy_s(pathCopy, MAXPATHLEN, fullPath, MAXPATHLEN - 1);
  wchar_t* remainingPath = nullptr;
  wchar_t* nextToken = wcstok_s(pathCopy, L"\\", &remainingPath);
  wchar_t* partialPath = nextToken;

  while (nextToken) {
    if ((GetFileAttributesW(partialPath) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      autoHandle h(CreateFileW(
          partialPath, 0,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (h.get() == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
          // The path can't be an invalid link if it doesn't exist.
          return false;
        } else {
          return true;
        }
      }

      std::unique_ptr<UINT8[]> byteBuffer =
          std::make_unique<UINT8[]>(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
      ZeroMemory(byteBuffer.get(), MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
      REPARSE_DATA_BUFFER* buffer = (REPARSE_DATA_BUFFER*)byteBuffer.get();
      DWORD bytes = 0;
      if (!DeviceIoControl(h.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer,
                           MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &bytes, nullptr)) {
        return true;
      }

      wchar_t* reparseTarget = nullptr;
      switch (buffer->ReparseTag) {
        case IO_REPARSE_TAG_MOUNT_POINT:
          reparseTarget =
              buffer->MountPointReparseBuffer.PathBuffer +
              (buffer->MountPointReparseBuffer.SubstituteNameOffset /
               sizeof(wchar_t));
          if (buffer->MountPointReparseBuffer.SubstituteNameLength <
              ARRAYSIZE(L"\\??\\")) {
            return false;
          }
          break;
        case IO_REPARSE_TAG_SYMLINK:
          reparseTarget =
              buffer->SymbolicLinkReparseBuffer.PathBuffer +
              (buffer->SymbolicLinkReparseBuffer.SubstituteNameOffset /
               sizeof(wchar_t));
          if (buffer->SymbolicLinkReparseBuffer.SubstituteNameLength <
              ARRAYSIZE(L"\\??\\")) {
            return false;
          }
          break;
        default:
          return true;
          break;
      }

      if (!reparseTarget) {
        return false;
      }
      if (wcsncmp(reparseTarget, L"\\??\\", ARRAYSIZE(L"\\??\\") - 1) != 0) {
        return true;
      }
    }

    nextToken = wcstok_s(nullptr, L"\\", &remainingPath);
    PathAppendW(partialPath, nextToken);
  }

  return false;
}

/**
 * Determine if a path is located within Program Files, either native or x86
 *
 * @param fullPath  The full path to check.
 * @return true if fullPath begins with either Program Files directory,
 *         false if it does not or if an error is encountered
 */
bool IsProgramFilesPath(TCHAR* fullPath) {
  LOG(("IsProgramFilesPath %ls", fullPath));
  // Make sure we don't try to compare against a short path.
  DWORD longInstallPathChars = GetLongPathNameW(fullPath, nullptr, 0);
  if (longInstallPathChars == 0) {
    LOG_WARN(("Failed because short path"));
    return false;
  }
  std::unique_ptr<wchar_t[]> longInstallPath =
      std::make_unique<wchar_t[]>(longInstallPathChars);
  if (!GetLongPathNameW(fullPath, longInstallPath.get(),
                        longInstallPathChars)) {
    LOG_WARN(("Failed to get long path name"));
    return false;
  }

  // First check for Program Files (x86).
  {
    PWSTR programFiles32PathRaw = nullptr;
    // FOLDERID_ProgramFilesX86 gets native Program Files directory on a 32-bit
    // OS or the (x86) directory on a 64-bit OS regardless of this binary's
    // bitness.
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr,
                                    &programFiles32PathRaw))) {
      // That call should never fail on any supported OS version.
      LOG_WARN(("Failed SHGetKnownFolderPath"));
      return false;
    }
    std::unique_ptr<wchar_t, CoTaskMemFreeDeleter>
        programFiles32Path(programFiles32PathRaw);
    // We need this path to have a trailing slash so our prefix test doesn't
    // match on a different folder which happens to have a name beginning with
    // the prefix we're looking for but then also more characters after that.
    size_t length = wcslen(programFiles32Path.get());
    if (length == 0) {
      LOG_WARN(("Failed length"));
      return false;
    }
    if (programFiles32Path.get()[length - 1] == L'\\') {
      if (_wcsnicmp(longInstallPath.get(), programFiles32Path.get(), length) ==
          0) {
        return true;
      }
    } else {
      // Allocate space for a copy of the string along with a terminator and one
      // extra character for the trailing backslash.
      length += 1;
      auto programFiles32PathWithSlash =
          std::make_unique<wchar_t[]>(length + 1);

      mywcsprintf(programFiles32PathWithSlash.get(), length + 1, L"%s\\",
                   programFiles32Path.get());

      if (_wcsnicmp(longInstallPath.get(), programFiles32PathWithSlash.get(),
                   length) == 0) {
        return true;
      }
    }
  }

  // If we didn't find (x86), check for the native Program Files.
  {
    // In case we're a 32-bit binary on 64-bit Windows, we now have a problem
    // getting the right "native" Program Files path, which is that there is no
    // FOLDERID_* value that returns that path. So we always read that one out
    // of its canonical registry location instead. If we're on a 32-bit OS, this
    // will be the same path that we just checked. First get the buffer size to
    // allocate for the path.
    DWORD length = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"Software\\Microsoft\\Windows\\CurrentVersion",
                     L"ProgramFilesDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, nullptr, &length) != ERROR_SUCCESS) {
      LOG_WARN(("Failed getting native Program Files length"));
      return false;
    }
    // RegGetValue returns the length including the terminator, but it's in
    // bytes, so convert that to characters.
    DWORD lengthChars = (length / sizeof(wchar_t));
    if (lengthChars <= 1) {
      LOG_WARN(("Failed length native"));
      return false;
    }
    auto programFilesNativePath =
        std::make_unique<wchar_t[]>(lengthChars);

    // Now actually read the value.
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion",
            L"ProgramFilesDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
            programFilesNativePath.get(), &length) != ERROR_SUCCESS) {
      LOG_WARN(("Failed getting native Program Files"));
      return false;
    }
    size_t nativePathStrLen =
        wcsnlen_s(programFilesNativePath.get(), lengthChars);
    if (nativePathStrLen == 0) {
      LOG_WARN(("Failed length native path"));
      return false;
    }

    // As before, append a backslash if there isn't one already.
    if (programFilesNativePath.get()[nativePathStrLen - 1] == L'\\') {
      if (_wcsnicmp(longInstallPath.get(), programFilesNativePath.get(),
                   nativePathStrLen) == 0) {
        return true;
      }
    } else {
      // Allocate space for a copy of the string along with a terminator and one
      // extra character for the trailing backslash.
      nativePathStrLen += 1;
      auto programFilesNativePathWithSlash =
          std::make_unique<wchar_t[]>(nativePathStrLen + 1);

      mywcsprintf(programFilesNativePathWithSlash.get(), nativePathStrLen + 1,
                   L"%s\\", programFilesNativePath.get());

      if (_wcsnicmp(longInstallPath.get(), programFilesNativePathWithSlash.get(),
                   nativePathStrLen) == 0) {
        return true;
      }
    }
  }

  return false;
}
#endif

/**
 * Performs checks of a full path for validity for this application.
 *
 * @param  origFullPath
 *         The full path to check.
 * @return true if the path is valid for this application and false otherwise.
 */
bool IsValidFullPath(TCHAR* origFullPath) {
  // Subtract 1 from MAXPATHLEN for null termination.
  if (wcslen(origFullPath) > MAXPATHLEN - 1) {
    // The path is longer than acceptable for this application.
    return false;
  }

#ifdef XP_WIN
  TCHAR testPath[MAXPATHLEN] = {L'\0'};
  // GetFullPathNameW will replace / with \ which PathCanonicalizeW requires.
  if (GetFullPathNameW(origFullPath, MAXPATHLEN, testPath, nullptr) == 0) {
    // Unable to get the full name for the path (e.g. invalid path).
    LOG_WARN(("Unable to get full path name"));
    return false;
  }

  TCHAR canonicalPath[MAXPATHLEN] = {L'\0'};
  if (!PathCanonicalizeW(canonicalPath, testPath)) {
    // Path could not be canonicalized (e.g. invalid path).
    LOG_WARN(("Unable to canonicalized path"));
    return false;
  }

  // Check if the path passed in resolves to a differerent path.
  if (_wcsicmp(origFullPath, canonicalPath) != 0) {
    // Case insensitive string comparison between the supplied path and the
    // canonical path are not equal. This will prevent directory traversal and
    // the use of / in paths since they are converted to \.
    LOG_WARN(("Original path doesn't match canonical path"));
    return false;
  }

  wcsncpy_s(testPath, MAXPATHLEN, origFullPath, MAXPATHLEN - 1);
  if (!PathStripToRootW(testPath)) {
    // It should always be possible to strip a valid path to its root.
    LOG_WARN(("Couldn't strip path to root"));
    return false;
  }

  if (origFullPath[0] == L'\\') {
    // Only allow UNC server share paths.
    if (!PathIsUNCServerShareW(testPath)) {
      LOG_WARN(("Path is not UNC server share"));
      return false;
    }
  }

  if (PathContainsInvalidLinks(canonicalPath)) {
    LOG_WARN(("Path contains invalid links"));
    return false;
  }
#else
  // Only allow full paths.
  if (origFullPath[0] != L('/')) {
    return false;
  }

  // The path must not traverse directories
  if (Lstrstr(origFullPath, L("/../")) != nullptr) {
    return false;
  }

  // The path shall not have a path traversal suffix
  const TCHAR invalidSuffix[] = L("/..");
  size_t pathLen = wcslen(origFullPath);
  size_t invalidSuffixLen = wcslen(invalidSuffix);
  if (invalidSuffixLen <= pathLen &&
      Lstrncmp(origFullPath + pathLen - invalidSuffixLen, invalidSuffix,
                  invalidSuffixLen) == 0) {
    return false;
  }
#endif
  return true;
}
