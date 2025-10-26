/*
@file
@mainpage

Map Caps Lock key to Escape key, or any key to any key.

The MIT License (MIT)
---------------------
Copyright (c) 2015-2021 Susam Pal

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/


#include <stdio.h>
#include <ctype.h>
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdarg.h>

/** Version of the program. */
#define VERSION "0.4.0-dev"

/** Author of the program. */
#define AUTHOR "Susam Pal"

/** Copyright notice. */
#define COPYRIGHT "Copyright (c) 2015-2021 " AUTHOR

/** URL to a copy of the license. */
#define LICENSE_URL "<https://susam.github.io/licenses/mit.html>"

/** URL to report issues to. */
#define SUPPORT_URL "<https://github.com/susam/uncap/issues>"

/** Maximum length of command line argument that is read. */
#define MAX_ARG_LEN 16

/** Maximum allowed length of an error message. */
#define MAX_ERR_LEN 256

/** Magic number to identify keyboard input injected by Uncap. */
#define UNCAP_INFO (WM_APP + 3195) /* 35963 */

#ifndef LLKHF_LOWER_IL_INJECTED
/** Workaround for missing definition in MinGW winuser.h. **/
#define LLKHF_LOWER_IL_INJECTED 0x00000002
#endif

/**
Check if two null-terminated byte strings are equal.

@param a Pointer to null-terminated byte string. (type: const char *)
@param b Pointer to null-terminated byte string. (type: const char *)

@return 1 if the two strings are equal; 0 otherwise.
*/
#define streq(a, b) (strcmp(a, b) == 0)


/**
Copy null-terminated byte string into a character array.

@param a Pointer to character array to copy the string to. (type: char *)
@param b Pointer to null-terminated byte string to copy. (type: const char *)
@param c Maximum number of characters to copy. (type: size_t)

@return a
*/
#define strcp(a, b, c) (a[0] = '\0', strncat(a, b, c - 1))


/**
Convert all characters of a string to lowercase.

@param s Pointer to null-terminated byte string.

@return s
*/
char *strlower(char *s)
{
    int i;
    for (i = 0; s[i] != '\0'; i++)
        s[i] = (char) tolower(s[i]);
    return s;
}


/**
Return name of the leaf directory or file in the specified path.

Both backslash and forward slash are treated as path separators. A
pointer to the beginning of the substring between the last slash
(exclusive) and the end of the string is returned as the basename.
Therefore if the specified path ends with a slash then an empty string
is returned.

@param path Path string.

@return Name of the leaf directory or file in the specified path.
*/
const char *basename(const char *path)
{
    const char *base;
    if ((base = strrchr(path, '\\')) != NULL)
        return base + 1;
    else if ((base = strrchr(path, '/')) != NULL)
        return base + 1;
    else
        return path;
}


/**
Values returned by a function to indicate success or failure.

These return codes may be returned by a function to indicate success or
failure of its operation as well as the next course of action.
*/
enum action {
    GOOD, /**< Successful operation; program should continue. */
    EXIT, /**< Successful operation; program should exit normally. */
    FAIL  /**< Failed operation; program should exit with error. */
};


/**
Global state of this program.
*/
struct state {
    char name[MAX_ARG_LEN];  /**< Program name. */
    WORD keymap[256];        /**< Key mappings. */
    HHOOK hook;              /**< Handle to keyboard hook procedure. */
    int console;             /**< Whether console mode is enabled. */
    int debug;               /**< Whether verbose mode is enabled. */
    FILE *file;              /**< File to write verbose logs to. */
    char error[MAX_ERR_LEN]; /**< Error message for failed operation. */

    /* New fields to support "Caps acting as Ctrl when used with other keys" */
    int capsDown;            /**< Caps is currently held down (pressed). */
    int capsUsedAsCtrl;      /**< Caps was used as Ctrl (other key pressed while caps held). */
    int capsCtrlInjected;    /**< We injected a Ctrl down; need to inject Ctrl up on release. */
    int capsAsCtrlEnabled;   /**< Whether the caps-as-ctrl behavior is enabled (via CLI). */
} my; /**< Global state of this program. */


/**
Output error message on the standard error stream.

@param format Format string for printf.
@param ...    Additional arguments.

@return EXIT_FAILURE; the caller of this function may return this code
        to indicate abnormal termination of the program.
*/
int error(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    fprintf(stderr, "%s: ", my.name);
    vfprintf(stderr, format, ap);
    va_end(ap);
    return EXIT_FAILURE;
}


