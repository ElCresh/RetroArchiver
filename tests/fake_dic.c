/* fake_dic - fake DiscImageCreator to test optical.c, dskrun and the
 * GUI without an optical drive. Understands "ls X", "cd X FILE.bin ...",
 * "data X FILE.bin SPEED START END ..." and "dvd X FILE.iso ...", and
 * with no arguments (or with /v) prints the
 * version like the real one. A read copies a folder from tests/fixtures/dic
 * (gen_dic.py) next to FILE, with the DISC prefix replaced by the name
 * of FILE, and echoes its console.txt.
 *
 *   FAKE_DIC_DIR      folder of the reads (default fixtures/dic)
 *   FAKE_DIC_SCRIPT   one line per read: the folder name
 *                     (cd_good, cd_noise, cd_c2, dvd_good), or FAIL
 *                     (error), WRONG (disc of the other type),
 *                     FIRMWARE (a known drive without its last firmware) or
 *                     SLOW folder (5 seconds first) or EXITn folder
 *                     (complete read but exit code n), RESETSn folder (n
 *                     drive resets first, then the folder or FAIL), LEADIN
 *                     [folder] (a drive that cannot read the lead-in: the read
 *                     fails, leaving the folder's _disc.txt with its TOC),
 *                     LEADIN2 [folder] (the same, stopping at LBA -2
 *                     without saying so, like the Sony AD-7200S);
 *                     without a script cd_good, data_good or dvd_good. A
 *                     data read fails if END is not the folder's DISC.bin end
 *   FAKE_DIC_NODRIVE  if defined, "ls" fails as with a wrong drive
 *   FAKE_DIC_PACE     milliseconds to pause after each line printed (the
 *                     window can be seen reading, for screenshots)
 *   FAKE_DIC_ASK_OFFSET  if defined, the drive is not in driveOffset.txt: a
 *                     CD read asks for the offset on the input and reads it
 *                     as DIC 20260101 does (scanf, then a character): with
 *                     NUL it fails with "GetLastError: 6"
 *   FAKE_DIC_LOG      file where each read appends one line: "cd sf" or
 *                     "cd nosf" ("data 0-300 nosf" for data), then "offset
 *                     N", "offset failed" or "offset none" (not asked)
 *
 * The position in the script is kept in SCRIPT.pos, as in the fake SAMdisk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define sleep5() Sleep(5000)
#define pause_ms(ms) Sleep(ms)
#else
#include <dirent.h>
#include <unistd.h>
#define sleep5() sleep(5)
#define pause_ms(ms) usleep((useconds_t)(ms) * 1000)
#endif

static void version(void)
{
    printf("AppVersion\n\t32 bit, AnsiBuild, 20260101T233129\n");
}

static int next_line(const char *script, char *out, size_t n)
{
    char pos[1024], line[256]; FILE *f; int k = 0, i;
    snprintf(pos, sizeof(pos), "%s.pos", script);
    f = fopen(pos, "r");
    if (f) { if (fscanf(f, "%d", &k) != 1) k = 0; fclose(f); }
    f = fopen(script, "r");
    if (!f) return -1;
    for (i = 0; i <= k; i++)
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    snprintf(out, n, "%s", line);
    f = fopen(pos, "w");
    if (f) { fprintf(f, "%d\n", k + 1); fclose(f); }
    return 0;
}

static int is_text(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcmp(dot, ".txt") || !strcmp(dot, ".cue") || !strcmp(dot, ".dat") || !strcmp(dot, ".ccd"));
}

/* "DISC (Track 1).bin" -> "CD01 (Track 1).bin" */
static void rename_to(const char *s, const char *base, char *out, size_t n)
{
    size_t k = 0;
    while (*s && k + 1 < n) {
        if (!strncmp(s, "DISC", 4)) {
            size_t b = strlen(base);
            if (k + b >= n) break;
            memcpy(out + k, base, b); k += b; s += 4;
        } else {
            out[k++] = *s++;
        }
    }
    out[k] = 0;
}

