/* A box becomes an item on Archive.org: see iaupload.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bearssl.h>
#include "iaupload.h"
#include "archive.h"
#include "b64.h"
#include "bagit.h"
#include "fsu8.h"
#include "https.h"
#include "json.h"
#include "messages.h"
#include "sha256.h"
#include "session.h"

#ifdef _WIN32
#include <windows.h>
static void nap_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void nap_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

#define CP(dst, src) str_copy(dst, sizeof(dst), src)
#define HASH_PIECE 65536

/* ---- the box ---- */

int ia_box_can_upload(const char *bagdir)
{
    arc_box b;
    if (!arc_load(bagdir, &b)) return IA_BOX_NOTBOX;
    if (b.state == ARC_OPEN) return IA_BOX_OPEN;
    if (b.state == ARC_BROKEN) return IA_BOX_BROKEN;
    if (b.editing) return IA_BOX_EDITING;
    if (b.old_format) return IA_BOX_OLD;
    return IA_BOX_OK;
}

/* ---- the identifier ---- */

int ia_ident_ok(const char *id)
{
    size_t i, n = id ? strlen(id) : 0;
    if (n < 5 || n > 100) return 0;
    for (i = 0; i < n; i++) {
        char c = id[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!i && !alnum) return 0;
        if (!alnum && c != '.' && c != '_' && c != '-') return 0;
    }
    return 1;
}

static int utf8_next(const unsigned char *p, unsigned long *cp)
{
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((unsigned long)(p[0] & 31) << 6) | (p[1] & 63); return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((unsigned long)(p[0] & 15) << 12) | ((unsigned long)(p[1] & 63) << 6) | (p[2] & 63);
        return 3;
    }
    *cp = '?';
    return 1;
}

/* an accented letter without its accent: the letters of cp1252, which is
 * already the alphabet of the folder names (bag_dirname) */
static const char *fold_letter(unsigned long cp)
{
    static const struct { unsigned long cp; const char *s; } map[] = {
        { 0xC0, "a" }, { 0xC1, "a" }, { 0xC2, "a" }, { 0xC3, "a" }, { 0xC4, "a" }, { 0xC5, "a" },
        { 0xC6, "ae" }, { 0xC7, "c" }, { 0xC8, "e" }, { 0xC9, "e" }, { 0xCA, "e" }, { 0xCB, "e" },
        { 0xCC, "i" }, { 0xCD, "i" }, { 0xCE, "i" }, { 0xCF, "i" }, { 0xD0, "d" }, { 0xD1, "n" },
        { 0xD2, "o" }, { 0xD3, "o" }, { 0xD4, "o" }, { 0xD5, "o" }, { 0xD6, "o" }, { 0xD8, "o" },
        { 0xD9, "u" }, { 0xDA, "u" }, { 0xDB, "u" }, { 0xDC, "u" }, { 0xDD, "y" }, { 0xDE, "th" },
        { 0xDF, "ss" }, { 0xFF, "y" }, { 0x0152, "oe" }, { 0x0153, "oe" },
        { 0x0160, "s" }, { 0x0161, "s" }, { 0x017D, "z" }, { 0x017E, "z" }, { 0, NULL }
    };
    int i;
    if (cp >= 0xE0 && cp <= 0xFE) cp -= 0x20;            /* lowercase to its capital */
    for (i = 0; map[i].s; i++) if (map[i].cp == cp) return map[i].s;
    return NULL;
}

/* "Citta' perduta 2.0" -> "citta-perduta-2-0" */
static void slug_add(strbuf *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned long cp; int k;
    while (*p) {
        const char *f;
        k = utf8_next(p, &cp);
        p += k;
        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
            char c[2]; c[0] = (char)cp; c[1] = 0;
            sb_puts(out, c);
        } else if ((f = fold_letter(cp)) != NULL) {
            sb_puts(out, f);
        } else if (out->len && out->s[out->len - 1] != '-') {
            sb_puts(out, "-");
        }
    }
}

/* the dashes squeezed, none at the ends, cut at n or at IA_ID_PROPOSE on a dash */
static void slug_trim(const strbuf *in, char *out, size_t n)
{
    size_t i, o = 0, max = n - 1 < IA_ID_PROPOSE ? n - 1 : IA_ID_PROPOSE;
    for (i = 0; i < in->len && o < max; i++) {
        if (in->s[i] == '-' && (!o || out[o - 1] == '-')) continue;
        out[o++] = in->s[i];
    }
    while (o && out[o - 1] == '-') o--;
    out[o] = 0;
    /* cut on a dash, so that a word is not left halved */
    if (i < in->len) {
        char *d = strrchr(out, '-');
        if (d && d - out >= 5) *d = 0;
    }
}

void ia_ident_propose(const meta_box *b, const char *prefix, char *out, size_t n)
{
    strbuf s;
    const char *marc = b->language[0] ? ia_lang_marc(b->language) : "";
    sb_init(&s);
    if (prefix && prefix[0]) { slug_add(&s, prefix); sb_puts(&s, "-"); }
    slug_add(&s, b->title);
    if (b->edition[0]) { sb_puts(&s, "-"); slug_add(&s, b->edition); }
    if (b->platform[0]) { sb_puts(&s, "-"); slug_add(&s, b->platform); }
    if (marc[0] && strcmp(marc, "eng")) { sb_puts(&s, "-"); slug_add(&s, marc); }
    slug_trim(&s, out, n);
    /* the slug holds only letters, digits and dashes, and no dash at the
     * ends: all that is left is a title with nothing usable in it, and the
     * five characters the rules want */
    if (!out[0]) str_copy(out, n, "item");
    while (strlen(out) < 5 && strlen(out) + 2 < n) strcat(out, "0");
    sb_free(&s);
}

/* ---- the languages ---- */

const char *ia_lang_marc(const char *name)
{
    static const struct { const char *name, *marc; } map[] = {
        { "italiano", "ita" }, { "italian", "ita" }, { "it", "ita" }, { "ita", "ita" },
        { "english", "eng" }, { "inglese", "eng" }, { "en", "eng" }, { "eng", "eng" },
        { "deutsch", "ger" }, { "german", "ger" }, { "tedesco", "ger" }, { "de", "ger" },
        { "fran\xc3\xa7" "ais", "fre" }, { "francais", "fre" }, { "french", "fre" },
        { "francese", "fre" }, { "fr", "fre" },
        { "espa\xc3\xb1ol", "spa" }, { "espanol", "spa" }, { "spanish", "spa" },
        { "spagnolo", "spa" }, { "es", "spa" },
        { "portugu\xc3\xaas", "por" }, { "portuguese", "por" }, { "pt", "por" },
        { "nederlands", "dut" }, { "dutch", "dut" }, { "nl", "dut" },
        { "svenska", "swe" }, { "swedish", "swe" }, { "sv", "swe" },
        { "dansk", "dan" }, { "danish", "dan" },
        { "norsk", "nor" }, { "suomi", "fin" }, { "finnish", "fin" },
        { "polski", "pol" }, { "polish", "pol" }, { "pl", "pol" },
        { "russian", "rus" }, { "russo", "rus" }, { "ru", "rus" },
        { "japanese", "jpn" }, { "giapponese", "jpn" }, { "ja", "jpn" },
        { "chinese", "chi" }, { "cinese", "chi" },
        { "multilingual", "mul" }, { "multilingua", "mul" }, { NULL, NULL }
    };
    char low[64]; size_t i;
    str_copy(low, sizeof(low), name);
    for (i = 0; low[i]; i++) if (low[i] >= 'A' && low[i] <= 'Z') low[i] += 'a' - 'A';
    while (i && (low[i - 1] == ' ' || low[i - 1] == '\t')) low[--i] = 0;
    for (i = 0; map[i].name; i++) if (!strcmp(low, map[i].name)) return map[i].marc;
    return name;                        /* unknown: as it is */
}

/* "Italiano, English" -> two fields */
static void languages_of(const char *list, ia_meta *m)
{
    const char *p = list, *e;
    char one[64];
    while (*p && m->nlang < IA_MAX_LANG) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        e = strchr(p, ',');
        if (!e) e = p + strlen(p);
        snprintf(one, sizeof(one), "%.*s", (int)(e - p) < 63 ? (int)(e - p) : 63, p);
        while (one[0] && one[strlen(one) - 1] == ' ') one[strlen(one) - 1] = 0;
        if (one[0]) CP(m->language[m->nlang++], ia_lang_marc(one));
        p = *e ? e + 1 : e;
    }
}

/* ---- bag-info.txt ---- */

typedef struct { char status[24], profile[160], missing[300], date[20]; } ia_baginfo;

static void baginfo_load(const char *bagdir, ia_baginfo *bi)
{
    char p[1024], line[1200], *colon, *e; FILE *f;
    memset(bi, 0, sizeof(*bi));
    u8_join(p, sizeof(p), bagdir, "bag-info.txt");
    if (!(f = u8_fopen(p, "rb"))) return;
    while (fgets(line, sizeof(line), f)) {
        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!(colon = strstr(line, ": "))) continue;
        *colon = 0;
        if (!strcmp(line, "Box-Status")) CP(bi->status, colon + 2);
        else if (!strcmp(line, "BagIt-Profile-Identifier")) CP(bi->profile, colon + 2);
        else if (!strcmp(line, "Missing-Media")) CP(bi->missing, colon + 2);
        else if (!strcmp(line, "Bagging-Date")) CP(bi->date, colon + 2);
    }
    fclose(f);
}

/* ---- the generated description ---- */

