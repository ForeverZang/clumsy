// clumsy needs to be run as admin to work properly.
// here's routines for checking admin state and self elevate
// from the example CppSelfElevate
// http://code.msdn.microsoft.com/windowsdesktop/CppUACSelfElevation-981c0160
#include <Windows.h>
#include <VersionHelpers.h>
#include "common.h"
#include "i18n.h"

// 
//   FUNCTION: IsRunAsAdmin()
//
//   PURPOSE: The function checks whether the current process is run as 
//   administrator. In other words, it dictates whether the primary access 
//   token of the process belongs to user account that is a member of the 
//   local Administrators group and it is elevated.
//
//   RETURN VALUE: Returns TRUE if the primary access token of the process 
//   belongs to user account that is a member of the local Administrators 
//   group and it is elevated. Returns FALSE if the token does not.
//
//   EXCEPTION: If this function fails, it throws a C++ DWORD exception which 
//   contains the Win32 error code of the failure.
//   * changed to not throw and return false *
//
//   EXAMPLE CALL:
//     try 
//     {
//         if (IsRunAsAdmin())
//             wprintf (L"Process is run as administrator\n");
//         else
//             wprintf (L"Process is not run as administrator\n");
//     }
//     catch (DWORD dwError)
//     {
//         wprintf(L"IsRunAsAdmin failed w/err %lu\n", dwError);
//     }
//
BOOL IsRunAsAdmin()
{
    BOOL fIsRunAsAdmin = FALSE;
    DWORD dwError = ERROR_SUCCESS;
    PSID pAdministratorsGroup = NULL;

    // Allocate and initialize a SID of the administrators group.
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(
        &NtAuthority, 
        2, 
        SECURITY_BUILTIN_DOMAIN_RID, 
        DOMAIN_ALIAS_RID_ADMINS, 
        0, 0, 0, 0, 0, 0, 
        &pAdministratorsGroup))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

    // Determine whether the SID of administrators group is enabled in 
    // the primary access token of the process.
    if (!CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

Cleanup:
    // Centralized cleanup for all allocated resources.
    if (pAdministratorsGroup)
    {
        FreeSid(pAdministratorsGroup);
        pAdministratorsGroup = NULL;
    }

    // Throw the error if something failed in the function.
    if (ERROR_SUCCESS != dwError)
    {
        return FALSE;
    }

    return fIsRunAsAdmin;
}

// pasta from:
// http://stackoverflow.com/questions/8046097/how-to-check-if-a-process-has-the-admin-rights
BOOL IsElevated( ) {
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if( OpenProcessToken( GetCurrentProcess( ),TOKEN_QUERY,&hToken ) ) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof( TOKEN_ELEVATION );
        if( GetTokenInformation( hToken, TokenElevation, &Elevation, sizeof( Elevation ), &cbSize ) ) {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if( hToken ) {
        CloseHandle( hToken );
    }
    return fRet;
}

// Show a localized message box. UI strings are UTF-8 (i18n); MessageBoxA
// would render them as ANSI mojibake, so convert and go through MessageBoxW.
static void elevateMessageBox(HWND hWnd, const char *textKey) {
    wchar_t text[512];
    wchar_t title[128];
    MultiByteToWideChar(CP_UTF8, 0, tr(textKey), -1, text, ARRAYSIZE(text));
    MultiByteToWideChar(CP_UTF8, 0, tr("app.title"), -1, title,
                        ARRAYSIZE(title));
    MessageBoxW(hWnd, text, title, MB_OK | MB_ICONWARNING);
}

// try elevate and error out when can't happen
// is silent then no message boxes are shown
// return whether to close the program
BOOL tryElevate(HWND hWnd, BOOL silent) {
    // Check the current process's "run as administrator" status.
    BOOL fIsRunAsAdmin;
    if (!IsWindowsVistaOrGreater()) {
        if (!silent) elevateMessageBox(hWnd, "error.elevateUnsupported");
        return TRUE;
    }

    fIsRunAsAdmin = IsRunAsAdmin();
    if (fIsRunAsAdmin) {
        return FALSE;
    }

    // when not silent then trying to reinvoke to elevate
    if (!silent) {
        wchar_t szPath[MAX_PATH];
        if (GetModuleFileNameW(NULL, szPath, ARRAYSIZE(szPath)))
        {
            // Launch itself as administrator.
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpVerb = L"runas";
            sei.lpFile = szPath;
            sei.hwnd = hWnd;
            sei.nShow = SW_NORMAL;

            // here it is restartintg the program using run as
            LOG("Try elevating by runas");
            if (!ShellExecuteExW(&sei))
            {
                DWORD dwError = GetLastError();
                if (dwError == ERROR_CANCELLED)
                {
                    // The user refused the elevation.
                    // alert and exit
                    elevateMessageBox(hWnd, "error.elevateRefused");
                }
            }
            // runas executed.
        } else {
            elevateMessageBox(hWnd, "error.elevateBadPath");
        }
    }

    // exit when not run as admin
    return TRUE;
}