/* Helpers shared by the window and its dialogs: see winutil.h */
#include "gui/winutil.h"
#include "core/strbuf.h"
#include <stdlib.h>
#include <string.h>

/* direct conversions into a short buffer fail and would leave the text
 * empty: convert into a buffer of the right size and truncate */
void to_w(const char *s, wchar_t *out, int n)
{
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *tmp;
    out[0] = 0;
    if (need <= 0) return;
    if (need <= n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n); return; }
    if (!(tmp = (wchar_t *)malloc(need * sizeof(wchar_t)))) return;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, tmp, need)) {
        int k = n - 1;
        if (k > 0 && tmp[k - 1] >= 0xD800 && tmp[k - 1] <= 0xDBFF) k--;   /* keep surrogate pairs whole */
        memcpy(out, tmp, k * sizeof(wchar_t));
        out[k] = 0;
    }
    free(tmp);
}

void to_u8(const wchar_t *s, char *out, int n)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *tmp;
    out[0] = 0;
    if (need <= 0) return;
    if (!(tmp = (char *)malloc(need))) return;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, tmp, need, NULL, NULL))
        str_copy(out, (size_t)n, tmp);      /* cuts on a UTF-8 boundary */
    free(tmp);
}

int run_capture(wchar_t *cmd, char *out, int n, DWORD *code)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    HANDLE rd, wr, nul;
    DWORD got; int len = 0, ok;
    out[0] = 0;
    if (!CreatePipe(&rd, &wr, &sa, 65536)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    /* how CreateProcess finds "samdisk": exe folder, current, system, PATH */
    ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return 0; }
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    while (len < n - 1 && ReadFile(rd, out + len, n - 1 - len, &got, NULL) && got) len += (int)got;
    out[len] = 0;
    if (code && !GetExitCodeProcess(pi.hProcess, code)) *code = 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
    return 1;
}

int parse_dic_version(const char *s, char *ver, int n)
{
    const char *p = strstr(s, "AppVersion"), *e, *w;
    int k = 0;
    if (!p || !(p = strchr(p, '\n'))) return 0;
    p++;
    for (e = p; *e && *e != '\r' && *e != '\n'; e++) ;
    for (w = e; w > p && w[-1] != ' ' && w[-1] != '\t' && w[-1] != ','; w--) ;
    while (w < e && k < n - 1) ver[k++] = *w++;
    ver[k] = 0;
    return k > 0;
}

int exit_missing_dll(DWORD code)
{
    return code == 0xC0000135 || code == 0xC0000139 || code == 0xC0000142;
}

void exe_path(const wchar_t *rel, wchar_t *out, int n)
{
    wchar_t *p;
    out[0] = 0;
    if (!GetModuleFileNameW(NULL, out, n) || !(p = wcsrchr(out, L'\\'))) { out[0] = 0; return; }
    if ((int)((p + 1 - out) + wcslen(rel)) >= n) { out[0] = 0; return; }
    wcscpy(p + 1, rel);
}

int file_exists(const wchar_t *path)
{
    DWORD a = path[0] ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