static char *read_text(const char *path)
{
    FILE *f = u8_fopen(path, "rb"); long n; char *s;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    s = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

/* software.retroarchiver of a metadata.json: the version that last wrote it */
static void file_version(const char *path, char *out, size_t n)
{
    char *text = read_text(path);
    jval *root = text ? json_parse(text, NULL, 0) : NULL;
    const jval *sw = root ? json_get(root, "software") : NULL;
    str_copy(out, n, sw ? json_str(sw, "retroarchiver") : "");
    json_free(root);
    free(text);
}

/* the reading programs, from the technical data of metadata.json */
static void tools_of(const char *bagdir, const meta_box *b, strbuf *out)
{
    char path[1200], *text; jval *root = NULL; const jval *media, *e;
    int i, floppy = 0, n = 0;
    char seen[6][80];

    for (i = 0; i < b->ndisk; i++)
        if (b->disk[i].kind == MEDIA_FLOPPY && b->disk[i].master[0]) floppy = 1;
    u8_join(path, sizeof(path), bagdir, "data" U8_SEP "metadata.json");
    text = read_text(path);
    if (text) root = json_parse(text, NULL, 0);
    media = root ? json_get(root, "media") : NULL;
    for (e = media && media->t == J_ARR ? media->child : NULL; e; e = e->next) {
        const char *tool = json_str(e, "tool");
        int k;
        if (!tool || !tool[0]) continue;
        for (k = 0; k < n && strcmp(seen[k], tool); k++) ;
        if (k == n && n < 6) CP(seen[n++], tool);
    }
    /* SAMdisk does not say its version in the package: only its name */
    if (floppy) sb_puts(out, "SAMdisk");
    for (i = 0; i < n; i++) sb_printf(out, "%s%s", out->len ? ", " : "", seen[i]);
    json_free(root);
    free(text);
}

static const meta_disk *disk_at(const meta_box *b, int n)
{
    int i;
    for (i = 0; i < b->ndisk; i++) if (b->disk[i].n == n) return &b->disk[i];
    return NULL;
}

/* the version that made the kept read of this medium, "" if unknown */
static const char *disk_version(const meta_disk *d)
{
    int i;
    if (!d) return "";
    for (i = 0; i < d->nreads; i++)
        if (d->master[0] && !strcmp(d->reads[i].file, d->master)) return d->reads[i].software;
    return d->nreads ? d->reads[0].software : "";
}

/* the last "convert" of the history, and the "edit" entries after it */
typedef struct { char convert[16], convert_at[20]; } ia_history;

/* the version alone: boxedit writes "RetroArchiver 261026" in the history,
 * the reads only "261026" (seen on the first real update: the name twice) */
static const char *sw_version(const char *sw)
{
    static const char name[] = "RetroArchiver ";
    if (sw && !strncmp(sw, name, sizeof(name) - 1)) return sw + sizeof(name) - 1;
    return sw ? sw : "";
}

static void history_scan(const meta_box *b, ia_history *h, strbuf *edits)
{
    jval *root; const jval *e;
    memset(h, 0, sizeof(*h));
    if (!b->history[0] || (root = json_parse(b->history, NULL, 0)) == NULL) return;
    for (e = root->t == J_ARR ? root->child : NULL; e; e = e->next) {
        const char *act = json_str(e, "action");
        if (act && !strcmp(act, "convert")) {
            CP(h->convert, sw_version(json_str(e, "software")));
            CP(h->convert_at, json_str(e, "at"));
            sb_clear(edits);                 /* only the edits after the conversion count */
        } else if (act && !strcmp(act, "edit")) {
            const char *sw = sw_version(json_str(e, "software")), *at = json_str(e, "at");
            sb_printf(edits, "%sRetroArchiver %s (%.10s)", edits->len ? ", " : "",
                      sw && sw[0] ? sw : "version unknown", at ? at : "");
        }
    }
    json_free(root);
}

/* the version of the package before any edit, for a bag that did not
 * record one per read: the oldest set-aside/previous-* */
typedef struct { char best[64]; } prev_ctx;

static void prev_pick(void *ctx, const char *name, int isdir)
{
    prev_ctx *p = (prev_ctx *)ctx;
    if (!isdir || strncmp(name, "previous-", 9)) return;
    if (!p->best[0] || strcmp(name, p->best) < 0) str_copy(p->best, sizeof(p->best), name);
}

static void previous_version(const char *bagdir, char *out, size_t n)
{
    char dir[1024], sub[1200], path[1400]; prev_ctx p;
    out[0] = 0;
    memset(&p, 0, sizeof(p));
    u8_join(dir, sizeof(dir), bagdir, session_aside_dir(bagdir));
    u8_listdir(dir, prev_pick, &p);
    if (!p.best[0]) return;
    u8_join(sub, sizeof(sub), dir, p.best);
    u8_join(path, sizeof(path), sub, "metadata.json");
    file_version(path, out, n);
}

/* "floppy 1-3", from the media whose version is v */
static void media_range(const meta_box *b, const char *v, const char *fallback, strbuf *out)
{
    int n, kind, num, run_kind = -1, run_from = 0, run_to = 0;
    char name[32];
    for (n = 1; n <= b->media_expected + 1; n++) {
        const meta_disk *d = n <= b->media_expected ? disk_at(b, n) : NULL;
        const char *dv = disk_version(d);
        int mine = d != NULL && !strcmp(dv[0] ? dv : fallback, v);
        meta_slot(b, n, &kind, &num);
        if (mine && run_kind == kind && run_to == num - 1) { run_to = num; continue; }
        if (run_kind >= 0) {
            meta_disk_name(run_kind, run_from, name, sizeof(name));
            sb_printf(out, "%s%s", out->len ? ", " : "", name);
            if (run_to > run_from) sb_printf(out, "-%d", run_to);
            run_kind = -1;
        }
        if (mine) { run_kind = kind; run_from = run_to = num; }
    }
}

/* the trace lines: which version produced the package */
static void trace_lines(const char *bagdir, const meta_box *b, strbuf *out)
{
    ia_history h; strbuf edits, versions; int n, i, nv = 0;
    char fallback[64] = "", list[8][16];

    sb_init(&edits);
    history_scan(b, &h, &edits);
    if (h.convert[0]) {
        /* the conversion remade the package: that version alone */
        sb_printf(out, "Archived with: RetroArchiver %s\n", h.convert);
    } else {
        /* the version of each kept read, grouped; a bag made before they
         * were recorded says what it can */
        for (n = 1; n <= b->media_expected; n++) {
            const char *v = disk_version(disk_at(b, n));
            if (!v[0]) continue;
            for (i = 0; i < nv && strcmp(list[i], v); i++) ;
            if (i == nv && nv < 8) CP(list[nv++], v);
        }
        if (!nv) {
            /* a bag made before the version of each read was recorded: with
             * no edit, the one that wrote the package; with edits, the
             * oldest metadata.json kept aside. Better to say it is not
             * known than to name the wrong version */
            if (!b->history[0]) {
                char path[1200];
                u8_join(path, sizeof(path), bagdir, "data" U8_SEP "metadata.json");
                file_version(path, fallback, sizeof(fallback));
            } else {
                previous_version(bagdir, fallback, sizeof(fallback));
            }
            if (fallback[0]) sb_printf(out, "Archived with: RetroArchiver %s\n", fallback);
            else sb_puts(out, "Archived with: RetroArchiver (version unknown, before 261026)\n");
        } else if (nv == 1) {
            sb_printf(out, "Archived with: RetroArchiver %s\n", list[0]);
        } else {
            sb_init(&versions);
            for (i = 0; i < nv; i++) {
                strbuf r;
                sb_init(&r);
                media_range(b, list[i], "", &r);
                sb_printf(&versions, "%sRetroArchiver %s (%s)", versions.len ? ", " : "",
                          list[i], r.s ? r.s : "");
                sb_free(&r);
            }
            sb_printf(out, "Archived with: %s\n", versions.s ? versions.s : "");
            sb_free(&versions);
        }
    }
    if (edits.len) sb_printf(out, "Edited with: %s\n", edits.s);
    sb_free(&edits);
}

/* one line per medium: what it is and how well it was read */
static void outcome_lines(const meta_box *b, strbuf *out)
{
    int n;
    char name[32];
    for (n = 1; n <= b->media_expected; n++) {
        const meta_disk *d = disk_at(b, n);
        const char *st = d && d->status[0] ? d->status : ST_MISSING;
        meta_disk_name(d ? d->kind : MEDIA_FLOPPY, d ? d->num : n, name, sizeof(name));
        if (!d) { int kind, num; meta_slot(b, n, &kind, &num); meta_disk_name(kind, num, name, sizeof(name)); }
        sb_printf(out, "%s: %s", name, st);
        if (d && !strcmp(d->verdict, "edc-ecc")) sb_puts(out, ", EDC/ECC verified (not redump-grade)");
        else if (d && d->agree >= 2 && d->kind == MEDIA_FLOPPY)
            sb_printf(out, " (%d reads agree)", d->agree);
        if (d && d->unstable_tracks > 0) sb_printf(out, " (%d unstable tracks)", d->unstable_tracks);
        sb_puts(out, "\n");
    }
}

static void description_of(const char *bagdir, const meta_box *b, const ia_baginfo *bi,
                           char *out, size_t n)
{
    strbuf d, tools;
    sb_init(&d);
    sb_init(&tools);
    if (b->notes[0]) sb_printf(&d, "%s\n\n", b->notes);
    outcome_lines(b, &d);
    if (bi->status[0] && !bag_status_complete(bi->status))
        sb_printf(&d, "Box incomplete: %s missing\n", bi->missing[0] ? bi->missing : "some media");
    tools_of(bagdir, b, &tools);
    if (tools.len) sb_printf(&d, "Read with: %s\n", tools.s);
    sb_puts(&d, "Files: a BagIt bag; see manifest-sha256.txt\n");
    trace_lines(bagdir, b, &d);
    str_copy(out, n, d.s ? d.s : "");
    sb_free(&tools);
    sb_free(&d);
}

/* ---- the fields ---- */

static void subject_add(ia_meta *m, const char *s)
{
    int i;
    if (!s || !s[0] || m->nsubj >= IA_MAX_SUBJECT) return;
    for (i = 0; i < m->nsubj; i++) if (!strcmp(m->subject[i], s)) return;
    CP(m->subject[m->nsubj++], s);
}

int ia_meta_from_box(const char *bagdir, const char *collection, ia_meta *out, strbuf *err)
{
    char data[1040], comp[80]; meta_box *b; ia_baginfo bi; int k, rc;
    static const char *const kind_subject[MEDIA_KINDS] = {
        "floppy disk image", "CD-ROM image", "DVD-ROM image"
    };

    memset(out, 0, sizeof(*out));
    if ((b = meta_new()) == NULL) { if (err) sb_printf(err, M_SES_NOMEM); return -1; }
    u8_join(data, sizeof(data), bagdir, "data");
    rc = meta_load(b, data, err);
    if (rc) { meta_delete(b); return -1; }
    baginfo_load(bagdir, &bi);

    CP(out->mediatype, "software");
    CP(out->collection, collection && collection[0] ? collection : "open_source_software");
    if (b->edition[0]) snprintf(out->title, sizeof(out->title), "%.255s (%.120s)", b->title, b->edition);
    else CP(out->title, b->title);
    CP(out->creator, b->publisher);
    CP(out->publisher, b->publisher);
    /* date only when the year is a year, as the schema recommends */
    if (strlen(b->year) == 4 && b->year[0] >= '0' && b->year[0] <= '9') CP(out->date, b->year);
    CP(out->platform, b->platform);
    CP(out->edition, b->edition);
    meta_composition(b, comp, sizeof(comp));
    CP(out->media, comp);
    CP(out->rights, "Copyright the publisher. Archived for preservation; "
                    "all rights remain with the copyright holders.");
    CP(out->scanner, "RetroArchiver " RA_VERSION);
    CP(out->profile, bi.profile[0] ? bi.profile : BAG_PROFILE);
    if (b->language[0]) languages_of(b->language, out);
    subject_add(out, b->platform);
    for (k = 0; k < MEDIA_KINDS; k++) if (meta_count(b, k)) subject_add(out, kind_subject[k]);
    subject_add(out, "retro software");
    subject_add(out, "BagIt");
    description_of(bagdir, b, &bi, out->description, sizeof(out->description));
    meta_delete(b);
    return 0;
}

/* ---- the plan of the files ---- */

static int by_size(const void *a, const void *b)
{
    const ia_file *x = (const ia_file *)a, *y = (const ia_file *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void plan_add(ia_files *p, const char *bagdir, const char *name, const char *sha)
{
    ia_file f;
    char path[1400];
    memset(&f, 0, sizeof(f));
    CP(f.name, name);
    CP(f.sha256, sha ? sha : "");
    u8_join(path, sizeof(path), bagdir, name);
    f.size = u8_filesize(path);
    if (f.size < 0) f.size = 0;
    ia_files_add(p, &f);
}

static const char *digest_of(const bag_digests *d, const char *rel)
{
    int i = bag_digests_find(d, rel);
    return i >= 0 ? d->hex[i] : "";
}

int ia_plan(const char *bagdir, const ia_record *prev, ia_files *out, strbuf *err)
{
    static const char *const tail[] = { "bag-info.txt", "manifest-sha256.txt", NULL };
    bag_digests tag, man; char hex[65], path[1200]; int i, first, n;

    memset(out, 0, sizeof(*out));
    memset(&tag, 0, sizeof(tag));
    memset(&man, 0, sizeof(man));
    if (bag_manifest_load(bagdir, "tagmanifest-sha256.txt", &tag)) {
        u8_join(path, sizeof(path), bagdir, "tagmanifest-sha256.txt");
        if (err) sb_printf(err, M_BAG_READ, path);
        bag_digests_free(&tag);
        return -1;
    }
    if (bag_manifest_load(bagdir, "manifest-sha256.txt", &man)) {
        if (err) sb_printf(err, M_ARC_NO_MANIFEST, bagdir);
        bag_digests_free(&tag);
        return -1;
    }
    /* bagit.txt creates the item with the fields */
    plan_add(out, bagdir, "bagit.txt", digest_of(&tag, "bagit.txt"));
    first = out->n;
    for (i = 0; i < man.n; i++) plan_add(out, bagdir, man.rel[i], man.hex[i]);
    if (out->n > first + 1)
        qsort(out->f + first, (size_t)(out->n - first), sizeof(ia_file), by_size);
    for (i = 0; tail[i]; i++) plan_add(out, bagdir, tail[i], digest_of(&tag, tail[i]));
    /* the tagmanifest is in no manifest: its digest is taken here, and it
     * goes last, so an item without it is a half upload to anyone */
    ia_tagmanifest_digest(bagdir, hex);
    plan_add(out, bagdir, "tagmanifest-sha256.txt", hex);

    for (i = 0; i < out->n; i++) {
        const ia_file *was = prev ? ia_files_find(&prev->files, out->f[i].name) : NULL;
        out->f[i].what = !was ? IA_F_NEW :
                         strcmp(was->sha256, out->f[i].sha256) ? IA_F_CHANGED : IA_F_SAME;
    }
    /* what the box no longer has is removed from the item */
    n = out->n;
    for (i = 0; prev && i < prev->files.n; i++) {
        ia_file f;
        int k;
        for (k = 0; k < n && strcmp(out->f[k].name, prev->files.f[i].name); k++) ;
        if (k < n) continue;
        f = prev->files.f[i];
        f.what = IA_F_REMOVED;
        ia_files_add(out, &f);
    }
    bag_digests_free(&tag);
    bag_digests_free(&man);
    return 0;
}

/* ---- the journal ---- */

static int journal_begin(const char *bagdir, const char *tagmanifest)
{
    char dir[1024], path[1200]; FILE *f;
    ia_upload_dir(bagdir, dir, sizeof(dir));
    if (u8_mkdirs(dir)) return -1;
    ia_journal_path(bagdir, path, sizeof(path));
    if (u8_exists(path)) return 0;                  /* a journal being taken up again */
    if (!(f = u8_fopen(path, "wb"))) return -1;
    fprintf(f, "tagmanifest %s\n", tagmanifest);
    fflush(f);
    fclose(f);
    return 0;
}

/* appended and flushed after every line: a list that grows, where a
 * half-written line is simply ignored when it is read back */
static void journal_file(const char *bagdir, const ia_file *f)
{
    char path[1200]; FILE *fp;
    ia_journal_path(bagdir, path, sizeof(path));
    if (!(fp = u8_fopen(path, "ab"))) return;
    fprintf(fp, "file %lld %s %s %s %s\n", f->size, f->sha256, f->md5, f->sha1, f->name);
    fflush(fp);
    fclose(fp);
}

int ia_journal_read(const char *bagdir, const char *tagmanifest, ia_files *out)
{
    char path[1200], line[1600]; FILE *f; int same = 0;
    memset(out, 0, sizeof(*out));
    ia_journal_path(bagdir, path, sizeof(path));
    if (!(f = u8_fopen(path, "rb"))) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *e = line + strlen(line);
        ia_file fl;
        long long size; char sha[80], md5[48], sha1[64], name[400];
        if (e == line || e[-1] != '\n') break;      /* a line left half written */
        *--e = 0;
        if (!strncmp(line, "tagmanifest ", 12)) {
            same = tagmanifest && !strcmp(line + 12, tagmanifest);
            continue;
        }
        if (sscanf(line, "file %lld %79s %47s %63s %399[^\n]", &size, sha, md5, sha1, name) != 5)
            continue;
        memset(&fl, 0, sizeof(fl));
        CP(fl.name, name);
        fl.size = size;
        CP(fl.sha256, sha);
        CP(fl.md5, md5);
        CP(fl.sha1, sha1);
        ia_files_add(out, &fl);
    }
    fclose(f);
    /* the package changed while the upload was stopped: the journal says
     * nothing about the files as they are now */
    if (!same) ia_files_free(out);
    return out->n;
}

static void journal_remove(const char *bagdir)
{
    char path[1200];
    ia_journal_path(bagdir, path, sizeof(path));
    u8_remove(path);
}

/* ---- talking to Archive.org ---- */

typedef struct {
    const ia_config *cfg;
    const ia_host *host;
    ia_result *res;
    https_client *c;
    tls_anchors *anchors;
    char auth[600];                     /* "Authorization: LOW access:secret" */
    const char *md_base, *s3_base;
    ia_progress pr;
    int deferred;                       /* fields too long for a header */
    char defer_name[4][32];
    int quiet;                          /* a look at the item while its tasks run: no phase, no log */
} ia_run;

static void ia_log(ia_run *r, const char *fmt, ...) RA_PRINTF(2, 3);

static void ia_log(ia_run *r, const char *fmt, ...)
{
    strbuf b; va_list ap;
    if (!r->host || !r->host->log) return;
    sb_init(&b);
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    if (b.s) r->host->log(r->host->ctx, b.s);
    sb_free(&b);
}

static void tell(ia_run *r, int phase)
{
    r->pr.phase = phase;
    if (r->host && r->host->progress) r->host->progress(r->host->ctx, &r->pr);
}

static int stopped(ia_run *r)
{
    return r->host && r->host->canceled && r->host->canceled(r->host->ctx);
}

/* a wait that Stop can cut short; 0 if it was cut short */
static int wait_s(ia_run *r, int seconds, int phase)
{
    int left;
    for (left = seconds; left > 0; left--) {
        int k;
        r->pr.seconds = left;
        tell(r, phase);
        for (k = 0; k < 4; k++) {
            if (stopped(r)) return 0;
            nap_ms(250);
        }
    }
    r->pr.seconds = 0;
    return 1;
}

/* the failure of https_do, as a result */
static int from_https(ia_run *r, int rc)
{
    ia_result *o = r->res;
    if (rc == HTTPS_OK) return IA_OK;
    if (rc == HTTPS_ERR_CANCELED) return o->why = IA_ERR_CANCELED;
    if (rc == HTTPS_ERR_TLS) { o->tls = *https_tls_info(r->c); return o->why = IA_ERR_TLS; }
    if (rc == HTTPS_ERR_FILE) return o->why = IA_ERR_FILE;
    if (rc == HTTPS_ERR_NET) { o->net_err = https_net_error(r->c); return o->why = IA_ERR_NET; }
    return o->why = IA_ERR_HTTP;
}

/* what the server's body says, for the log and the window; never a key */
static void detail_of(ia_result *o, const https_response *rs)
{
    const char *b = rs->body.s;
    char *p;
    if (!b || !b[0]) { o->detail[0] = 0; return; }
    str_copy(o->detail, sizeof(o->detail), b);
    for (p = o->detail; *p; p++) if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
}

static int says(const https_response *rs, const char *what)
{
    return rs->body.s && strstr(rs->body.s, what) != NULL;
}

/* a GET, with the keys; the status in res->status */
static int ia_get(ia_run *r, const char *url, https_response *rs)
{
    https_request rq;
    int rc;
    memset(&rq, 0, sizeof(rq));
    rq.method = "GET";
    rq.url = url;
    rq.headers = r->auth;
    rc = https_do(r->c, &rq, rs);
    if (rc == HTTPS_OK) r->res->status = rs->status;
    return from_https(r, rc);
}

/* ---- the identifier, on Archive.org ---- */

int ia_ident_check(const ia_config *cfg, const char *id, const char *mine, ia_result *out)
{
    ia_run r;
    https_response rs;
    char url[700];
    int rc;

    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    r.cfg = cfg; r.res = out;
    CP(out->identifier, id ? id : "");
    if (!ia_ident_ok(id)) return out->why = IA_ERR_IDENT;
    r.md_base = cfg->md_base && cfg->md_base[0] ? cfg->md_base : IA_MD_BASE;
    {
        https_url u;
        tls_config tc;
        snprintf(url, sizeof(url), "%s/services/check_identifier.php?output=json&identifier=%s",
                 r.md_base, id);
        if (!https_parse_url(url, &u) || !https_url_allowed(&u)) {
            out->net_err = NET_ERR_RESOLVE;
            return out->why = IA_ERR_NET;
        }
        if (u.tls && (!cfg->cacert || !cfg->cacert[0] ||
                      (r.anchors = tls_anchors_load(cfg->cacert, NULL)) == NULL))
            return out->why = IA_ERR_NO_CERTS;
        memset(&tc, 0, sizeof(tc));
        tc.anchors = r.anchors;
        tc.connect_ms = cfg->connect_ms;
        tc.io_ms = cfg->io_ms;
        tc.now = cfg->now;
        if ((r.c = https_new(&tc)) == NULL) {
            tls_anchors_free(r.anchors);
            return out->why = IA_ERR_MEM;
        }
    }
    snprintf(r.auth, sizeof(r.auth), "Authorization: LOW %s:%s\r\nAccept: application/json\r\n",
             cfg->access ? cfg->access : "", cfg->secret ? cfg->secret : "");
    https_response_init(&rs);
    rc = ia_get(&r, url, &rs);
    if (rc == IA_OK) {
        const char *code;
        jval *root = json_parse(rs.body.s, NULL, 0);
        code = root ? json_str(root, "code") : NULL;
        if (!code) rc = out->why = IA_ERR_HTTP;
        else if (!strcmp(code, "available")) out->ident = IA_ID_FREE;
        else out->ident = mine && !strcmp(mine, id) ? IA_ID_OURS : IA_ID_TAKEN;
        json_free(root);
    }
    memset(r.auth, 0, sizeof(r.auth));
    https_response_free(&rs);
    https_free(r.c);
    tls_anchors_free(r.anchors);
    return rc;
}

int ia_item_read(const ia_config *cfg, const char *id, ia_meta *live, ia_result *out)
{
    ia_run r;
    https_response rs;
    https_url u;
    tls_config tc;
    char url[700];
    int rc;

    memset(out, 0, sizeof(*out));
    memset(live, 0, sizeof(*live));
    memset(&r, 0, sizeof(r));
    r.cfg = cfg; r.res = out;
    CP(out->identifier, id ? id : "");
    if (!ia_ident_ok(id)) return out->why = IA_ERR_IDENT;
    r.md_base = cfg->md_base && cfg->md_base[0] ? cfg->md_base : IA_MD_BASE;
    snprintf(url, sizeof(url), "%s/metadata/%s", r.md_base, id);
    if (!https_parse_url(url, &u) || !https_url_allowed(&u)) {
        out->net_err = NET_ERR_RESOLVE;
        return out->why = IA_ERR_NET;
    }
    if (u.tls && (!cfg->cacert || !cfg->cacert[0] ||
                  (r.anchors = tls_anchors_load(cfg->cacert, NULL)) == NULL))
        return out->why = IA_ERR_NO_CERTS;
    memset(&tc, 0, sizeof(tc));
    tc.anchors = r.anchors;
    tc.connect_ms = cfg->connect_ms;
    tc.io_ms = cfg->io_ms;
    tc.now = cfg->now;
    if ((r.c = https_new(&tc)) == NULL) {
        tls_anchors_free(r.anchors);
        return out->why = IA_ERR_MEM;
    }
    snprintf(r.auth, sizeof(r.auth), "Authorization: LOW %s:%s\r\nAccept: application/json\r\n",
             cfg->access ? cfg->access : "", cfg->secret ? cfg->secret : "");
    https_response_init(&rs);
    rc = ia_get(&r, url, &rs);
    if (rc == IA_OK) {
        /* {} : there is no such item */
        if (rs.status == 200 && ia_meta_from_item(rs.body.s, live) == 0) out->ident = IA_ID_OURS;
        else if (rs.status == 200) out->ident = IA_ID_FREE;
        else { detail_of(out, &rs); rc = out->why = IA_ERR_HTTP; }
    }
    memset(r.auth, 0, sizeof(r.auth));
    https_response_free(&rs);
    https_free(r.c);
    tls_anchors_free(r.anchors);
    return rc;
}

int ia_ident_free(const ia_config *cfg, const char *base, char *out, size_t n,
                  ia_result *res)
{
    char id[IA_ID_MAX];
    int k;
    for (k = 1; k <= 20; k++) {
        if (k == 1) str_copy(id, sizeof(id), base);
        else snprintf(id, sizeof(id), "%s-%d", base, k);
        if (ia_ident_check(cfg, id, NULL, res) != IA_OK) return res->why;
        if (res->ident == IA_ID_FREE || res->ident == IA_ID_OURS) {
            str_copy(out, n, id);
            return IA_OK;
        }
    }
    return res->why = IA_ERR_TAKEN;
}

/* ---- hashing a file ---- */

typedef struct { ia_run *r; long long done; } put_ctx;

static int hash_one(ia_run *r, const char *bagdir, ia_file *f)
{
    char path[1400]; FILE *fp;
    sha256_ctx s2; br_md5_context m5; br_sha1_context s1;
    unsigned char buf[HASH_PIECE], d[32];
    size_t n;
    char hex[65];
    int i;

    u8_join(path, sizeof(path), bagdir, f->name);
    if (!(fp = u8_fopen(path, "rb"))) {
        CP(r->res->file, f->name);
        return r->res->why = IA_ERR_FILE;
    }
    sha256_init(&s2);
    br_md5_init(&m5);
    br_sha1_init(&s1);
    r->pr.done = 0;
    r->pr.size = f->size;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        sha256_update(&s2, buf, n);
        br_md5_update(&m5, buf, n);
        br_sha1_update(&s1, buf, n);
        r->pr.done += (long long)n;
        tell(r, IA_PH_HASH);
        if (stopped(r)) { fclose(fp); return r->res->why = IA_ERR_CANCELED; }
    }
    fclose(fp);
    sha256_final(&s2, d);
    sha256_hex(d, hex);
    br_md5_out(&m5, d);
    for (i = 0; i < 16; i++) sprintf(f->md5 + 2 * i, "%02x", d[i]);
    br_sha1_out(&s1, d);
    for (i = 0; i < 20; i++) sprintf(f->sha1 + 2 * i, "%02x", d[i]);
    /* the package guarantees this file: if it does not, it is not sent */
    if (f->sha256[0] && strcmp(hex, f->sha256)) {
        CP(r->res->file, f->name);
        return r->res->why = IA_ERR_CHANGED;
    }
    CP(f->sha256, hex);
    return IA_OK;
}

/* the MD5 in base64, for Content-MD5 */
static void md5_b64(const char *hex, char *out, size_t n)
{
    unsigned char raw[16];
    int i;
    for (i = 0; i < 16; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        raw[i] = (unsigned char)v;
    }
    b64_encode(raw, 16, out, n);
}

/* ---- the fields as headers ---- */

/* a header value: no newlines, and uri(...) when it is not ASCII */
static void header_value(strbuf *out, const char *v)
{
    const unsigned char *p = (const unsigned char *)v;
    int high = 0;
    for (; *p; p++) if (*p >= 0x80) high = 1;
    if (!high) {
        char *q;
        size_t at = out->len;
        sb_puts(out, v);
        for (q = out->s + at; *q; q++) if (*q == '\r' || *q == '\n' || *q == '\t') *q = ' ';
        return;
    }
    sb_puts(out, "uri(");
    https_pct(out, v, "");
    sb_puts(out, ")");
}

static void meta_header(ia_run *r, strbuf *h, const char *name, const char *value, int idx)
{
    strbuf v;
    char hname[80], *q;
    if (!value || !value[0]) return;
    str_copy(hname, sizeof(hname), name);
    /* an underscore in the name of a field is written "--" */
    for (q = hname; *q; q++)
        if (*q == '_' && strlen(hname) + 2 < sizeof(hname)) {
            memmove(q + 1, q, strlen(q) + 1);
            *q++ = '-';
            *q = '-';
        }
    sb_init(&v);
    header_value(&v, value);
    if (v.len > IA_HEADER_MAX) {
        /* too long for a header: it goes with a metadata write afterwards */
        if (r->deferred < 4) str_copy(r->defer_name[r->deferred++], 32, name);
    } else if (idx < 0) {
        sb_printf(h, "x-archive-meta-%s: %s\r\n", hname, v.s ? v.s : "");
    } else {
        sb_printf(h, "x-archive-meta%02d-%s: %s\r\n", idx, hname, v.s ? v.s : "");
    }
    sb_free(&v);
}

/* Archive.org shows the description as HTML: a plain line break would be
 * lost (seen on the first real upload, all on one line), so it becomes
 * <br>, and & < > of the user's notes become entities */
static void html_lines(strbuf *o, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '\n': sb_puts(o, "<br>"); break;
        case '\r': break;
        case '&':  sb_puts(o, "&amp;"); break;
        case '<':  sb_puts(o, "&lt;"); break;
        case '>':  sb_puts(o, "&gt;"); break;
        default:   sb_add(o, s, 1); break;
        }
    }
}