#define LOG_FMT \n    "% -10s %3d %5lu %3lu " \n    "%3lu (%#04lx) %3lu (%#04lx) " \n    "[%s%s%s%s%s%s%s]\n"

/**
Log details of a key stroke to a specified file.
*/
#define logKeyTo(file) \n            fprintf(file, LOG_FMT, \n                    wParamStr, nCode, p->dwExtraInfo, p->flags, \n                    p->scanCode, p->scanCode, p->vkCode, p->vkCode, \n                    vkStr, upStr, extStr, altStr, lowStr, injStr, uncapStr)

/**
Log details of a key stroke.

@param nCode  Code used to determine how to process the message.
@param wParam Identifier of the keyboard message.
@param lParam Pointer to KBDLLHOOKSTRUCT structure.
*/
void logKey(int nCode, WPARAM wParam, LPARAM lParam)
{
    const KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *) lParam;

    char wParamStr[16];
    char vkStr[16];
    char extStr[16];
    char lowStr[16];
    char injStr[16];
    char altStr[16];
    char upStr[16];
    char uncapStr[16];

    /* Translate identifier of keyboard message to string notation. */
    switch (wParam) {
    case WM_KEYDOWN:
        strcpy(wParamStr, "KEYDOWN");
        break;

    case WM_KEYUP:
        strcpy(wParamStr, "KEYUP");
        break;

    case WM_SYSKEYDOWN:
        strcpy(wParamStr, "SYSKEYDOWN");
        break;

    case WM_SYSKEYUP:
        strcpy(wParamStr, "SYSKEYUP");
        break;

    default:
        strcpy(wParamStr, "UNKNOWN");
        break;
    }

    /* Translate virtual-key code to string notation. */
    if (p->vkCode == VK_RETURN)
        sprintf(vkStr, "RETURN");
    else if (p->vkCode == VK_CAPITAL)
        sprintf(vkStr, "CAPITAL");
    else if (p->vkCode == VK_ESCAPE)
        sprintf(vkStr, "ESCAPE");
    else if (p->vkCode == VK_LCONTROL)
        sprintf(vkStr, "LCONTROL");
    else if (p->vkCode == VK_RCONTROL)
        sprintf(vkStr, "RCONTROL");
    else if (p->vkCode == VK_LMENU)
        sprintf(vkStr, "LMENU");
    else if (p->vkCode == VK_RMENU)
        sprintf(vkStr, "RMENU");
    else if (p->vkCode == VK_LWIN)
        sprintf(vkStr, "LWIN");
    else if (p->vkCode == VK_RWIN)
        sprintf(vkStr, "RWIN");
    else if ((p->vkCode >= '0' && p->vkCode <= '9') ||
             (p->vkCode >= 'A' && p->vkCode <= 'Z'))
        sprintf(vkStr, "%c", p->vkCode);
    else if (p->vkCode >= VK_NUMPAD0 && p->vkCode <= VK_NUMPAD9)
        sprintf(vkStr, "NUMPAD%d", p->vkCode - VK_NUMPAD0);
    else if (p->vkCode >= VK_F1 && p->vkCode <= VK_F24)
        sprintf(vkStr, "F%d", p->vkCode - VK_F1 + 1);
    else
        sprintf(vkStr, "%#x", p->vkCode);

    /* Translate each flag to string notation. */
    strcpy(extStr, p->flags & LLKHF_EXTENDED ? " EXT" : "");
    strcpy(lowStr, p->flags & LLKHF_LOWER_IL_INJECTED ? " LOW": "");
    strcpy(injStr, p->flags & LLKHF_INJECTED ? " INJ" : "");
    strcpy(altStr, p->flags & LLKHF_ALTDOWN ? " ALT" : "");
    strcpy(upStr, p->flags & LLKHF_UP ? " UP" : " DN");
    strcpy(uncapStr, p->dwExtraInfo == UNCAP_INFO ? " UNCAP" : "");

    /* Log key to standard error stream if verbose mode is enabled. */
    if (my.debug)
        logKeyTo(stderr);

    /* Log key to user specified file if file logging is enabled. */
    if (my.file != NULL)
        logKeyTo(my.file);

    fflush(NULL);
}


