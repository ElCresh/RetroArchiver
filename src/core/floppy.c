/* floppy - the floppy medium: SAMdisk 3.x to read, compare for the
 * verdict, edsk and fat12 for the technical data. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"
#include "edsk.h"
#include "fat12.h"
#include "fsu8.h"

static char samdisk[512] = "samdisk";
/* where to read from: the drive, or an image (testing with the real
 * SAMdisk on a machine without a floppy controller) */
static char source[512] = "a:";

void floppy_set_samdisk(const char *path) { str_copy(samdisk, sizeof(samdisk), path); }
void floppy_set_source(const char *src) { str_copy(source, sizeof(source), src); }

int floppy_source_is_drive(const char *s)
{
    while (*s == ' ') s++;
    if (!isalpha((unsigned char)s[0]) || s[1] != ':') return 0;
    s += 2;
    if (*s == '\\' || *s == '/') s++;
    while (*s == ' ') s++;
    return *s == 0;
}

/* without arguments SAMdisk prints "SAMdisk 3.8.12 (date), (c) ...": the
 * first line starting with "SAMdisk ", second word */
int floppy_samdisk_series(const char *s, char *ver, size_t n)
{
    while (*s) {
        if (!strncmp(s, "SAMdisk ", 8)) {
            size_t k = 0;
            s += 8;
            while (*s == ' ') s++;
            while (*s && *s != ' ' && *s != '\r' && *s != '\n' && k + 1 < n) ver[k++] = *s++;
            ver[k] = 0;
            return k > 0 && isdigit((unsigned char)ver[0]) ? atoi(ver) : 0;
        }
        while (*s && *s != '\n') s++;
        if (*s) s++;
    }
    if (n) ver[0] = 0;
    return 0;
}

/* a number right before "rpm" ("300.0 rpm") or right after ("rpm: 300") */
int floppy_parse_rpm(const char *text)
{
    const char *p, *q;
    for (p = text; (p = strstr(p, "rpm")) != NULL; p += 3) {
        double v = -1;
        for (q = p; q > text && q[-1] == ' '; q--) ;
        if (q > text && isdigit((unsigned char)q[-1])) {
            while (q > text && (isdigit((unsigned char)q[-1]) || q[-1] == '.')) q--;
            v = atof(q);
        } else {
            for (q = p + 3; *q == ' ' || *q == ':' || *q == '='; q++) ;
            if (isdigit((unsigned char)*q)) v = atof(q);
        }
        if (v >= 50 && v < 2000) return (int)(v + 0.5);
    }
    return -1;
}

/* samdisk list does not list floppies: to know whether the drive
 * responds, measure the rotation */
static int check(strbuf *msg)
{
    const char *argv[] = { samdisk, "rpm", source, NULL };
    int code = 0, rc = runner_run(argv, NULL, NULL, &code);
    if (rc == RUNNER_NOSTART) {
        sb_printf(msg, "Cannot find %s: put it in the program folder or in the PATH.", samdisk);
        return -1;
    }
    if (rc != RUNNER_DONE) {
        sb_printf(msg, "Check of drive %s canceled: try again when you like.", source);
        return -1;
    }
    if (code != 0) {
        sb_printf(msg, "Drive %s does not respond (samdisk rpm gave error %d): "
                       "check fdrawcmd.sys and that there is a floppy in the drive.", source, code);
        return -1;
    }
    return 0;
}

/* a missing SAMdisk must be reported as such, not as a damaged floppy */
static int no_tool(runner_line_fn on_line, void *ctx)
{
    char line[640];
    snprintf(line, sizeof(line), "Cannot find %s: put it in the program folder "
                                 "or in the PATH, or give the full path of samdisk.exe.", samdisk);
    if (on_line) on_line(ctx, line);
    return MEDIA_NOTOOL;
}

typedef struct { FILE *f; runner_line_fn fn; void *ctx; } tee;

static void tee_line(void *c, const char *line)
{
    tee *t = (tee *)c;
    if (t->f) fprintf(t->f, "%s\n", line);
    if (t->fn) t->fn(t->ctx, line);
}

/* the exit code of scan does not matter: on a damaged disk it reports
 * errors, but the text is needed all the same */
static int scan(const char *out_txt, runner_line_fn on_line, void *ctx)
{
    const char *argv[] = { samdisk, "scan", source, NULL };
    tee t; int code = 0, rc;
    t.f = u8_fopen(out_txt, "wb"); t.fn = on_line; t.ctx = ctx;
    if (!t.f) return -1;
    rc = runner_run_tty(argv, tee_line, &t, &code);
    fclose(t.f);
    if (rc == RUNNER_NOSTART) { u8_remove(out_txt); return no_tool(on_line, ctx); }
    if (rc == RUNNER_CANCELED) return 1;
    return rc == RUNNER_DONE ? 0 : -1;
}