static void meta_headers(ia_run *r, const ia_meta *m, long long total, strbuf *h)
{
    int i;
    strbuf d;
    sb_printf(h, "x-archive-auto-make-bucket: 1\r\n");
    sb_printf(h, "x-archive-size-hint: %lld\r\n", total);
    meta_header(r, h, "mediatype", m->mediatype, -1);
    meta_header(r, h, "collection", m->collection, -1);
    meta_header(r, h, "title", m->title, -1);
    meta_header(r, h, "creator", m->creator, -1);
    meta_header(r, h, "publisher", m->publisher, -1);
    meta_header(r, h, "date", m->date, -1);
    meta_header(r, h, "platform", m->platform, -1);
    meta_header(r, h, "edition", m->edition, -1);
    meta_header(r, h, "media", m->media, -1);
    meta_header(r, h, "rights", m->rights, -1);
    meta_header(r, h, "scanner", m->scanner, -1);
    meta_header(r, h, "bagit-profile", m->profile, -1);
    sb_init(&d);
    html_lines(&d, m->description);
    meta_header(r, h, "description", d.s ? d.s : "", -1);
    sb_free(&d);
    for (i = 0; i < m->nlang; i++)
        meta_header(r, h, "language", m->language[i], m->nlang > 1 ? i : -1);
    for (i = 0; i < m->nsubj; i++)
        meta_header(r, h, "subject", m->subject[i], m->nsubj > 1 ? i : -1);
}

