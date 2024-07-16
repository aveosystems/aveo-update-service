; This Source Code Form is subject to the terms of the Mozilla Public
; License, v. 2.0. If a copy of the MPL was not distributed with this
; file, You can obtain one at http://mozilla.org/MPL/2.0/. */

; Modified from: https://hg.mozilla.org/mozilla-central/file/tip/browser/installer/windows/nsis/

; Windows installer for Aveo Systems Update Service

RequestExecutionLevel admin
CRCCheck on
Unicode true
ManifestSupportedOS all
ManifestDPIAware true

; Variables
Var TempUpdateServiceName
Var InstallVCRedist

; Other included files may depend upon these includes!
; The following includes are provided by NSIS.
!include FileFunc.nsh
!include LogicLib.nsh
!include MUI2.nsh
!include x64.nsh
!include WinMessages.nsh
!include WinVer.nsh
!include WordFunc.nsh

; shared functions with Mira Connect setup
!include common.nsh

; additional utils
!include strip.nsh

!insertmacro GetOptions
!insertmacro GetParameters
!insertmacro GetSize

!define AddUpdateServiceCertKeys "!insertmacro AddUpdateServiceCertKeys"

; The test machines use this fallback key to run tests.
; And anyone that wants to run tests themselves should already have 
; this installed.
!define FALLBACK_KEY \
  "SOFTWARE\Aveo Systems\Update Service\7bbba04f0511408d8aab391029b40221"

!ifndef VERSION
  !define VERSION 1.0.0.0
!endif
!define COMPANY_NAME "Aveo Systems"
!define APP_NAME "Update Service"
; serviceinstall.cpp also uses this key, in case the path is changed, update
; there too.
!define INSTALL_DIR_REG_KEY "Software\${COMPANY_NAME}\${APP_NAME}"
!define UNINSTALL_REG_KEY \
 "Software\Microsoft\Windows\CurrentVersion\Uninstall\${COMPANY_NAME} ${APP_NAME}"
!define INSTALLER_PATH_OUT "dist"
!define INSTALLER_FILE_NAME "updateservice-installer.exe"
!define UNINSTALLER_FILE_NAME "uninstall.exe"
!define CERTIFICATE_NAME "Aveo Systems, Inc."
!define CERTIFICATE_ISSUER "Sectigo Public Code Signing CA R36"
!define VC_REDIST_FILE_NAME "VC_redist.x64.exe"
!define VC_VERSION_REG_KEY "Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
!define VC_VERSION_REDIST "14.32.31326.00"

VIAddVersionKey /LANG=0 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=0 "CompanyName" "${COMPANY_NAME}"
VIAddVersionKey /LANG=0 "LegalTrademarks" "${COMPANY_NAME}® is a registered trademark"
VIAddVersionKey /LANG=0 "LegalCopyright" "©${COMPANY_NAME}, Inc. All rights reserved."
VIAddVersionKey /LANG=0 "FileDescription" "${APP_NAME} ${APP_NAME} Installer"
VIAddVersionKey /LANG=0 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "OriginalFilename" "${INSTALLER_FILE_NAME}"
VIProductVersion ${VERSION}
VIFileVersion ${VERSION}

;Add installer ID that will be verified by the update service
PEAddResource "installer-id.txt" "#2836" "#2836"

Name "${COMPANY_NAME} ${APP_NAME}"
OutFile "${INSTALLER_PATH_OUT}\${INSTALLER_FILE_NAME}"

SetOverwrite on

InstallDir "$PROGRAMFILES64\${COMPANY_NAME}\${APP_NAME}"
InstallDirRegKey HKLM "${INSTALL_DIR_REG_KEY}" ""

ShowUnInstDetails nevershow

;Interface Settings
!define MUI_ABORTWARNING

; Uninstaller Pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

;--------------------------------
;Languages
 
!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Code Signing
; These directives are used to code sign the installer and uninstaller.
; The signing script expects that the certificate password is stored in
; the system environment variable SIGNING_CERTIFICATE_PASSWORD. The '= 0'
; is used to ensure that the directives do not run in parallel, since some
; signing utilities do not support concurrent execution.
!finalize './scripts/sign "%1" "Aveo Systems Update Service Setup"' = 0
!uninstfinalize './scripts/sign "%1" "Aveo Systems Update Service Uninstall"' = 0

Function .onInit
  ; Remove the current exe directory from the search order.
  ; This only effects LoadLibrary calls and not implicitly loaded DLLs.
  System::Call 'kernel32::SetDllDirectoryW(w "")'

  ${Unless} ${AtLeastWin7}
    Abort
  ${EndUnless}
FunctionEnd

Function un.onInit
  ; Remove the current exe directory from the search order.
  ; This only effects LoadLibrary calls and not implicitly loaded DLLs.
  System::Call 'kernel32::SetDllDirectoryW(w "")'
FunctionEnd