static int careful_mode;

static void careful(int on) { careful_mode = on; }

/* a track line of the scan is "  10.0  1 2 3 4 5[dc] 6 ..."; SAMdisk puts
 * the sector problems in square brackets (dc, ic, nd, ...), while "7d"
 * (deleted data) is normal and copy protections use it too */
static int problems(const char *scan_txt)
{
    FILE *f = u8_fopen(scan_txt, "rb");
    char line[1024];
    int n = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') continue;
        while (*p >= '0' && *p <= '9') p++;
        if (*p != '.' || p[1] < '0' || p[1] > '9' || p[2] != ' ') continue;
        if (strchr(p, '[')) n++;
    }
    fclose(f);
    return n;
}

static int read_disk(const char *out_img, runner_line_fn on_line, void *ctx)
{
    const char *normal[]  = { samdisk, "copy", source, out_img, NULL };
    const char *fragile[] = { samdisk, "copy", source, out_img, "--retries=30", "--rescans=5", NULL };
    const char *const *argv = careful_mode ? fragile : normal;
    int code = 0, rc = runner_run_tty(argv, on_line, ctx, &code);
    if (rc == RUNNER_DONE && code == 0 && u8_filesize(out_img) > 0) return 0;
    u8_remove(out_img);               /* no half-written files in data/ */
    if (rc == RUNNER_NOSTART) return no_tool(on_line, ctx);
    return rc == RUNNER_CANCELED ? 1 : -1;
}

static const char *format_name(int nsec, int cyl, int heads, char *buf)
{
    if (heads == 2 && cyl == 80 && nsec == 18) return "1.44M";
    if (heads == 2 && cyl == 80 && nsec == 9)  return "720K";
    if (heads == 2 && cyl == 80 && nsec == 15) return "1.2M";
    if (heads == 2 && cyl == 80 && nsec == 36) return "2.88M";
    if (heads == 2 && cyl == 40 && nsec == 9)  return "360K";
    if (heads == 1 && cyl == 40 && nsec == 9)  return "180K";
    sprintf(buf, "%d cylinders x %d heads x %d sectors", cyl, heads, nsec);
    return buf;
}

static void tech(void *ctx, const char *img, json_writer *w)
{
    edsk d; strbuf msg; fat_info fi; char buf[64]; int i, std;

    (void)ctx;
    sb_init(&msg);
    if (!edsk_load(img, &d, &msg) || !d.ok) {
        jw_null(w, "geometry");
        jw_null(w, "filesystem");
        jw_str(w, "filesystem_note", "image not readable as EDSK");
        edsk_free(&d);
        sb_free(&msg);
        return;
    }
    std = edsk_snap(d.nomcyl);
    jw_begin_obj(w, "geometry");
    jw_str(w, "format", format_name(d.nsec0, std, d.heads, buf));
    jw_int(w, "cylinders", std);
    jw_int(w, "heads", d.heads);
    jw_int(w, "sectors", d.nsec0);
    jw_int(w, "captured_cylinders", d.cyls);
    jw_end_obj(w);

    if (fat12_read(&d, &fi)) {
        jw_begin_obj(w, "filesystem");
        jw_str(w, "type", "FAT12");
        jw_str(w, "volume_label", fi.volume_label[0] ? fi.volume_label : NULL);
        jw_begin_arr(w, "files");
        for (i = 0; i < fi.nfiles; i++) {
            jw_begin_obj(w, NULL);
            jw_str(w, "name", fi.files[i].path);
            if (fi.files[i].isdir) jw_str(w, "type", "dir");
            else jw_int(w, "bytes", fi.files[i].size);
            jw_str(w, "date", fi.files[i].date[0] ? fi.files[i].date : NULL);
            jw_end_obj(w);
        }
        jw_end_arr(w);
        jw_end_obj(w);
        if (fi.note[0]) jw_str(w, "filesystem_note", fi.note);
        fat12_free(&fi);
    } else {
        jw_null(w, "filesystem");
        jw_str(w, "filesystem_note", fi.note);
    }
    edsk_free(&d);
    sb_free(&msg);
}

static int volume_label(const char *img, char *out, size_t n)
{
    edsk d; fat_info fi; strbuf msg; int ok = 0;
    sb_init(&msg);
    memset(&fi, 0, sizeof(fi));
    memset(&d, 0, sizeof(d));
    if (edsk_load(img, &d, &msg) && d.ok && fat12_read(&d, &fi) && fi.volume_label[0]) {
        str_copy(out, n, fi.volume_label);
        ok = 1;
    }
    fat12_free(&fi);
    edsk_free(&d);
    sb_free(&msg);
    return ok;
}

const media_ops floppy_media = {
    "floppy", ".dsk", check, scan, read_disk, compare_files, tech, problems, careful,
    "SAMdisk", "Scan", 0, NULL, volume_label
};