/* back from what html_lines wrote, and from the <br /> Archive.org may
 * write itself, to the plain text the box and the window have */
static void html_plain(const char *s, char *out, size_t n)
{
    static const struct { const char *from; char to; } ent[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' },
        { "<br>", '\n' }, { "<br/>", '\n' }, { "<br />", '\n' }, { "<BR>", '\n' }, { NULL, 0 }
    };
    size_t k = 0;
    while (*s && k + 1 < n) {
        int i;
        for (i = 0; ent[i].from && strncmp(s, ent[i].from, strlen(ent[i].from)); i++) ;
        if (ent[i].from) { out[k++] = ent[i].to; s += strlen(ent[i].from); }
        else out[k++] = *s++;
    }
    out[k] = 0;
}

/* a single-valued field: a list (two creators, added on the site) is read
 * as its values joined, so that it is shown, and kept, as it is */
static void item_text(const jval *m, const char *key, char *out, size_t n)
{
    const jval *v = json_get(m, key), *e;
    out[0] = 0;
    if (!v) return;
    if (v->t == J_STR) { str_copy(out, n, v->s); return; }
    if (v->t != J_ARR) return;
    for (e = v->child; e; e = e->next) {
        if (e->t != J_STR) continue;
        if (out[0] && strlen(out) + 2 < n) strcat(out, "; ");
        if (strlen(out) + strlen(e->s) < n) strcat(out, e->s);
    }
}