Section "UpdateService"
  AllowSkipFiles off

  CreateDirectory "$INSTDIR"
  SetOutPath "$INSTDIR"

  ; always use the 64-bit registry.
  ${If} ${RunningX64}
  ${OrIf} ${IsNativeARM64}
    SetRegView 64
  ${EndIf}

  ; Install the VC++ Runtime Redistributable if needed
  ReadRegStr $0 HKLM "${VC_VERSION_REG_KEY}" "Version"
  ${If} $0 == ""
    StrCpy $InstallVCRedist "1"
  ${Else}
    ${CharStrip} 'v' $0 $1 ; remove a leading 'v' from version in registry
    ${VersionCompare} $1 ${VC_VERSION_REDIST} $2
    ${If} $2 == "2" ; if redist version is newer, install it
      StrCpy $InstallVCRedist "1"
    ${Else}
      DetailPrint "VC++ Runtime $1 already installed"
    ${EndIf}
  ${EndIf}
  ${If} $InstallVCRedist == "1"
    File "redist\${VC_REDIST_FILE_NAME}"
    DetailPrint "Installing VC++ Runtime ${VC_VERSION_REDIST}..."
    ExecWait '"$INSTDIR\${VC_REDIST_FILE_NAME}" /install /quiet /norestart'
    Delete "$INSTDIR\${VC_REDIST_FILE_NAME}"
  ${EndIf}

  ; If the service already exists, then it will be stopped when upgrading it
  ; via the updateservice_tmp.exe command executed below.
  ; The updateservice_tmp.exe command will rename the file to
  ; updateservice.exe if updateservice_tmp.exe is newer.
  ; If the service does not exist yet, we install it and drop the file on
  ; disk as updateservice.exe directly.
  StrCpy $TempUpdateServiceName "updateservice.exe"
  IfFileExists "$INSTDIR\updateservice.exe" 0 skipAlreadyExists
    StrCpy $TempUpdateServiceName "updateservice_tmp.exe"
  skipAlreadyExists:

  ; We always write out a copy and then decide whether to install it or 
  ; not by calling its 'install' cmdline which works by version comparison.
  File "/oname=$TempUpdateServiceName" "AveoUpdateService\x64\Release\updateservice.exe"

  ; Install the application update service.
  ; If a service already exists, the command line parameter will stop the
  ; service and only install itself if it is newer than the already installed
  ; service.  If successful it will remove the old updateservice.exe
  ; and replace it with updateservice_tmp.exe.
  ExecWait '"$INSTDIR\$TempUpdateServiceName" install'

  ; Write license.txt
  File "license.txt"

  WriteUninstaller "$INSTDIR\${UNINSTALLER_FILE_NAME}"

  ;Store installation folder
  WriteRegStr HKLM "${INSTALL_DIR_REG_KEY}" "" $INSTDIR

  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "DisplayName" "${COMPANY_NAME} ${APP_NAME}"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "UninstallString" \
                   '"$INSTDIR\${UNINSTALLER_FILE_NAME}"'
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "QuietUninstallString" \
                   '"$INSTDIR\${UNINSTALLER_FILE_NAME}" /S'
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "DisplayIcon" \
                   "$INSTDIR\${UNINSTALLER_FILE_NAME},0"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "Publisher" "${COMPANY_NAME}"
  WriteRegDWORD HKLM "${UNINSTALL_REG_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_REG_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $R2 $R3 $R4
  WriteRegDWORD HKLM "${UNINSTALL_REG_KEY}" "EstimatedSize" $R2

  ${If} ${RunningX64}
  ${OrIf} ${IsNativeARM64}
    SetRegView lastused
  ${EndIf}

  ; Write update service certificates to registry
  ${AddUpdateServiceCertKeys}

SectionEnd

; By renaming before deleting we improve things slightly in case
; there is a file in use error. In this case a new install can happen.
Function un.RenameDelete
  Pop $9
  ; If the .aveo-delete file already exists previously, delete it
  ; If it doesn't exist, the call is ignored.
  ; We don't need to pass /REBOOTOK here since it was already marked that way
  ; if it exists.
  Delete "$9.aveo-delete"
  Rename "$9" "$9.aveo-delete"
  ${If} ${Errors}
    Delete /REBOOTOK "$9"
  ${Else} 
    Delete /REBOOTOK "$9.aveo-delete"
  ${EndIf}
  ClearErrors
FunctionEnd

Section "Uninstall"
  ; Delete the service so that no updates will be attempted
  ExecWait '"$INSTDIR\updateservice.exe" uninstall'

  Push "$INSTDIR\updateservice.exe"
  Call un.RenameDelete
  Push "$INSTDIR\updateservice_tmp.exe"
  Call un.RenameDelete
  Push "$INSTDIR\updateservice.old"
  Call un.RenameDelete
  Push "$INSTDIR\Uninstall.exe"
  Call un.RenameDelete
  Push "$INSTDIR\license.txt"
  Call un.RenameDelete
  Push "$INSTDIR\updates\updater.exe"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-1.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-2.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-3.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-4.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-5.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-6.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-7.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-8.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-9.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-10.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-install.log"
  Call un.RenameDelete
  Push "$INSTDIR\logs\updateservice-uninstall.log"
  Call un.RenameDelete
  RMDir /REBOOTOK "$INSTDIR\logs"
  RMDir /REBOOTOK "$INSTDIR\update"
  RMDir /REBOOTOK "$INSTDIR"
  ; If the default install location was used, clean up the company name directory
  RMDir "$PROGRAMFILES64\${COMPANY_NAME}" ; only succeeds if directory empty

  ${If} ${RunningX64}
  ${OrIf} ${IsNativeARM64}
    SetRegView 64
  ${EndIf}
  DeleteRegValue HKLM "${INSTALL_DIR_REG_KEY}" "Installed"
  DeleteRegKey HKLM "${INSTALL_DIR_REG_KEY}"
  DeleteRegKey /ifempty HKLM "SOFTWARE\${COMPANY_NAME}"
  DeleteRegKey HKLM "${UNINSTALL_REG_KEY}"
  DeleteRegKey HKLM "${FALLBACK_KEY}"
  ${If} ${RunningX64}
  ${OrIf} ${IsNativeARM64}
    SetRegView lastused
  ${EndIf}
SectionEnd