/**
Map one key to another key.

@return 1 if the keyboard message is processed, i.e. a key is mapped to
        another key, otherwise call CallNextHookEx and return the value
        it returns.
*/
LRESULT CALLBACK keyboardHook(int nCode, WPARAM wParam, LPARAM lParam)
{
    KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *) lParam;
    WORD keyCode = (WORD) p->vkCode;
    WORD mapCode = my.keymap[keyCode];

    if (my.debug || my.file) {
        logKey(nCode, wParam, lParam);
    }

    /*
     New behavior (enabled only when capsAsCtrlEnabled is set):
     - If Caps Lock pressed: delay processing (swallow KEYDOWN).
     - If another key is pressed while capsDown: inject LCTRL down once and mark capsUsedAsCtrl.
     - On Caps release: if capsUsedAsCtrl then inject LCTRL up; else treat as tap -> emit ESC (down+up).
    */

    /* Handle Caps Lock special behavior (only for hardware events) if enabled. */
    if (my.capsAsCtrlEnabled && keyCode == VK_CAPITAL && p->dwExtraInfo != UNCAP_INFO && nCode >= 0) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (!my.capsDown) {
                /* Record that Caps is down and swallow the physical press for now. */
                my.capsDown = 1;
                my.capsUsedAsCtrl = 0;
                return 1; /* swallow */
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (my.capsUsedAsCtrl) {
                /* Caps was used as Ctrl: release injected Ctrl and swallow Caps. */
                if (my.capsCtrlInjected) {
                    INPUT inputs[1];
                    ZeroMemory(&inputs, sizeof inputs);
                    inputs[0].type = INPUT_KEYBOARD;
                    inputs[0].ki.wVk = VK_LCONTROL;
                    inputs[0].ki.dwFlags = KEYEVENTF_KEYUP;
                    inputs[0].ki.dwExtraInfo = UNCAP_INFO;
                    SendInput(1, inputs, sizeof *inputs);
                    my.capsCtrlInjected = 0;
                }
                my.capsDown = 0;
                my.capsUsedAsCtrl = 0;
                return 1; /* swallow */
            } else {
                /* Caps was tapped alone: emit ESC (keydown + keyup) and swallow Caps. */
                INPUT inputs[2];
                ZeroMemory(&inputs, sizeof inputs);

                /* ESC down */
                inputs[0].type = INPUT_KEYBOARD;
                inputs[0].ki.wVk = VK_ESCAPE;
                inputs[0].ki.dwFlags = 0;
                inputs[0].ki.dwExtraInfo = UNCAP_INFO;
                /* ESC up */
                inputs[1] = inputs[0];
                inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

                SendInput(2, inputs, sizeof *inputs);
                my.capsDown = 0;
                return 1; /* swallow */
            }
        }
    }

    /* If Caps-as-Ctrl is enabled and Caps is held and another keydown occurs, inject Ctrl down once. */
    if (my.capsAsCtrlEnabled && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
        my.capsDown && keyCode != VK_CAPITAL && p->dwExtraInfo != UNCAP_INFO &&
        nCode >= 0) {
        if (!my.capsUsedAsCtrl) {
            INPUT inputs[1];
            ZeroMemory(&inputs, sizeof inputs);
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wVk = VK_LCONTROL;
            inputs[0].ki.dwFlags = 0; /* key down */
            inputs[0].ki.dwExtraInfo = UNCAP_INFO;
            SendInput(1, inputs, sizeof *inputs);
            my.capsCtrlInjected = 1;
            my.capsUsedAsCtrl = 1;
        }
        /* Continue processing this other key normally (it will now behave as if Ctrl is held). */
    }

    /* Existing mapping behavior (unchanged). */
    if (mapCode == 0) {
        /* If key pressed is unmapped, disable the key press. */
        return 1;
    } else if (keyCode != mapCode && p->dwExtraInfo != UNCAP_INFO &&
               nCode >= 0) {
        /* If key is mapped, translate it to what it is mapped to. */
        INPUT inputs[1];
        ZeroMemory(&inputs, sizeof inputs);
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = mapCode;
        inputs[0].ki.dwFlags = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
                      ? KEYEVENTF_KEYUP : 0;
        inputs[0].ki.dwExtraInfo = UNCAP_INFO;

        SendInput(1, inputs, sizeof *inputs);
        return 1;
    }

    return CallNextHookEx(my.hook, nCode, wParam, lParam);
}