static int copy_file(const char *src, const char *dst, const char *base)
{
    static char buf[65536];
    FILE *in = fopen(src, "rb"), *out; size_t n;
    if (!in) return -1;
    if (!(out = fopen(dst, "wb"))) { fclose(in); return -1; }
    if (is_text(src)) {
        char line[2048], fixed[4096];
        while (fgets(line, sizeof(line), in)) {
            rename_to(line, base, fixed, sizeof(fixed));
            fputs(fixed, out);
        }
    } else {
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* the name of the read and its folder, from FILE ("dir/CD01.bin") */
static void split(const char *file, char *dir, size_t dn, char *base, size_t bn)
{
    const char *a = strrchr(file, '/'), *b = strrchr(file, '\\'), *dot;
    if (b > a) a = b;
    if (a) snprintf(dir, dn, "%.*s", (int)(a - file), file); else snprintf(dir, dn, ".");
    snprintf(base, bn, "%s", a ? a + 1 : file);
    if ((dot = strrchr(base, '.')) != NULL) base[dot - base] = 0;
}

static int copy_one(const char *src_dir, const char *name, const char *dir, const char *base)
{
    char src[2048], dst[2048], nn[512];
    if (!strcmp(name, "console.txt") || name[0] == '.') return 0;
    snprintf(src, sizeof(src), "%s/%s", src_dir, name);
    rename_to(name, base, nn, sizeof(nn));
    snprintf(dst, sizeof(dst), "%s/%s", dir, nn);
    return copy_file(src, dst, base);
}

static int copy_dir(const char *src_dir, const char *dir, const char *base)
{
    int bad = 0;
#ifdef _WIN32
    char pat[1024]; WIN32_FIND_DATAA fd; HANDLE h;
    snprintf(pat, sizeof(pat), "%s\\*", src_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && copy_one(src_dir, fd.cFileName, dir, base)) bad = 1;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(src_dir); struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.' && copy_one(src_dir, e->d_name, dir, base)) bad = 1;
    closedir(d);
#endif
    return bad ? -1 : 0;
}

/* console.txt: the progress lines ("... 120/ 420") rewrite the same
 * line. FAKE_DIC_PACE=ms: a pause after each line, to see the window
 * during a read (screenshots on the VM) */
static void replay(const char *src_dir, const char *base)
{
    char p[1100], line[1024], fixed[2048]; FILE *f; size_t k;
    const char *pace = getenv("FAKE_DIC_PACE");
    int ms = pace ? atoi(pace) : 0;
    snprintf(p, sizeof(p), "%s/console.txt", src_dir);
    if (!(f = fopen(p, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        rename_to(line, base, fixed, sizeof(fixed));
        k = strlen(fixed);
        printf("%s%s", fixed, k && fixed[k - 1] >= '0' && fixed[k - 1] <= '9' &&
                              (strchr(fixed, '/') || strstr(fixed, "(LBA)")) ? "\r" : "\n");
        if (ms > 0) { fflush(stdout); pause_ms(ms); }
    }
    printf("\n");
    fclose(f);
}

/* GetDriveOffsetManually of DIC 20260101: the same scanf and getchar */
static int ask_offset(int *v)
{
    char buf[8] = "";
    printf("This drive doesn't define in driveOffset.txt\n"
           "Please input drive offset(Samples): ");
    fflush(stdout);
    if (!scanf("%6[^\n]%*[^\n]", buf) || getchar() == EOF) {
        printf("[F:GetDriveOffsetManually][L:1234] GetLastError: 6, The handle is invalid.\n\n");
        return 0;
    }
    *v = atoi(buf);
    return 1;
}

enum { CD, DVD, DATA };
static long data_start, data_end;

/* FAKE_DIC_LOG: how the read was asked for */
static void log_read(int kind, int sf, const char *off)
{
    const char *p = getenv("FAKE_DIC_LOG"); FILE *f;
    if (!p || !(f = fopen(p, "a"))) return;
    if (kind == DATA) fprintf(f, "data %ld-%ld", data_start, data_end);
    else fprintf(f, "%s", kind == DVD ? "dvd" : "cd");
    fprintf(f, " %s offset %s\n", sf ? "sf" : "nosf", off);
    fclose(f);
}

/* data: the end must be the disc's lead-out, as in the folder's TOC */
static int data_range_ok(const char *src)
{
    char p[1100]; FILE *f; long size;
    snprintf(p, sizeof(p), "%s/DISC.bin", src);
    if (!(f = fopen(p, "rb"))) { printf("fake_dic: %s has no DISC.bin for data\n", src); return 0; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    if (data_start != 0 || data_end * 2352 != size) {
        printf("fake_dic: data %ld-%ld, but the disc has %ld sectors\n", data_start, data_end, size / 2352);
        return 0;
    }
    return 1;
}

static int dump(int kind, const char *file, int sf)
{
    char line[256], dir[1024], base[256], src[1024], p[1400], off[16] = "none";
    const char *script = getenv("FAKE_DIC_SCRIPT"), *fix = getenv("FAKE_DIC_DIR");
    int dvd = kind == DVD;
    const char *what = kind == DVD ? "dvd_good" : kind == DATA ? "data_good" : "cd_good";
    int code = 0, asked = 0, have_offset = 0;
    FILE *f;

    split(file, dir, sizeof(dir), base, sizeof(base));
    version();
    printf("CurrentDirectory\n\t%s\nWorkingPath\n\t Argument: %s\n", dir, file);
    /* like the real one, the logs exist even when the read fails; the
     * drive is known before the offset question */
    snprintf(p, sizeof(p), "%s/%s_drive.txt", dir, base);
    if ((f = fopen(p, "w")) != NULL) {
        fputs("========== InquiryData ==========\n\t            VendorId: ASUS\n"
              "\t           ProductId: DRW-08D6MT\n\tProductRevisionLevel: 1.00\n", f);
        fclose(f);
    }
    if (getenv("FAKE_DIC_ASK_OFFSET") && !dvd) {
        if (!ask_offset(&asked)) { log_read(kind, sf, "failed"); return 1; }
        snprintf(off, sizeof(off), "%d", asked);
        have_offset = 1;
    }
    log_read(kind, sf, off);
    if (script) {
        if (next_line(script, line, sizeof(line))) { printf("fake_dic: script finished\n"); return 1; }
        what = line;
    }
    /* "RESETS3 cd_good": the drive resets itself 3 times, as seen on the PC
     * with /sf; with /sf and 3 or more, RetroArchiver must stop it */
    if (!strncmp(what, "RESETS", 6)) {
        const char *sp = strchr(what, ' ');
        int i, n = atoi(what + 6);
        for (i = 0; i < n; i++) {
            printf("LBA[000016, 0x00010]: [F:ReadCDForCheckingExe][L:1712]\n\tOpcode: 0xa8\n"
                   "[F:ExecSptiCmd][L:247] GetLastError: 121, The semaphore timeout period has expired.\n"
                   "\tScsiStatus: 0x02 = CHECK_CONDITION\n"
                   "\tSenseData Key-Asc-Ascq: 06-29-00 = UNIT_ATTENTION - POWER ON, RESET, OR BUS DEVICE RESET OCCURRED\n"
                   "Please wait for 40000 milliseconds until the device is returned\n");
            fflush(stdout);
        }
        if (sf && n >= 3) { printf("fake_dic: still resetting\n"); fflush(stdout); sleep5(); sleep5(); }
        what = sp ? sp + 1 : (dvd ? "dvd_good" : "cd_good");
    }
    if (!strcmp(what, "FAIL")) {
        printf("LBA[000000, 0000000]: [F:ReadCDForCheckingReadInOut][L:1022]\n"
               "\tOpcode: 0xbe\n\tScsiStatus: 0x02 = CHECK_CONDITION\n"
               "\tSenseData Key-Asc-Ascq: 02-3a-00 = NOT_READY - MEDIUM NOT PRESENT\n");
        return 1;
    }
    if (!strcmp(what, "WRONG")) {
        printf("Wrong command. The disc isn't CD, CD-R, CD-RW, GD\n");
        return 1;
    }
    if (!strcmp(what, "FIRMWARE")) {
        printf("[ERROR] This drive isn't latest firmware. Please update\n"
               " -> https://web.archive.org/web/20200128060702/http://www.skcj.co.jp/discon/download/index.html\n");
        return 1;
    }
    /* a drive that cannot read the lead-in, as the ASUS DRW-08D6MT on the PC;
     * "LEADIN data_good": the TOC DIC wrote before stopping is that disc's */
    if (!strncmp(what, "LEADIN", 6)) {
        /* LEADIN2: like the Sony AD-7200S (offset +48), no scrambled reading,
         * stops at LBA -2 without saying "can't read the lead-in" */
        int sony = what[6] == '2';
        const char *sp = strchr(what, ' ');
        if (sp) {
            snprintf(src, sizeof(src), "%s/%s", fix ? fix : "fixtures/dic", sp + 1);
            copy_one(src, "DISC_disc.txt", dir, base);
        }
        if (sony) {
            printf("[WARNING] This drive does not support scrambled reading\n"
                   "Set OpCode: 0xbe, SubCode: 1(Raw)\n"
                   "LBA[-00002, 0xfffffffe]: [F:ReadCDAll][L:1098]\n\tOpcode: 0xbe\n"
                   "\tScsiStatus: 0x02 = CHECK_CONDITION\n"
                   "\tSenseData Key-Asc-Ascq: 05-21-00 = ILLEGAL_REQUEST - LOGICAL BLOCK ADDRESS OUT OF RANGE\n"
                   "lpCmd: be, 04, ff, ff, ff, fe, 00, 00, 01, f8, 01, 00\ndwBufSize: 2742\n");
            return 1;
        }
        printf("This drive can't read the lead-in\nSet OpCode: 0xbe, SubCode: 1(Raw)\n"
               "LBA[-00001, 0xffffffff]: [F:ReadCDAll][L:1114]\n\tOpcode: 0xbe\n"
               "\tScsiStatus: 0x02 = CHECK_CONDITION\n"
               "\tSenseData Key-Asc-Ascq: 05-2c-00 = ILLEGAL_REQUEST - COMMAND SEQUENCE ERROR\n"
               "lpCmd: be, 04, ff, ff, ff, ff, 00, 00, 01, fa, 01, 00\ndwBufSize: 2742\n");
        return 1;
    }
    if (!strncmp(what, "SLOW", 4)) {
        printf("fake_dic: slow\n");
        fflush(stdout);
        sleep5();
        what = what[4] == ' ' ? what + 5 : (dvd ? "dvd_good" : "cd_good");
    }
    /* "EXIT3 cd_good": complete read, but DIC exits with 3 */
    if (!strncmp(what, "EXIT", 4)) {
        const char *sp = strchr(what, ' ');
        code = atoi(what + 4);
        what = sp ? sp + 1 : (dvd ? "dvd_good" : "cd_good");
    }
    snprintf(src, sizeof(src), "%s/%s", fix ? fix : "fixtures/dic", what);
    if (kind == DATA && !data_range_ok(src)) return 1;
    if (copy_dir(src, dir, base)) { printf("fake_dic: cannot copy %s to %s\n", src, dir); return 1; }
    replay(src, base);
    /* DIC writes the offset it used in _disc.txt */
    if (have_offset) {
        snprintf(p, sizeof(p), "%s/%s_disc.txt", dir, base);
        if ((f = fopen(p, "a")) != NULL) {
            fprintf(f, "\t-         Drive Offset(Byte) %6d, (Samples) %5d\n", asked * 4, asked);
            fclose(f);
        }
    }
    if (code) printf("LBA[123456, 0x1e240]: [F:ReadCDForCheckingReadInOut][L:999] Read error (simulated)\n");
    return code;
}

int main(int argc, char **argv)
{
    /* with no arguments, like the real one: help, version and "Press any key",
     * which with input from NUL does not wait */
    if (argc < 2) {
        printf("Press any key to continue . . . \n");
        version();
        printf("Usage\n\t/v\tPrint version\n\tcd <DriveLetter> <Filename> <DriveSpeed(0-72)> ...\n");
        getchar();
        return 1;
    }
    if (!strcmp(argv[1], "/v")) {
        printf("Invalid argument\nIf you see the usage, execute .exe without argument\n");
        version();
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "ls")) {
        if (getenv("FAKE_DIC_NODRIVE")) {
            printf("[F:GetHandle][L:57] GetLastError: 5, Access is denied.\n\n");
            version();
            return 1;
        }
        version();
        printf("StartTime: 2026-10-16T21:24:40+0200\nEndTime: 2026-10-16T21:24:40+0200\n");
        return 0;
    }
    if (argc >= 5 && (!strcmp(argv[1], "cd") || !strcmp(argv[1], "dvd")))
    {
        int i, sf = 0;
        for (i = 5; i < argc; i++) if (!strcmp(argv[i], "/sf")) sf = 1;
        return dump(!strcmp(argv[1], "dvd") ? DVD : CD, argv[3], sf);
    }
    /* data <DriveLetter> <Filename> <DriveSpeed(0-72)> <StartLBA> <EndLBA+1> */
    if (argc >= 7 && !strcmp(argv[1], "data"))
    {
        int i, sf = 0;
        for (i = 7; i < argc; i++) if (!strcmp(argv[i], "/sf")) sf = 1;
        data_start = atol(argv[5]);
        data_end = atol(argv[6]);
        return dump(DATA, argv[3], sf);
    }
    printf("fake_dic: unexpected command\n");
    return 2;
}