static int item_list(const jval *m, const char *key, char *dst, size_t stride, int max)
{
    const jval *v = json_get(m, key), *e;
    int k = 0;
    if (v && v->t == J_STR) { str_copy(dst, stride, v->s); return 1; }
    for (e = v && v->t == J_ARR ? v->child : NULL; e && k < max; e = e->next)
        if (e->t == J_STR) str_copy(dst + (size_t)k++ * stride, stride, e->s);
    return k;
}

int ia_meta_from_item(const char *json, ia_meta *out)
{
    jval *root = json ? json_parse(json, NULL, 0) : NULL;
    const jval *m = root ? json_get(root, "metadata") : NULL;
    char *desc;
    memset(out, 0, sizeof(*out));
    if (!m || m->t != J_OBJ) { json_free(root); return -1; }
    item_text(m, "mediatype", out->mediatype, sizeof(out->mediatype));
    /* an item in several collections: the first is the one it was made in */
    if (item_list(m, "collection", out->collection, sizeof(out->collection), 1) < 1)
        out->collection[0] = 0;
    item_text(m, "title", out->title, sizeof(out->title));
    item_text(m, "creator", out->creator, sizeof(out->creator));
    item_text(m, "publisher", out->publisher, sizeof(out->publisher));
    item_text(m, "date", out->date, sizeof(out->date));
    item_text(m, "platform", out->platform, sizeof(out->platform));
    item_text(m, "edition", out->edition, sizeof(out->edition));
    item_text(m, "media", out->media, sizeof(out->media));
    item_text(m, "rights", out->rights, sizeof(out->rights));
    item_text(m, "scanner", out->scanner, sizeof(out->scanner));
    item_text(m, "bagit-profile", out->profile, sizeof(out->profile));
    if ((desc = (char *)malloc(IA_DESC_MAX)) != NULL) {
        item_text(m, "description", desc, IA_DESC_MAX);
        html_plain(desc, out->description, sizeof(out->description));
        free(desc);
    }
    out->nlang = item_list(m, "language", out->language[0], sizeof(out->language[0]), IA_MAX_LANG);
    out->nsubj = item_list(m, "subject", out->subject[0], sizeof(out->subject[0]), IA_MAX_SUBJECT);
    json_free(root);
    return 0;
}

/* the value of one field, for the deferred metadata write */
static const char *meta_field(const ia_meta *m, const char *name)
{
    if (!strcmp(name, "description")) return m->description;
    if (!strcmp(name, "title")) return m->title;
    if (!strcmp(name, "rights")) return m->rights;
    if (!strcmp(name, "media")) return m->media;
    return "";
}

/* the fields a header could not carry, with a JSON Patch on the metadata */
static int post_patch(ia_run *r, const char *id, const char *patch, int fields);

static int write_long_fields(ia_run *r, const char *id)
{
    strbuf patch;
    int i, rc;
    if (!r->deferred) return IA_OK;
    tell(r, IA_PH_META);
    sb_init(&patch);
    sb_puts(&patch, "[");
    for (i = 0; i < r->deferred; i++) {
        json_writer v;
        /* the paths of a patch have no prefix: "/description", not
         * "/metadata/description" */
        sb_printf(&patch, "%s{\"op\":\"replace\",\"path\":\"/%s\",\"value\":",
                  i ? "," : "", r->defer_name[i]);
        jw_init(&v);
        if (!strcmp(r->defer_name[i], "description")) {
            strbuf d;
            sb_init(&d);
            html_lines(&d, r->cfg->meta->description);
            jw_str(&v, NULL, d.s ? d.s : "");
            sb_free(&d);
        } else {
            jw_str(&v, NULL, meta_field(r->cfg->meta, r->defer_name[i]));
        }
        if (v.out.s) sb_puts(&patch, v.out.s);
        jw_free(&v);
        sb_puts(&patch, "}");
    }
    sb_puts(&patch, "]");
    rc = post_patch(r, id, patch.s ? patch.s : "[]", r->deferred);
    sb_free(&patch);
    return rc;
}

/* one JSON Patch on the item's metadata: a patch that changes nothing is
 * refused by Archive.org, and for us the fields were already right */
static int post_patch(ia_run *r, const char *id, const char *patch, int fields)
{
    https_request rq; https_response rs; strbuf body, head; char url[700];
    int rc;
    sb_init(&body);
    https_form(&body, "-target", "metadata");
    https_form(&body, "-patch", patch);
    snprintf(url, sizeof(url), "%s/metadata/%s", r->md_base, id);
    /* without its type Archive.org does not read the form: a 400 with an
     * HTML page (seen on the first real update) */
    sb_init(&head);
    sb_puts(&head, r->auth);
    sb_puts(&head, "Content-Type: application/x-www-form-urlencoded\r\n");
    memset(&rq, 0, sizeof(rq));
    rq.method = "POST";
    rq.url = url;
    rq.headers = head.s;
    rq.body = body.s;
    rq.body_len = body.len;
    https_response_init(&rs);
    rc = https_do(r->c, &rq, &rs);
    if (rc == HTTPS_OK) {
        r->res->status = rs.status;
        ia_log(r, "POST /metadata/%s %d fields -> %d", id, fields, rs.status);
        if (rs.status != 200 && !says(&rs, "No changes")) {
            detail_of(r->res, &rs);
            rc = r->res->why = IA_ERR_HTTP;
        } else {
            rc = IA_OK;
        }
    } else {
        rc = from_https(r, rc);
    }
    https_response_free(&rs);
    sb_free(&body);
    memset(head.s, 0, head.len);         /* it carries the keys */
    sb_free(&head);
    return rc;
}

/* one operation of an update's patch: add a field the item has not,
 * remove one the box has emptied, replace one that changed */