/**
Kill other running instances of this program.

@return Next action to take based on whether this function could
        terminate other running instances of this program or not. FAIL
        is returned if this function failed to terminate at least one
        other running instance of this program. EXIT is returned if this
        function successfully terminated all other running instances of
        this program or if there were no other running instances of this
        program.
*/
enum action kill(void)
{
    char myExeFile[MAX_ARG_LEN + 4];
    char *dot;
    PROCESSENTRY32 entry;
    HANDLE snapshotHandle;
    int failure = 0;

    /* Take a snapshot of all processes running on the system. */
    snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshotHandle == NULL) {
        sprintf(my.error, "Cannot take snapshot of processes; error: %lu.", GetLastError());
        return FAIL;
    }

    /* Begin iterating through each process in the snapshot. */
    entry.dwSize = sizeof entry;
    if (!Process32First(snapshotHandle, &entry)) {
        sprintf(my.error, "Cannot retrieve process from snapshot; error: %lu.", GetLastError());
        CloseHandle(snapshotHandle);
        return FAIL;
    }

    /* Name of the process to be killed. */
    strcpy(myExeFile, my.name);
    strlower(myExeFile);
    dot = strrchr(myExeFile, '.');
    if (dot == NULL || !streq(dot, ".exe"))
        strcat(myExeFile, ".exe");

    /* Iterate through each process in the snapshot, find the process to
       be killed and kill it. */
    do {
        HANDLE processHandle;

        /* Ignore current process. */
        if (entry.th32ProcessID == GetCurrentProcessId())
            continue;

        /* Ignore other programs. */
        if (!streq(strlower(entry.szExeFile), myExeFile))
            continue;

        /* Open another instance of this program. */
        processHandle = OpenProcess(PROCESS_ALL_ACCESS, 0,
                                    entry.th32ProcessID);
        if (processHandle == NULL) {
            error("Cannot open process \"%s\" (PID %lu); error %lu.",
                  entry.szExeFile, entry.th32ProcessID, GetLastError());
            failure = 1;
            continue;
        }

        /* Terminate another instance of this program. */
        if (TerminateProcess(processHandle, 0)) {
            printf("Terminated %s (PID %lu).\n",
                   entry.szExeFile, entry.th32ProcessID);
        } else {
            error("Cannot terminate %s (PID %lu); error %lu.\n",\n                  entry.szExeFile, entry.th32ProcessID, GetLastError());
            failure = 1;
        }
        CloseHandle(processHandle);

    } while (Process32Next(snapshotHandle, &entry));

    CloseHandle(snapshotHandle);

    if (failure) {
        sprintf(my.error, "Failed to terminate all running instances of %s.", myExeFile);
        return FAIL;
    }

    return EXIT;
}

/**
Show usage and help details of this program.
*/
void showHelp(void)
{
    const char *usage =
"Usage: %s [-k] [-c] [-d] [-f FILE] [-h] [-v] [[MAP_KEY:TO_KEY]...]\n\n";

    const char *summary =
"Map Caps Lock key to Escape key, or any key to any key.\n\n";

    const char *description1 =
"Caps Lock key is mapped to Escape key by default. This may be\n"
"overridden by specifying a new mapping for Caps Lock key. Any key\n"
"may be mapped to any key with one or more MAP_KEY:TO_KEY arguments.\n"
"Each argument is a colon separated pair of virtual-key codes from\n"
"<https://msdn.microsoft.com/library/windows/desktop/dd375731.aspx>.\n\n";

    const char *description2 =
"If MAP_KEY equals TO_KEY, then no mapping occurs for it. If TO_KEY\n"
"equals 0, then the key mapped to 0 is disabled.\n\n";

    const char *details =
"Options:\n"
"  -k, --kill       Kill other instances of uncap.\n"
"  -c, --console    Run silently in console.\n"
"  -d, --debug      Run verbosely in console.\n"
"  -f, --file FILE  Write verbose logs to file.\n"
"  -h, --help       Show this help and exit.\n"
"  -v, --version    Show version and exit.\n"
"  --caps-as-ctrl   Enable Caps-as-Ctrl behavior (hold Caps + other key -> Ctrl).\n"
"  --no-caps-as-ctrl Disable Caps-as-Ctrl behavior (default).\n\n"
"Arguments:\n"
"  MAP_KEY          Virtual-key code of key to map.\n"
"  TO_KEY           Virtual-key code of key to map to.\n\n"
"Report bugs to " SUPPORT_URL ".\n";

    printf(usage, my.name);
    printf(summary);
    printf(description1);
    printf(description2);
    printf(details);
}

/** end of file */