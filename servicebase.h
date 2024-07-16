/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <windows.h>
#include "updatecommon.h"

BOOL VerifySameFiles(LPCWSTR file1Path, LPCWSTR file2Path, BOOL& sameContent);

// 32KiB for comparing files at a time seems reasonable.
// The bigger the better for speed, but this will be used
// on the stack so I don't want it to be too big.
#define COMPARE_BLOCKSIZE 32768

// The following string resource value is used to uniquely identify the signed
// Aveo Systems application as an installer.  Before the update service will
// execute the installer it must have this installer identity string in its string
// table.  No other signed Aveo Systems product will have this string table value.
#define UPDATER_IDENTITY_STRING \
  "aveo-installer-c206aa25-b890-4b6a-85c9-a915a6e1a561"
#define IDS_UPDATER_IDENTITY 2836