static void patch_op(strbuf *patch, const ia_meta *live, const ia_meta *want, int i)
{
    static const ia_meta empty;
    json_writer v;
    const char *op = !ia_field_differs(live, &empty, i) ? "add" :
                     !ia_field_differs(want, &empty, i) ? "remove" : "replace";
    sb_printf(patch, "%s{\"op\":\"%s\",\"path\":\"/%s\"", patch->len > 1 ? "," : "", op,
              ia_field_name(i));
    if (strcmp(op, "remove")) {
        int k;
        jw_init(&v);
        if (i == IA_FD_LANG || i == IA_FD_SUBJ) {
            jw_begin_arr(&v, NULL);
            for (k = 0; k < (i == IA_FD_LANG ? want->nlang : want->nsubj); k++)
                jw_str(&v, NULL, i == IA_FD_LANG ? want->language[k] : want->subject[k]);
            jw_end_arr(&v);
        } else if (i == IA_FD_DESC) {
            strbuf d;
            sb_init(&d);
            html_lines(&d, want->description);
            jw_str(&v, NULL, d.s ? d.s : "");
            sb_free(&d);
        } else {
            jw_str(&v, NULL, ia_field_text(want, i));
        }
        sb_puts(patch, ",\"value\":");
        if (v.out.s) sb_puts(patch, v.out.s);
        jw_free(&v);
    }
    sb_puts(patch, "}");
}

/* 1 if the item is on Archive.org, 0 if not ({}), -1 and res->why if the
 * question failed */
static int item_there(ia_run *r, const char *id)
{
    https_response rs; char url[700]; ia_meta *m; int rc, there = -1;
    snprintf(url, sizeof(url), "%s/metadata/%s", r->md_base, id);
    if ((m = (ia_meta *)malloc(sizeof(*m))) == NULL) { r->res->why = IA_ERR_MEM; return -1; }
    https_response_init(&rs);
    rc = ia_get(r, url, &rs);
    if (rc == IA_OK && rs.status == 200) there = ia_meta_from_item(rs.body.s, m) == 0;
    else if (rc == IA_OK) { detail_of(r->res, &rs); r->res->why = IA_ERR_HTTP; }
    https_response_free(&rs);
    free(m);
    return there;
}

/* an update: the fields as the item has them now against those confirmed
 * in the window; only the ones that differ are written */
static int update_fields(ia_run *r, const char *id)
{
    https_response rs; char url[700]; ia_meta *live; strbuf patch;
    unsigned bits;
    int rc, i;
    tell(r, IA_PH_META);
    if ((live = (ia_meta *)malloc(sizeof(*live))) == NULL) return r->res->why = IA_ERR_MEM;
    snprintf(url, sizeof(url), "%s/metadata/%s", r->md_base, id);
    https_response_init(&rs);
    rc = ia_get(r, url, &rs);
    if (rc == IA_OK && (rs.status != 200 || ia_meta_from_item(rs.body.s, live))) {
        detail_of(r->res, &rs);
        rc = r->res->why = IA_ERR_HTTP;
    }
    https_response_free(&rs);
    if (rc != IA_OK) { free(live); return rc; }
    bits = ia_meta_diff(live, r->cfg->meta);
    sb_init(&patch);
    sb_puts(&patch, "[");
    for (i = 0; i < IA_NFIELDS; i++)
        if (bits & (1u << i)) {
            patch_op(&patch, live, r->cfg->meta, i);
            r->res->fields_changed++;
        }
    sb_puts(&patch, "]");
    rc = r->res->fields_changed ? post_patch(r, id, patch.s, r->res->fields_changed) : IA_OK;
    if (!r->res->fields_changed) ia_log(r, "The fields of %s are already right", id);
    sb_free(&patch);
    free(live);
    return rc;
}

/* ---- sending one file ---- */

static int put_progress(void *ctx, long long off, const void *data, size_t n)
{
    put_ctx *p = (put_ctx *)ctx;
    (void)data;
    if (off == 0) p->done = 0;
    p->done = off + (long long)n;
    p->r->pr.done = p->done;
    tell(p->r, IA_PH_PUT);
    return stopped(p->r);
}

/* 403: the keys, or an item that is somebody else's */
static int why_403(ia_run *r, const char *id)
{
    https_response rs; char url[700]; int rc, empty = 1;
    snprintf(url, sizeof(url), "%s/metadata/%s", r->md_base, id);
    https_response_init(&rs);
    rc = ia_get(r, url, &rs);
    if (rc == IA_OK && rs.status == 200 && rs.body.s && strstr(rs.body.s, "\"files\"")) empty = 0;
    https_response_free(&rs);
    return r->res->why = empty ? IA_ERR_KEYS : IA_ERR_TAKEN;
}

static int put_file(ia_run *r, const char *id, const ia_file *f, const strbuf *extra)
{
    char url[1400], md5[32];
    strbuf path, head;
    https_request rq;
    https_response rs;
    put_ctx pc;
    int rc, tries, tries_max = r->cfg->put_tries > 0 ? r->cfg->put_tries : IA_PUT_TRIES;
    char local[1400];
    static const int backoff[] = { 10, 30, 60 };
    static const int slow[] = { 30, 60, 120, 300, 600 };
    int slows = 0;

    sb_init(&path);
    https_pct(&path, f->name, "/");
    snprintf(url, sizeof(url), "%s/%s/%s", r->s3_base, id, path.s ? path.s : "");
    sb_free(&path);
    u8_join(local, sizeof(local), r->cfg->bagdir, f->name);
    md5_b64(f->md5, md5, sizeof(md5));

    sb_init(&head);
    sb_puts(&head, r->auth);
    sb_printf(&head, "Content-MD5: %s\r\n", md5);
    sb_puts(&head, "x-archive-queue-derive: 0\r\n");
    if (extra && extra->len) sb_puts(&head, extra->s);

    https_response_init(&rs);
    pc.r = r; pc.done = 0;
    r->pr.size = f->size;
    r->pr.done = 0;
    for (tries = 0;; ) {
        memset(&rq, 0, sizeof(rq));
        rq.method = "PUT";
        rq.url = url;
        rq.headers = head.s;
        rq.body_file = local;
        rq.file_len = -1;
        rq.on_body = put_progress;
        rq.ctx = &pc;
        rq.expect_continue = 1;
        rc = https_do(r->c, &rq, &rs);
        if (rc == HTTPS_OK) {
            r->res->status = rs.status;
            ia_log(r, "PUT %s %lld bytes -> %d", f->name, f->size, rs.status);
            if (rs.status == 200 || rs.status == 201) { rc = IA_OK; break; }
            if (rs.status == 503 && says(&rs, "appears to be spam")) {
                detail_of(r->res, &rs);
                rc = r->res->why = IA_ERR_SPAM;
                break;
            }
            if (rs.status == 503 && slows < (int)(sizeof(slow) / sizeof(slow[0]))) {
                ia_log(r, "Archive.org is busy: waiting %d s", slow[slows]);
                if (!wait_s(r, slow[slows++], IA_PH_RETRY)) { rc = r->res->why = IA_ERR_CANCELED; break; }
                continue;
            }
            if (rs.status == 403) { rc = why_403(r, id); detail_of(r->res, &rs); break; }
            detail_of(r->res, &rs);
            CP(r->res->file, f->name);
            rc = r->res->why = IA_ERR_HTTP;
            break;
        }
        rc = from_https(r, rc);
        if (rc == IA_ERR_CANCELED || rc == IA_ERR_FILE || rc == IA_ERR_TLS ||
            rc == IA_ERR_NO_CERTS || tries >= tries_max) {
            CP(r->res->file, f->name);
            break;
        }
        ia_log(r, "PUT %s failed: trying again in %d s", f->name, backoff[tries < 3 ? tries : 2]);
        if (!wait_s(r, backoff[tries < 3 ? tries : 2], IA_PH_RETRY)) {
            rc = r->res->why = IA_ERR_CANCELED;
            break;
        }
        tries++;
        r->res->why = IA_OK;
    }
    https_response_free(&rs);
    sb_free(&head);
    return rc;
}

/* a file the box no longer has: out of the item, with the old version kept
 * in its history (nothing is ever thrown away, as in set-aside/). Already
 * gone (404) is what we wanted */
static int delete_file(ia_run *r, const char *id, const ia_file *f)
{
    char url[1400];
    strbuf path, head;
    https_request rq;
    https_response rs;
    int rc;
    sb_init(&path);
    https_pct(&path, f->name, "/");
    snprintf(url, sizeof(url), "%s/%s/%s", r->s3_base, id, path.s ? path.s : "");
    sb_free(&path);
    sb_init(&head);
    sb_puts(&head, r->auth);
    sb_puts(&head, "x-archive-keep-old-version: 1\r\nx-archive-cascade-delete: 1\r\n");
    memset(&rq, 0, sizeof(rq));
    rq.method = "DELETE";
    rq.url = url;
    rq.headers = head.s;
    https_response_init(&rs);
    rc = https_do(r->c, &rq, &rs);
    if (rc == HTTPS_OK) {
        r->res->status = rs.status;
        ia_log(r, "DELETE %s -> %d", f->name, rs.status);
        if (rs.status == 200 || rs.status == 204 || rs.status == 404) rc = IA_OK;
        else if (rs.status == 403) { rc = why_403(r, id); detail_of(r->res, &rs); }
        else { detail_of(r->res, &rs); CP(r->res->file, f->name); rc = r->res->why = IA_ERR_HTTP; }
    } else {
        rc = from_https(r, rc);
        CP(r->res->file, f->name);
    }
    https_response_free(&rs);
    sb_free(&head);
    return rc;
}

/* ---- waiting for the tasks, and the verification ---- */

