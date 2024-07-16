/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef UPDATECOMMON_H
#define UPDATECOMMON_H

#include <stdio.h>
#include <memory>
#include <windows.h>

#ifndef MAXPATHLEN
#  define MAXPATHLEN MAX_PATH
#endif

static inline int mywcsprintf(WCHAR* dest, size_t count, const WCHAR* fmt,
    ...) {
    size_t _count = count - 1;
    va_list varargs;
    va_start(varargs, fmt);
    int result = _vsnwprintf_s(dest, count, count - 1, fmt, varargs);
    va_end(varargs);
    dest[_count] = L'\0';
    return result;
}

struct HandleDeleter {
    typedef HANDLE pointer;
    void operator()(HANDLE handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
typedef std::unique_ptr<SC_HANDLE, HandleDeleter> autoHandle;

struct HandleModuleDeleter {
    typedef HMODULE pointer;
    void operator()(HMODULE handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            FreeModule(handle);
        }
    }
};
typedef std::unique_ptr<HMODULE, HandleModuleDeleter> autoModuleHandle;

class UpdateLog {
 public:
  static UpdateLog& GetPrimaryLog() {
    static UpdateLog primaryLog;
    return primaryLog;
  }

  void Init(TCHAR* logFilePath);
  void Finish();
  void Flush();
  void Printf(const char* fmt, ...);
  void WarnPrintf(const char* fmt, ...);

  ~UpdateLog() { Finish(); }

 protected:
  UpdateLog();
  FILE* logFP;
  TCHAR mDstFilePath[MAXPATHLEN];
};

bool IsValidFullPath(TCHAR* fullPath);
bool IsProgramFilesPath(TCHAR* fullPath);

#define LOG_WARN(args) UpdateLog::GetPrimaryLog().WarnPrintf args
#define LOG(args) UpdateLog::GetPrimaryLog().Printf args
#define LogInit(FILEPATH_) UpdateLog::GetPrimaryLog().Init(FILEPATH_)
#define LogFinish() UpdateLog::GetPrimaryLog().Finish()
#define LogFlush() UpdateLog::GetPrimaryLog().Flush()
#endif
