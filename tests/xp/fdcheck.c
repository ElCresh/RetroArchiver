/* fdcheck - probe for an automatic mode: asks fdrawcmd.sys every few
 * seconds whether a floppy is in the drive (IOCTL_FD_CHECK_DISK) and prints
 * the answer with the time it took, so that on the real PC we can hear
 * what each question costs (motor, head) and see when an inserted or
 * removed disk is noticed.
 *
 *   fdcheck [SECONDS] [DRIVE] [COUNT]   default every 2 seconds, drive 0 (A:);
 *                                       COUNT questions then exit (0 = until Q)
 *
 * Q or Esc quits. RetroArchiver and SAMdisk must not be using the drive.
 * Codes from fdrawcmd.h (https://github.com/simonowen/fdrawcmd); the
 * driver answers ERROR_NO_MEDIA_IN_DRIVE when no disk is present.
 * User-facing text is Italian, like the rest of the program. */
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <winioctl.h>

#define FD_CTL_CODE(i, m) CTL_CODE(FILE_DEVICE_UNKNOWN, i, m, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_FDRAWCMD_GET_VERSION FD_CTL_CODE(0x888, METHOD_BUFFERED)
#define IOCTL_FD_CHECK_DISK        FD_CTL_CODE(0x917, METHOD_BUFFERED)   /* driver 1.0.1.10 and later */

static void stamp(char *out, size_t n)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snprintf(out, n, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    out[n - 1] = 0;
}

/* 1 present, 0 absent, -1 other error (in *err) */
static int check(HANDLE h, DWORD *ms, DWORD *err)
{
    DWORD got, t0 = GetTickCount();
    BOOL ok = DeviceIoControl(h, IOCTL_FD_CHECK_DISK, NULL, 0, NULL, 0, &got, NULL);
    *err = ok ? 0 : GetLastError();
    *ms = GetTickCount() - t0;
    if (ok) return 1;
    return *err == ERROR_NO_MEDIA_IN_DRIVE ? 0 : -1;
}

static int quit_pressed(void)
{
    int c;
    if (!_kbhit()) return 0;
    c = _getch();
    return c == 'q' || c == 'Q' || c == 27;
}

int main(int argc, char **argv)
{
    char path[32], when[16];
    int secs = argc > 1 ? atoi(argv[1]) : 2, drive = argc > 2 ? atoi(argv[2]) : 0;
    int count = argc > 3 ? atoi(argv[3]) : 0;
    int state, last = -2, n = 0, k;
    DWORD ver = 0, got, ms, err;
    HANDLE h;

    if (secs < 1) secs = 1;
    _snprintf(path, sizeof(path), "\\\\.\\fdraw%d", drive);
    path[sizeof(path) - 1] = 0;
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("Non riesco ad aprire %s (errore %lu): fdrawcmd.sys e' installato e attivo?\n"
               "Chiudi RetroArchiver e SAMdisk, poi riprova.\n", path, GetLastError());
        return 2;
    }
    if (DeviceIoControl(h, IOCTL_FDRAWCMD_GET_VERSION, NULL, 0, &ver, sizeof(ver), &got, NULL))
        printf("fdrawcmd.sys %lu.%lu.%lu.%lu\n", (ver >> 24) & 0xff, (ver >> 16) & 0xff,
               (ver >> 8) & 0xff, ver & 0xff);
    printf("Chiedo al lettore %c: ogni %d secondi se c'e' un dischetto. Q o Esc per uscire.\n"
           "Prova: lascialo vuoto un po', inserisci un dischetto, toglilo. Ascolta il lettore.\n\n",
           'A' + drive, secs);

    for (;;) {
        state = check(h, &ms, &err);
        stamp(when, sizeof(when));
        n++;
        if (state == 1)      printf("%s  #%-4d presente   %5lu ms", when, n, ms);
        else if (state == 0) printf("%s  #%-4d assente    %5lu ms", when, n, ms);
        else                 printf("%s  #%-4d errore %lu %5lu ms", when, n, err, ms);
        if (last != -2 && state != last)
            printf("   <-- %s", state == 1 ? "INSERITO" : state == 0 ? "TOLTO" : "CAMBIATO");
        printf("\n");
        last = state;
        if (count > 0 && n >= count) break;
        for (k = 0; k < secs * 10; k++) {
            if (quit_pressed()) { CloseHandle(h); return 0; }
            Sleep(100);
        }
    }
    CloseHandle(h);
    return 0;
}