/* 1 when the item has no task queued or running; *bad gets those in error */
static int tasks_done(ia_run *r, const char *id, int *bad, int *rc)
{
    https_response rs; char url[700]; int done = 0;
    snprintf(url, sizeof(url), "%s/services/tasks.php?identifier=%s&summary=1", r->md_base, id);
    https_response_init(&rs);
    *rc = ia_get(r, url, &rs);
    if (*rc == IA_OK && rs.status == 200) {
        jval *root = json_parse(rs.body.s, NULL, 0);
        const jval *v = root ? json_get(root, "value") : NULL;
        const jval *s = v ? json_get(v, "summary") : NULL;
        if (!s) s = root ? json_get(root, "summary") : NULL;
        if (s) {
            done = json_int(s, "queued", 0) == 0 && json_int(s, "running", 0) == 0;
            *bad = (int)json_int(s, "error", 0);
        } else {
            done = 1;                   /* an answer without a summary: nothing queued */
        }
        json_free(root);
    } else if (*rc == IA_OK) {
        done = 1;                       /* no tasks for this item */
    }
    https_response_free(&rs);
    return done;
}

typedef struct { const jval *files, *meta; } item_view;

static const jval *item_file(const item_view *it, const char *name)
{
    const jval *e;
    for (e = it->files && it->files->t == J_ARR ? it->files->child : NULL; e; e = e->next) {
        const char *n = json_str(e, "name");
        if (n && (!strcmp(n, name) || (n[0] == '/' && !strcmp(n + 1, name)))) return e;
    }
    return NULL;
}

/* Archive.org writes the sizes as strings; a number is read too */
static long long item_size(const jval *f)
{
    const jval *v = json_get(f, "size");
    if (!v) return -1;
    if (v->t == J_STR) return atoll(v->s);
    if (v->t == J_NUM) return (long long)v->n;
    return -1;
}

static int meta_matches(const jval *m, const char *key, const char *want)
{
    const jval *v = json_get(m, key);
    if (!want[0]) return 1;
    if (!v) return 0;
    if (v->t == J_STR) return !strcmp(v->s, want);
    if (v->t == J_ARR) {
        const jval *e;
        for (e = v->child; e; e = e->next) if (e->t == J_STR && !strcmp(e->s, want)) return 1;
    }
    return 0;
}

static void verify_fail(ia_run *r, const char *name, const char *why)
{
    r->res->mismatch++;
    if (r->quiet) return;
    if (!r->res->file[0]) { CP(r->res->file, name); CP(r->res->detail, why); }
    ia_log(r, "verify: %s: %s", name, why);
}

/* everything in the item, as Archive.org reports it, against what went up */
static int verify(ia_run *r, const char *id, const ia_files *plan)
{
    https_response rs; char url[700]; int rc, i;
    jval *root = NULL; item_view it;
    const jval *e;

    if (!r->quiet) tell(r, IA_PH_VERIFY);
    snprintf(url, sizeof(url), "%s/metadata/%s", r->md_base, id);
    https_response_init(&rs);
    rc = ia_get(r, url, &rs);
    if (rc != IA_OK) { https_response_free(&rs); return rc; }
    if (rs.status != 200 || (root = json_parse(rs.body.s, NULL, 0)) == NULL) {
        https_response_free(&rs);
        json_free(root);
        return r->res->why = IA_ERR_HTTP;
    }
    it.files = json_get(root, "files");
    it.meta = json_get(root, "metadata");
    r->res->file[0] = 0;
    r->res->mismatch = 0;
    for (i = 0; i < plan->n; i++) {
        const ia_file *f = &plan->f[i];
        const jval *got;
        const char *s;
        if (f->what == IA_F_REMOVED) continue;
        if ((got = item_file(&it, f->name)) == NULL) { verify_fail(r, f->name, "missing in the item"); continue; }
        if (item_size(got) >= 0 && item_size(got) != f->size)
            verify_fail(r, f->name, "a different size");
        if ((s = json_str(got, "md5")) != NULL && strcmp(s, f->md5))
            verify_fail(r, f->name, "a different MD5");
        if ((s = json_str(got, "sha1")) != NULL && f->sha1[0] && strcmp(s, f->sha1))
            verify_fail(r, f->name, "a different SHA-1");
    }
    /* files under data/ that the package does not have: an unfinished
     * update, or somebody else uploading into the item */
    for (e = it.files && it.files->t == J_ARR ? it.files->child : NULL; e; e = e->next) {
        const char *n = json_str(e, "name");
        if (!n) continue;
        if (*n == '/') n++;
        if (strncmp(n, "data/", 5)) continue;
        if (!ia_files_find(plan, n)) verify_fail(r, n, "in the item but not in the package");
    }
    if (it.meta) {
        const ia_meta *m = r->cfg->meta;
        int k;
        if (!meta_matches(it.meta, "title", m->title)) verify_fail(r, "title", "a different value");
        if (!meta_matches(it.meta, "mediatype", m->mediatype)) verify_fail(r, "mediatype", "a different value");
        if (!meta_matches(it.meta, "creator", m->creator)) verify_fail(r, "creator", "a different value");
        if (!meta_matches(it.meta, "date", m->date)) verify_fail(r, "date", "a different value");
        for (k = 0; k < m->nsubj; k++)
            if (!meta_matches(it.meta, "subject", m->subject[k])) verify_fail(r, "subject", "missing");
        for (k = 0; k < m->nlang; k++)
            if (!meta_matches(it.meta, "language", m->language[k])) verify_fail(r, "language", "missing");
    }
    json_free(root);
    https_response_free(&rs);
    return r->res->mismatch ? (r->res->why = IA_ERR_VERIFY) : IA_OK;
}

/* The item is already what went up, although Archive.org still has tasks
 * for it? The derive (torrent, previews) can stay queued for hours after the
 * files are in place, and the item is on the site long before. Nothing is
 * reported: an item that is not there yet is just not ready */
static int item_ready(ia_run *r, const char *id, const ia_files *plan)
{
    int rc, why = r->res->why;
    r->quiet = 1;
    rc = verify(r, id, plan);
    r->quiet = 0;
    r->res->mismatch = 0;
    r->res->why = why;
    return rc == IA_OK;
}

/* ---- the whole upload ---- */

static void record_fill(ia_record *rec, const ia_config *cfg, const ia_files *plan,
                        const char *tagmanifest, int state)
{
    char now[20];
    int i;
    meta_now(now);
    CP(rec->identifier, cfg->identifier);
    snprintf(rec->url, sizeof(rec->url), "%s%s", IA_DETAILS_URL, cfg->identifier);
    CP(rec->collection, cfg->meta->collection);
    if (!rec->created[0]) CP(rec->created, now);
    else CP(rec->updated, now);
    rec->state = state;
    if (state == IA_ST_VERIFIED) CP(rec->verified, now);
    CP(rec->tagmanifest, tagmanifest);
    rec->meta = *cfg->meta;
    ia_files_free(&rec->files);
    for (i = 0; i < plan->n; i++)
        if (plan->f[i].what != IA_F_REMOVED && plan->f[i].at[0]) ia_files_add(&rec->files, &plan->f[i]);
}

static void record_event(ia_record *rec, const char *action, const char *extra)
{
    char now[20], entry[400];
    meta_now(now);
    snprintf(entry, sizeof(entry), "{\"at\": \"%s\", \"action\": \"%s\"%s}", now, action,
             extra ? extra : "");
    ia_record_event(rec, entry);
}

