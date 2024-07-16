/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <memory>

BOOL ExecuteServiceCommand(int argc, LPWSTR* argv);

const wchar_t kCommandLineDelimiter[] = L" \t";

inline size_t ArgStrLen(const wchar_t* s) {
    size_t backslashes = 0;
    size_t i = wcslen(s);
    bool hasDoubleQuote = wcschr(s, L'"') != nullptr;
    // Only add doublequotes if the string contains a space or a tab
    bool addDoubleQuotes = wcspbrk(s, kCommandLineDelimiter) != nullptr;

    if (addDoubleQuotes) {
        i += 2;  // initial and final doublequote
    }

    if (hasDoubleQuote) {
        while (*s) {
            if (*s == '\\') {
                ++backslashes;
            }
            else {
                if (*s == '"') {
                    // Escape the doublequote and all backslashes preceding the
                    // doublequote
                    i += backslashes + 1;
                }

                backslashes = 0;
            }

            ++s;
        }
    }

    return i;
}

/**
 * Copy string "s" to string "d", quoting the argument as appropriate and
 * escaping doublequotes along with any backslashes that immediately precede
 * doublequotes.
 * The CRT parses this to retrieve the original argc/argv that we meant,
 * see STDARGV.C in the MSVC CRT sources.
 *
 * @return the end of the string
 */
inline wchar_t* ArgToString(wchar_t* d, const wchar_t* s) {
    size_t backslashes = 0;
    bool hasDoubleQuote = wcschr(s, L'"') != nullptr;
    // Only add doublequotes if the string contains a space or a tab
    bool addDoubleQuotes = wcspbrk(s, kCommandLineDelimiter) != nullptr;

    if (addDoubleQuotes) {
        *d = '"';  // initial doublequote
        ++d;
    }

    if (hasDoubleQuote) {
        size_t i;
        while (*s) {
            if (*s == '\\') {
                ++backslashes;
            }
            else {
                if (*s == '"') {
                    // Escape the doublequote and all backslashes preceding the
                    // doublequote
                    for (i = 0; i <= backslashes; ++i) {
                        *d = '\\';
                        ++d;
                    }
                }

                backslashes = 0;
            }

            *d = *s;
            ++d;
            ++s;
        }
    }
    else {
        size_t len = wcslen(s);
        wcscpy_s(d, len + 1, s); // + 1 to account for null terminator
        d += len;
    }

    if (addDoubleQuotes) {
        *d = '"';  // final doublequote
        ++d;
    }

    return d;
}

/**
 * Creates a command line from a list of arguments.
 *
 * @param argc Number of elements in |argv|
 * @param argv Array of arguments
 * @param aArgcExtra Number of elements in |aArgvExtra|
 * @param aArgvExtra Optional array of arguments to be appended to the resulting
 *                   command line after those provided by |argv|.
 */
inline std::unique_ptr<wchar_t[]> MakeCommandLine(
    int argc, const wchar_t* const* argv, int aArgcExtra = 0,
    const wchar_t* const* aArgvExtra = nullptr) {
    size_t i;
    size_t len = 0;

    // The + 1 for each argument reserves space for either a ' ' or the null
    // terminator, depending on the position of the argument.
    for (i = 0; i < argc; ++i) {
        len += ArgStrLen(argv[i]) + 1;
    }

    for (i = 0; i < aArgcExtra; ++i) {
        len += ArgStrLen(aArgvExtra[i]) + 1;
    }

    // Protect against callers that pass 0 arguments
    if (len == 0) {
        len = 1;
    }

    auto s = std::make_unique<wchar_t[]>(len);

    int totalArgc = argc + aArgcExtra;

    wchar_t* c = s.get();
    for (i = 0; i < argc; ++i) {
        c = ArgToString(c, argv[i]);
        if (i + 1 != totalArgc) {
            *c = ' ';
            ++c;
        }
    }

    for (i = 0; i < aArgcExtra; ++i) {
        c = ArgToString(c, aArgvExtra[i]);
        if (i + 1 != aArgcExtra) {
            *c = ' ';
            ++c;
        }
    }

    *c = '\0';

    return s;
}