int ia_upload(const ia_config *cfg, const ia_host *host, ia_result *out)
{
    ia_run r;
    ia_files plan, done;
    ia_record rec;
    tls_config tc;
    https_url u;
    strbuf err, extra;
    char tagmanifest[65], url0[700];
    int rc = IA_OK, i = 0, k, state = IA_ST_INTERRUPTED, box, upd, finished = 0;
    long long total = 0;

    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    memset(&plan, 0, sizeof(plan));
    memset(&done, 0, sizeof(done));
    ia_record_init(&rec);
    r.cfg = cfg; r.host = host; r.res = out;
    CP(out->identifier, cfg->identifier ? cfg->identifier : "");
    snprintf(out->url, sizeof(out->url), "%s%s", IA_DETAILS_URL, out->identifier);
    r.md_base = cfg->md_base && cfg->md_base[0] ? cfg->md_base : IA_MD_BASE;
    r.s3_base = cfg->s3_base && cfg->s3_base[0] ? cfg->s3_base : IA_S3_BASE;

    if ((box = ia_box_can_upload(cfg->bagdir)) != IA_BOX_OK) {
        out->box_why = box;
        return out->why = IA_ERR_BOX;
    }
    if (!ia_ident_ok(cfg->identifier)) return out->why = IA_ERR_IDENT;
    if (!cfg->meta) return out->why = IA_ERR_MEM;

    /* the item was made by an upload of this box: this one is an update */
    ia_record_load(cfg->bagdir, &rec);
    upd = ia_record_made(&rec) && !strcmp(rec.identifier, cfg->identifier);
    out->update = upd;
    sb_init(&err);
    if (ia_plan(cfg->bagdir, upd ? &rec : NULL, &plan, &err)) {
        CP(out->detail, err.s ? err.s : "");
        sb_free(&err);
        ia_record_free(&rec);
        return out->why = IA_ERR_BOX;
    }
    sb_free(&err);
    ia_tagmanifest_digest(cfg->bagdir, tagmanifest);
    /* what a stopped upload already sent: taken up again */
    ia_journal_read(cfg->bagdir, tagmanifest, &done);

    snprintf(url0, sizeof(url0), "%s/", r.s3_base);
    if (!https_parse_url(url0, &u) || !https_url_allowed(&u)) {
        out->net_err = NET_ERR_RESOLVE;
        rc = out->why = IA_ERR_NET;
        goto out;
    }
    if (u.tls && (!cfg->cacert || !cfg->cacert[0] ||
                  (r.anchors = tls_anchors_load(cfg->cacert, NULL)) == NULL)) {
        rc = out->why = IA_ERR_NO_CERTS;
        goto out;
    }
    memset(&tc, 0, sizeof(tc));
    tc.anchors = r.anchors;
    tc.connect_ms = cfg->connect_ms;
    tc.io_ms = cfg->io_ms;
    tc.now = cfg->now;
    if ((r.c = https_new(&tc)) == NULL) { rc = out->why = IA_ERR_MEM; goto out; }
    snprintf(r.auth, sizeof(r.auth), "Authorization: LOW %s:%s\r\n",
             cfg->access ? cfg->access : "", cfg->secret ? cfg->secret : "");

    if (cfg->verify_only) {
        int bad = 0, grc = IA_OK, ok;
        ia_config vc = *cfg;
        if (!upd) { out->box_why = IA_BOX_OK; rc = out->why = IA_ERR_BOX; goto out; }
        ia_files_free(&plan);
        for (i = 0; i < rec.files.n; i++) {
            ia_file f = rec.files.f[i];
            f.what = IA_F_SAME;
            ia_files_add(&plan, &f);
        }
        vc.meta = &rec.meta;
        r.cfg = &vc;
        ia_log(&r, "Verifying %s against the upload of %s", out->identifier,
               rec.updated[0] ? rec.updated : rec.created);
        ok = tasks_done(&r, cfg->identifier, &bad, &grc);
        if (grc != IA_OK) { rc = grc; goto out; }
        if (bad > 0) {
            verify_fail(&r, cfg->identifier, "a task of Archive.org ended in error");
            rc = out->why = IA_ERR_VERIFY;
        } else {
            rc = verify(&r, cfg->identifier, &plan);
        }
        r.cfg = cfg;
        if (rc != IA_OK && rc != IA_ERR_VERIFY) goto out;      /* the network: nothing is known */
        if (rc == IA_ERR_VERIFY && !ok && !bad) {               /* still being archived */
            ia_log(&r, "Archive.org has not finished: %s", out->detail);
            rc = out->why = IA_ERR_PENDING;
            goto out;
        }
        rec.state = rc == IA_OK ? IA_ST_VERIFIED : IA_ST_FAILED;
        if (rc == IA_OK) meta_now(rec.verified);
        record_event(&rec, "verify", rc == IA_OK ? ", \"result\": \"ok\"" : ", \"result\": \"failed\"");
        sb_init(&err);
        if (ia_record_save(cfg->bagdir, &rec, &err) && rc == IA_OK) {
            CP(out->detail, err.s ? err.s : "");
            rc = out->why = IA_ERR_RECORD;
        }
        sb_free(&err);
        ia_log(&r, "Verification finished: state %s", ia_state_key(rec.state));
        goto out;
    }

    /* the item is gone (test_collection forgets its items after 30 days):
     * the box goes up again from the start, as the first time */
    if (upd) {
        int there = item_there(&r, cfg->identifier);
        if (there < 0) { rc = out->why; goto out; }
        if (!there) {
            ia_log(&r, "%s is no longer on Archive.org: uploading it again", out->identifier);
            upd = out->update = 0;
            ia_files_free(&plan);
            ia_files_free(&done);
            ia_journal_path(cfg->bagdir, url0, sizeof(url0));
            u8_remove(url0);
            ia_record_free(&rec);
            ia_record_init(&rec);
            sb_init(&err);
            if (ia_plan(cfg->bagdir, NULL, &plan, &err)) {
                CP(out->detail, err.s ? err.s : "");
                sb_free(&err);
                rc = out->why = IA_ERR_BOX;
                goto out;
            }
            sb_free(&err);
        }
    }
    for (i = 0; i < plan.n; i++)
        if (!upd || plan.f[i].what == IA_F_NEW || plan.f[i].what == IA_F_CHANGED) total += plan.f[i].size;

    r.pr.files = plan.n;
    r.pr.total = total;
    tell(&r, IA_PH_PREPARE);
    if (upd)
        ia_log(&r, "Updating %s: %lld bytes of new and changed files", out->identifier, total);
    else
        ia_log(&r, "Uploading %d files (%lld bytes) to %s", plan.n, total, out->identifier);
    if (journal_begin(cfg->bagdir, tagmanifest)) {
        rc = out->why = IA_ERR_RECORD;
        goto out;
    }

    sb_init(&extra);
    for (i = 0; i < plan.n; i++) {
        ia_file *f = &plan.f[i];
        const ia_file *was = ia_files_find(&done, f->name);
        r.pr.file = i + 1;
        r.pr.name = f->name;
        if (f->what == IA_F_REMOVED) continue;      /* deleted after the uploads */
        if (upd && f->what == IA_F_SAME) {          /* in the item already, as it is */
            const ia_file *old = ia_files_find(&rec.files, f->name);
            if (old) { CP(f->md5, old->md5); CP(f->sha1, old->sha1); CP(f->at, old->at); }
            continue;
        }
        if (was) {                      /* already sent before the interruption */
            CP(f->md5, was->md5);
            CP(f->sha1, was->sha1);
            meta_now(f->at);
            out->bytes_sent += f->size;
            r.pr.total_done += f->size;
            continue;
        }
        if (stopped(&r)) { rc = out->why = IA_ERR_CANCELED; break; }
        if ((rc = hash_one(&r, cfg->bagdir, f)) != IA_OK) break;
        sb_clear(&extra);
        /* the first file creates the item, with the fields; in an update
         * the version a file replaces stays in the item's history */
        if (upd) sb_puts(&extra, "x-archive-keep-old-version: 1\r\n");
        else if (i == 0) meta_headers(&r, cfg->meta, total, &extra);
        if ((rc = put_file(&r, cfg->identifier, f, &extra)) != IA_OK) break;
        meta_now(f->at);
        journal_file(cfg->bagdir, f);
        out->files_sent++;
        out->bytes_sent += f->size;
        r.pr.total_done += f->size;
    }
    sb_free(&extra);
    /* an update: then the files the box no longer has */
    for (k = 0; upd && rc == IA_OK && i == plan.n && k < plan.n; k++) {
        ia_file *f = &plan.f[k];
        if (f->what != IA_F_REMOVED || ia_files_find(&done, f->name)) continue;
        if (stopped(&r)) { rc = out->why = IA_ERR_CANCELED; break; }
        r.pr.file = k + 1;
        r.pr.name = f->name;
        tell(&r, IA_PH_DELETE);
        if ((rc = delete_file(&r, cfg->identifier, f)) != IA_OK) break;
        journal_file(cfg->bagdir, f);
        out->files_removed++;
    }
    if (i < plan.n || rc != IA_OK) {    /* stopped partway: resumable */
        state = IA_ST_INTERRUPTED;
        record_event(&rec, "interrupt", NULL);
    } else if ((rc = upd ? update_fields(&r, cfg->identifier)
                         : write_long_fields(&r, cfg->identifier)) != IA_OK) {
        state = IA_ST_INTERRUPTED;
    } else {
        int bad = 0, left = cfg->task_wait_s > 0 ? cfg->task_wait_s : IA_TASK_WAIT_S;
        int poll = cfg->task_poll_s > 0 ? cfg->task_poll_s : IA_TASK_POLL_S;
        int grc = IA_OK, ok;
        char extra_ev[120];
        finished = 1;
        journal_remove(cfg->bagdir);
        if (upd) {
            snprintf(extra_ev, sizeof(extra_ev), ", \"files\": %d, \"removed\": %d, \"fields\": %d",
                     out->files_sent, out->files_removed, out->fields_changed);
            record_event(&rec, "update", extra_ev);
        } else {
            snprintf(extra_ev, sizeof(extra_ev), ", \"files\": %d", plan.n);
            record_event(&rec, "create", extra_ev);
        }
        tell(&r, IA_PH_TASKS);
        for (;;) {
            ok = tasks_done(&r, cfg->identifier, &bad, &grc);
            if (grc != IA_OK) { rc = grc; break; }
            if (ok || bad > 0) break;
            if (item_ready(&r, cfg->identifier, &plan)) { ok = 1; break; }
            if (left <= 0) break;
            if (!wait_s(&r, poll < left ? poll : left, IA_PH_TASKS)) { rc = out->why = IA_ERR_CANCELED; break; }
            left -= poll;
        }
        if (rc == IA_OK && !ok) {
            state = IA_ST_PENDING;
            rc = out->why = IA_ERR_PENDING;
        } else if (rc == IA_OK) {
            if (bad > 0) {
                verify_fail(&r, cfg->identifier, "a task of Archive.org ended in error");
                rc = out->why = IA_ERR_VERIFY;
                state = IA_ST_FAILED;
            } else {
                rc = verify(&r, cfg->identifier, &plan);
                state = rc == IA_OK ? IA_ST_VERIFIED : IA_ST_FAILED;
            }
            record_event(&rec, "verify", rc == IA_OK ? ", \"result\": \"ok\"" : ", \"result\": \"failed\"");
        } else {
            state = IA_ST_PENDING;
        }
    }
    /* an update left halfway keeps the record of the item as it was made:
     * the plan of the resume is again against it, the journal says what is
     * done, and the files still to delete are not forgotten */
    if (upd && !finished) {
        rec.state = state;
    } else {
        record_fill(&rec, cfg, &plan, tagmanifest, state);
        if (upd) rec.kept = cfg->kept;
    }
    sb_init(&err);
    if (ia_record_save(cfg->bagdir, &rec, &err) && rc == IA_OK) {
        CP(out->detail, err.s ? err.s : "");
        rc = out->why = IA_ERR_RECORD;
    }
    sb_free(&err);
    ia_log(&r, "Upload finished: %d files sent, %lld bytes, state %s", out->files_sent,
           out->bytes_sent, ia_state_key(state));

out:
    memset(r.auth, 0, sizeof(r.auth));
    https_free(r.c);
    tls_anchors_free(r.anchors);
    ia_files_free(&plan);
    ia_files_free(&done);
    ia_record_free(&rec);
    return out->why;
}
