/* core/iarec.c and core/iaupload.c: the identifier, the fields, the plan
 * of the files and a whole upload against the small Archive.org of
 * tests/fake_https.c, on 127.0.0.1 only: no network needed, and nothing
 * ever reaches the real archive.org */
#include <stdlib.h>
#include "unit.h"
#include "core/archive.h"
#include "core/bagit.h"
#include "core/fsu8.h"
#include "core/iarec.h"
#include "core/iaupload.h"
#include "core/session.h"
#include "core/sha256.h"

#ifdef _WIN32
#include <windows.h>
#define FAKE_HTTPS "..\\build\\fake_https.exe"
static void nap(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#define FAKE_HTTPS "../build/fake_https"
static void nap(int ms) { usleep((useconds_t)ms * 1000); }
#endif

#define IROOT  "tmp_unit/ia"
#define PORTS  "tmp_unit/ia_ports.txt"
#define ITEM   "tmp_unit/fake_ia"

static int port_plain, port_tls;

/* ---- the fake server ---- */

#ifdef _WIN32
static HANDLE srv;
static int server_start(void)
{
    char cmd[512]; STARTUPINFOA si; PROCESS_INFORMATION pi;
    snprintf(cmd, sizeof(cmd), "%s %s tls plain good", FAKE_HTTPS, PORTS);
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    srv = pi.hProcess;
    return 1;
}
static void server_stop(void)
{
    if (!srv) return;
    TerminateProcess(srv, 0);
    WaitForSingleObject(srv, 5000);
    CloseHandle(srv);
    srv = NULL;
}
#else
static pid_t srv;
static int server_start(void)
{
    const char *argv[7];
    argv[0] = FAKE_HTTPS; argv[1] = PORTS; argv[2] = "tls";
    argv[3] = "plain"; argv[4] = "good"; argv[5] = NULL;
    srv = fork();
    if (srv == 0) { execv(FAKE_HTTPS, (char *const *)argv); _exit(127); }
    return srv > 0;
}
static void server_stop(void)
{
    if (srv <= 0) return;
    kill(srv, SIGTERM);
    waitpid(srv, NULL, 0);
    srv = 0;
}
#endif

static int read_ports(void)
{
    int tries;
    for (tries = 0; tries < 100; tries++) {
        FILE *f = fopen(PORTS, "r"); char name[64]; int p, ready = 0;
        if (f) {
            while (fscanf(f, "%63s", name) == 1) {
                if (!strcmp(name, "ready")) { ready = 1; break; }
                if (fscanf(f, "%d", &p) != 1) break;
                if (!strcmp(name, "plain")) port_plain = p;
                if (!strcmp(name, "good")) port_tls = p;
            }
            fclose(f);
            if (ready) return port_plain && port_tls;
        }
        nap(100);
    }
    return 0;
}

/* ---- a box to upload ---- */

static void put(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static char *slurp(const char *path)
{
    static char buf[200000]; FILE *f = u8_fopen(path, "rb"); size_t n;
    if (!f) return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    return buf;
}

static int has(const char *path, const char *needle)
{
    const char *s = slurp(path);
    return s && strstr(s, needle) != NULL;
}

/* a closed box: nf floppies and nc CDs, each read with version sw */
static meta_box *make_box(const char *folder, const char *title, const char *edition,
                          int nf, int nc, const char *sw, const char *notes)
{
    meta_box *b = meta_new(); strbuf err; int i, bad; char bag[400], p[700];
    sb_init(&err);
    snprintf(bag, sizeof(bag), IROOT "/%s", folder);
    u8_mkdirs(IROOT);
    CHECK(bag_create(bag, &err) == 0);
    str_copy(b->title, sizeof(b->title), title);
    str_copy(b->edition, sizeof(b->edition), edition);
    str_copy(b->platform, sizeof(b->platform), "MS-DOS");
    str_copy(b->publisher, sizeof(b->publisher), "Microsoft");
    str_copy(b->year, sizeof(b->year), "2000");
    str_copy(b->language, sizeof(b->language), "Italiano");
    if (notes) str_copy(b->notes, sizeof(b->notes), notes);
    str_copy(b->started, sizeof(b->started), "2026-10-10T10:00:00");
    str_copy(b->closed, sizeof(b->closed), "2026-10-10T11:00:00");
    meta_set_counts(b, nf, nc, 0);
    str_copy(b->names[MEDIA_FLOPPY], sizeof(b->names[0]), "FLOPPY{nn}");
    str_copy(b->names[MEDIA_CD], sizeof(b->names[0]), "CD{nn}");
    for (i = 1; i <= nf + nc; i++) {
        meta_disk *d = meta_disk_get(b, i);
        int kind, num;
        meta_slot(b, i, &kind, &num);
        d->kind = kind; d->num = num;
        str_copy(d->status, sizeof(d->status), kind == MEDIA_FLOPPY ? ST_CLEAN : ST_REDUMP);
        if (kind == MEDIA_FLOPPY) {
            str_copy(d->verdict, sizeof(d->verdict), "identical");
            d->agree = 2;
        }
        if (kind == MEDIA_FLOPPY) {
            snprintf(d->master, sizeof(d->master), "FLOPPY%02d.dsk", num);
            snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
            /* a size of its own per medium, so the order by size shows */
            {
                FILE *f = u8_fopen(p, "wb"); int k;
                if (f) { for (k = 0; k < 400 * i; k++) fputc('a' + i, f); fclose(f); }
            }
        } else {
            snprintf(d->master, sizeof(d->master), "CD%02d", num);
            snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
            u8_mkdirs(p);
            snprintf(p, sizeof(p), "%s/data/%s/x.bin", bag, d->master);
            put(p, "a CD read");
            snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
        }
        d->nreads = 1;
        str_copy(d->reads[0].file, sizeof(d->reads[0].file), d->master);
        str_copy(d->reads[0].software, sizeof(d->reads[0].software), sw ? sw : "");
        str_copy(d->reads[0].at, sizeof(d->reads[0].at), "2026-10-10T10:30:00");
        sha256_path(p, d->reads[0].sha256, &d->reads[0].bytes);
    }
    snprintf(p, sizeof(p), "%s/data", bag);
    CHECK(meta_save(b, p, NULL, NULL, &err) == 0);
    CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    sb_free(&err);
    return b;
}

static void bag_of(const char *folder, char *out, size_t n)
{
    snprintf(out, n, IROOT "/%s", folder);
}

/* ---- the identifier ---- */

static void test_ident(void)
{
    meta_box *b = meta_new();
    char id[IA_ID_MAX];

    CHECK(!ia_ident_ok("abcd"));                      /* too short */
    CHECK(ia_ident_ok("abcde"));
    CHECK(!ia_ident_ok("-abcde"));                    /* the first one is alphanumeric */
    CHECK(ia_ident_ok("a.b_c-d1"));
    CHECK(!ia_ident_ok("abc de"));
    CHECK(!ia_ident_ok("abc/de"));
    CHECK(!ia_ident_ok(""));

    str_copy(b->title, sizeof(b->title), "Citt\xc3\xa0 perduta!");
    str_copy(b->platform, sizeof(b->platform), "MS-DOS");
    str_copy(b->edition, sizeof(b->edition), "2.0");
    str_copy(b->language, sizeof(b->language), "Italiano");
    ia_ident_propose(b, NULL, id, sizeof(id));
    CHECK_STR(id, "citta-perduta-2-0-ms-dos-ita");
    ia_ident_propose(b, "ra", id, sizeof(id));
    CHECK_STR(id, "ra-citta-perduta-2-0-ms-dos-ita");

    /* English is not written in the identifier */
    str_copy(b->language, sizeof(b->language), "English");
    b->edition[0] = 0;
    ia_ident_propose(b, NULL, id, sizeof(id));
    CHECK_STR(id, "citta-perduta-ms-dos");

    /* a long title is cut on a dash, with room for a suffix */
    str_copy(b->title, sizeof(b->title),
             "Un titolo molto molto lungo che non sta in novanta caratteri "
             "nemmeno a volerlo tagliare con cura");
    ia_ident_propose(b, NULL, id, sizeof(id));
    CHECK(strlen(id) <= IA_ID_PROPOSE && ia_ident_ok(id));
    CHECK(id[strlen(id) - 1] != '-');

    /* a title with nothing usable still gives a valid identifier */
    str_copy(b->title, sizeof(b->title), "!!");
    b->platform[0] = 0;
    b->language[0] = 0;
    ia_ident_propose(b, NULL, id, sizeof(id));
    CHECK(ia_ident_ok(id));

    CHECK_STR(ia_lang_marc("italiano"), "ita");
    CHECK_STR(ia_lang_marc("Deutsch"), "ger");
    CHECK_STR(ia_lang_marc("Klingon"), "Klingon");
    meta_delete(b);
}

/* ---- the fields ---- */

static void test_ia_fields(void)
{
    char bag[400];
    ia_meta m;
    strbuf err;
    meta_box *b;

    sb_init(&err);
    b = make_box("Windows 2000 Server", "Windows 2000 Server", "Retail IT", 2, 1, "261026",
                 "Scatola grande, due dischetti e un CD.");
    meta_delete(b);
    bag_of("Windows 2000 Server (Retail IT)", bag, sizeof(bag));
    if (!u8_isdir(bag)) bag_of("Windows 2000 Server", bag, sizeof(bag));
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    CHECK_STR(m.mediatype, "software");
    CHECK_STR(m.collection, "open_source_software");
    CHECK_STR(m.title, "Windows 2000 Server (Retail IT)");
    CHECK_STR(m.creator, "Microsoft");
    CHECK_STR(m.publisher, "Microsoft");
    CHECK_STR(m.date, "2000");
    CHECK_STR(m.platform, "MS-DOS");
    CHECK_STR(m.media, "2 floppy, 1 CD");
    CHECK(m.nlang == 1);
    CHECK_STR(m.language[0], "ita");
    CHECK(strstr(m.rights, "all rights remain with the copyright holders") != NULL);
    CHECK_STR(m.scanner, "RetroArchiver " RA_VERSION);
    CHECK(strstr(m.profile, "bagit-profile") != NULL);
    /* the subjects: the platform, the kinds of medium, and ours */
    CHECK(m.nsubj >= 5);
    CHECK_STR(m.subject[0], "MS-DOS");
    CHECK(strstr(m.subject[1], "floppy disk image") != NULL);
    CHECK(strstr(m.subject[2], "CD-ROM image") != NULL);

    /* the description: the notes, the outcomes, the files, the trace */
    CHECK(strstr(m.description, "Scatola grande, due dischetti e un CD.\n\n") == m.description);
    CHECK(strstr(m.description, "\nfloppy 1: clean (2 reads agree)\n") != NULL);
    CHECK(strstr(m.description, "\nfloppy 2: clean (2 reads agree)\n") != NULL);
    CHECK(strstr(m.description, "\nCD 1: redump-verified\n") != NULL);
    CHECK(strstr(m.description, "Files: a BagIt bag; see manifest-sha256.txt") != NULL);
    CHECK(strstr(m.description, "Archived with: RetroArchiver 261026\n") != NULL);
    CHECK(strstr(m.description, "Box incomplete") == NULL);
    CHECK(strstr(m.description, "Edited with") == NULL);
    sb_free(&err);
}

/* a converted box is "Archived with" the version that converted it, and
 * the edits after it are their own line */
static void test_ia_history(void)
{
    char bag[400], data[500], p[700];
    ia_meta m;
    strbuf err;
    meta_box *b;

    sb_init(&err);
    b = make_box("Antico", "Antico", "", 2, 0, "261017", NULL);
    bag_of("Antico", bag, sizeof(bag));
    snprintf(data, sizeof(data), "%s/data", bag);
    /* converted by 261024, then edited by 261026: three versions, but
     * only the conversion and the edits count */
    CHECK(meta_history_add(b, "{\"at\": \"2026-10-24T09:00:00\", \"action\": \"convert\", "
                              "\"software\": \"261024\"}") == 0);
    CHECK(meta_history_add(b, "{\"at\": \"2026-10-26T09:00:00\", \"action\": \"edit\", "
                              "\"software\": \"RetroArchiver 261026\", \"reason\": \"titolo\"}") == 0);
    CHECK(meta_save(b, data, NULL, NULL, &err) == 0);
    {
        int bad;
        CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    }
    CHECK(ia_meta_from_box(bag, "test_collection", &m, &err) == 0);
    CHECK_STR(m.collection, "test_collection");
    CHECK(strstr(m.description, "Archived with: RetroArchiver 261024\n") != NULL);
    CHECK(strstr(m.description, "Edited with: RetroArchiver 261026 (2026-10-26)\n") != NULL);
    meta_delete(b);

    /* reads of two versions: grouped, with the media of each */
    b = make_box("Misto", "Misto", "", 3, 0, "261026", NULL);
    bag_of("Misto", bag, sizeof(bag));
    snprintf(data, sizeof(data), "%s/data", bag);
    str_copy(b->disk[2].reads[0].software, sizeof(b->disk[2].reads[0].software), "261030");
    CHECK(meta_save(b, data, NULL, NULL, &err) == 0);
    {
        int bad;
        CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    }
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    CHECK(strstr(m.description, "Archived with: RetroArchiver 261026 (floppy 1-2), "
                                "RetroArchiver 261030 (floppy 3)\n") != NULL);
    meta_delete(b);

    /* a bag whose reads say no version: the one that wrote the package */
    b = make_box("Vecchio", "Vecchio", "", 1, 0, "", NULL);
    bag_of("Vecchio", bag, sizeof(bag));
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    CHECK(strstr(m.description, "Archived with: RetroArchiver " RA_VERSION "\n") != NULL);
    meta_delete(b);

    /* an incomplete box says so in the description */
    b = make_box("Incompleto", "Incompleto", "", 2, 0, "261026", NULL);
    bag_of("Incompleto", bag, sizeof(bag));
    snprintf(data, sizeof(data), "%s/data", bag);
    meta_set_counts(b, 3, 0, 0);
    str_copy(meta_disk_get(b, 3)->status, 24, ST_MISSING);
    CHECK(meta_save(b, data, NULL, NULL, &err) == 0);
    {
        int bad;
        CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    }
    snprintf(p, sizeof(p), "%s/bag-info.txt", bag);
    CHECK(has(p, "Box-Status: incomplete"));
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    CHECK(strstr(m.description, "Box incomplete: floppy 3") != NULL);
    CHECK(strstr(m.description, "floppy 3: missing") != NULL);
    meta_delete(b);
    sb_free(&err);
}

/* ---- the plan of the files ---- */

static void test_plan(void)
{
    char bag[400];
    ia_files p;
    strbuf err;
    ia_record prev;
    int i;

    sb_init(&err);
    bag_of("Misto", bag, sizeof(bag));
    CHECK(ia_plan(bag, NULL, &p, &err) == 0);
    /* bagit.txt first, the tag files last */
    CHECK(p.n >= 8);
    CHECK_STR(p.f[0].name, "bagit.txt");
    CHECK_STR(p.f[p.n - 1].name, "tagmanifest-sha256.txt");
    CHECK_STR(p.f[p.n - 2].name, "manifest-sha256.txt");
    CHECK_STR(p.f[p.n - 3].name, "bag-info.txt");
    /* data/ from the small files to the big ones */
    for (i = 2; i < p.n - 3; i++) CHECK(p.f[i - 1].size <= p.f[i].size);
    for (i = 1; i < p.n - 3; i++) CHECK(!strncmp(p.f[i].name, "data/", 5));
    /* every file has its digest from the manifest, and its size */
    for (i = 0; i < p.n; i++) CHECK(strlen(p.f[i].sha256) == 64 && p.f[i].size > 0);
    /* with no earlier upload everything is new */
    for (i = 0; i < p.n; i++) CHECK(p.f[i].what == IA_F_NEW);

    /* against a record: the same, the changed and the ones that went */
    ia_record_init(&prev);
    for (i = 0; i < p.n; i++) {
        ia_file f = p.f[i];
        if (!strcmp(f.name, "bag-info.txt")) str_copy(f.sha256, sizeof(f.sha256), "beef");
        ia_files_add(&prev.files, &f);
    }
    {
        ia_file gone;
        memset(&gone, 0, sizeof(gone));
        str_copy(gone.name, sizeof(gone.name), "data/FLOPPY09.dsk");
        gone.size = 10;
        ia_files_add(&prev.files, &gone);
    }
    ia_files_free(&p);
    CHECK(ia_plan(bag, &prev, &p, &err) == 0);
    for (i = 0; i < p.n; i++) {
        if (!strcmp(p.f[i].name, "bag-info.txt")) CHECK(p.f[i].what == IA_F_CHANGED);
        else if (!strcmp(p.f[i].name, "data/FLOPPY09.dsk")) CHECK(p.f[i].what == IA_F_REMOVED);
        else CHECK(p.f[i].what == IA_F_SAME);
    }
    CHECK(ia_files_find(&p, "data/FLOPPY09.dsk") != NULL);
    ia_record_free(&prev);
    ia_files_free(&p);

    /* a folder that is not a bag: a clear error, no crash */
    sb_clear(&err);
    CHECK(ia_plan("tmp_unit/ia/nowhere", NULL, &p, &err) == -1);
    CHECK(err.s && err.s[0]);
    ia_files_free(&p);
    sb_free(&err);
}

/* ---- which boxes may go ---- */

static void test_can_upload(void)
{
    char bag[400], p[700];
    strbuf err;
    sb_init(&err);
    bag_of("Misto", bag, sizeof(bag));
    CHECK(ia_box_can_upload(bag) == IA_BOX_OK);
    CHECK(ia_box_can_upload("tmp_unit/ia/nowhere") == IA_BOX_NOTBOX);
    /* an edit under way */
    snprintf(p, sizeof(p), "%s/" BAG_EDIT_DIR, bag);
    u8_mkdirs(p);
    CHECK(ia_box_can_upload(bag) == IA_BOX_EDITING);
    u8_rmtree(p);
    /* a box of 261017: it is converted first */
    snprintf(p, sizeof(p), "%s/%s", bag, SESSION_ASIDE_DIR_OLD);
    u8_mkdirs(p);
    CHECK(ia_box_can_upload(bag) == IA_BOX_OLD);
    u8_rmtree(p);
    /* still open */
    snprintf(p, sizeof(p), "%s/%s", bag, BAG_OPEN_MARK);
    put(p, "");
    CHECK(ia_box_can_upload(bag) == IA_BOX_OPEN);
    u8_remove(p);
    CHECK(ia_box_can_upload(bag) == IA_BOX_OK);
    sb_free(&err);
}

/* ---- a whole upload ---- */

typedef struct { int logs, progress, cancel_after, canceled; char last[400]; } hostctx;

static void h_progress(void *ctx, const ia_progress *p)
{
    hostctx *h = (hostctx *)ctx;
    (void)p;
    h->progress++;
}

static void h_log(void *ctx, const char *line)
{
    hostctx *h = (hostctx *)ctx;
    h->logs++;
    str_copy(h->last, sizeof(h->last), line);
}

static int h_canceled(void *ctx)
{
    hostctx *h = (hostctx *)ctx;
    if (!h->cancel_after) return 0;
    if (--h->cancel_after > 0) return 0;
    h->canceled = 1;
    h->cancel_after = 1;                 /* it stays cancelled */
    return 1;
}

static ia_host make_host(hostctx *h)
{
    ia_host host;
    memset(&host, 0, sizeof(host));
    host.ctx = h;
    host.progress = h_progress;
    host.log = h_log;
    host.canceled = h_canceled;
    return host;
}

static ia_config make_cfg(const char *bag, const ia_meta *m, const char *id,
                          char *md, char *s3, const char *scenario)
{
    ia_config c;
    char suffix[40] = "";
    memset(&c, 0, sizeof(c));
    if (scenario && scenario[0]) snprintf(suffix, sizeof(suffix), "-%s", scenario);
    snprintf(md, 200, "http://127.0.0.1:%d%s%s", port_plain, scenario && scenario[0] ? "/ia" : "", suffix);
    snprintf(s3, 200, "http://127.0.0.1:%d/s3%s", port_plain, suffix);
    c.bagdir = bag;
    c.md_base = md;
    c.s3_base = s3;
    c.access = "goodaccess";
    c.secret = "goodsecret";
    c.identifier = id;
    c.meta = m;
    c.connect_ms = 5000;
    c.io_ms = 10000;
    c.task_wait_s = 1;
    c.task_poll_s = 1;
    c.put_tries = 0;
    return c;
}

static void item_reset(void)
{
    u8_rmtree(ITEM);
}

static void test_upload_ok(void)
{
    char bag[400], md[200], s3[200], path[700];
    ia_meta m;
    ia_config cfg;
    ia_result res;
    ia_host host;
    hostctx h;
    strbuf err;
    ia_record rec;

    sb_init(&err);
    memset(&h, 0, sizeof(h));
    host = make_host(&h);
    item_reset();
    bag_of("Misto", bag, sizeof(bag));
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    cfg = make_cfg(bag, &m, "misto-ms-dos-ita", md, s3, NULL);

    CHECK(ia_upload(&cfg, &host, &res) == IA_OK);
    CHECK(res.why == IA_OK && res.mismatch == 0);
    CHECK(res.files_sent >= 8 && res.bytes_sent > 0);
    CHECK(h.logs >= 2 && h.progress > 0);
    CHECK_STR(res.url, "https://archive.org/details/misto-ms-dos-ita");

    /* the journal is gone and the record says verified */
    ia_journal_path(bag, path, sizeof(path));
    CHECK(!u8_exists(path));
    ia_record_path(bag, path, sizeof(path));
    CHECK(u8_exists(path));
    CHECK(has(path, "\"schema\": \"" IA_RECORD_SCHEMA "\""));
    CHECK(has(path, "\"state\": \"verified\""));
    CHECK(has(path, "\"identifier\": \"misto-ms-dos-ita\""));
    CHECK(has(path, "\"action\": \"verify\""));
    /* no key ever lands in the record */
    CHECK(!has(path, "goodsecret") && !has(path, "goodaccess"));
    /* the description's line breaks reach Archive.org as <br>: a header
     * turns a plain one into a space (the first real upload came out on
     * one line) */
    CHECK(has("tmp_unit/fake_ia/meta.txt", "clean (2 reads agree)<br>"));

    CHECK(ia_record_load(bag, &rec) == 1);
    CHECK(rec.state == IA_ST_VERIFIED);
    CHECK(rec.files.n >= 8);
    CHECK(strlen(rec.tagmanifest) == 64);
    CHECK_STR(rec.meta.title, m.title);
    CHECK(ia_files_find(&rec.files, "bagit.txt") != NULL);
    CHECK(strlen(ia_files_find(&rec.files, "bagit.txt")->md5) == 32);
    CHECK(strlen(ia_files_find(&rec.files, "bagit.txt")->sha1) == 40);
    ia_record_free(&rec);

    /* the archive sees it: verified, and to update once the bag changes */
    CHECK(ia_box_state(bag, NULL) == IA_ST_VERIFIED);
    {
        arc_box ab;
        CHECK(arc_load(bag, &ab) == 1);
        CHECK(ab.ia_state == IA_ST_VERIFIED);
        CHECK_STR(ab.ia_id, "misto-ms-dos-ita");
        CHECK_STR(ab.ia_url, "https://archive.org/details/misto-ms-dos-ita");
        CHECK(strlen(ab.ia_date) == 19);
    }
    snprintf(path, sizeof(path), "%s/tagmanifest-sha256.txt", bag);
    {
        FILE *f = u8_fopen(path, "ab");
        if (f) { fputs("0000  extra.txt\n", f); fclose(f); }
    }
    CHECK(ia_box_state(bag, NULL) == IA_ST_OUTDATED);
    sb_free(&err);
}

/* the item is on Archive.org: the identifier is no longer free, and the
 * first one that is gets a -2 */
static void test_ident_remote(void)
{
    char md[200], s3[200];
    ia_config cfg;
    ia_result res;
    char id[IA_ID_MAX];

    cfg = make_cfg(NULL, NULL, NULL, md, s3, NULL);
    CHECK(ia_ident_check(&cfg, "misto-ms-dos-ita", NULL, &res) == IA_OK);
    CHECK(res.ident == IA_ID_TAKEN);
    /* ours, when that is the identifier this box already has */
    CHECK(ia_ident_check(&cfg, "misto-ms-dos-ita", "misto-ms-dos-ita", &res) == IA_OK);
    CHECK(res.ident == IA_ID_OURS);
    CHECK(ia_ident_check(&cfg, "qualcosa-daltro", NULL, &res) == IA_OK);
    CHECK(res.ident == IA_ID_FREE);
    /* a bad one never leaves the PC */
    CHECK(ia_ident_check(&cfg, "no", NULL, &res) == IA_ERR_IDENT);

    CHECK(ia_ident_free(&cfg, "misto-ms-dos-ita", id, sizeof(id), &res) == IA_OK);
    CHECK_STR(id, "misto-ms-dos-ita-2");
    CHECK(ia_ident_free(&cfg, "libero-del-tutto", id, sizeof(id), &res) == IA_OK);
    CHECK_STR(id, "libero-del-tutto");
}

static void upload_case(const char *folder, const char *scenario, const char *id,
                        int want, hostctx *h, ia_result *res)
{
    char bag[400], md[200], s3[200];
    ia_meta m;
    ia_config cfg;
    ia_host host;
    strbuf err;
    sb_init(&err);
    host = make_host(h);
    bag_of(folder, bag, sizeof(bag));
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    cfg = make_cfg(bag, &m, id, md, s3, scenario);
    CHECK(ia_upload(&cfg, &host, res) == want);
    sb_free(&err);
}

static void test_upload_problems(void)
{
    char bag[400], md[200], s3[200], path[700];
    hostctx h;
    ia_result res;
    ia_meta m;
    ia_config cfg;
    ia_host host;
    strbuf err;

    sb_init(&err);
    /* keys Archive.org does not accept: the item stays empty, so it is
     * the keys and not the identifier */
    item_reset();
    memset(&h, 0, sizeof(h));
    host = make_host(&h);
    bag_of("Antico", bag, sizeof(bag));
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    cfg = make_cfg(bag, &m, "antico-ms-dos-ita", md, s3, NULL);
    cfg.secret = "wrong";
    CHECK(ia_upload(&cfg, &host, &res) == IA_ERR_KEYS);
    /* the journal is left behind: the upload can be taken up again */
    ia_journal_path(bag, path, sizeof(path));
    CHECK(u8_exists(path));
    CHECK(ia_box_state(bag, NULL) == IA_ST_INTERRUPTED);
    CHECK(!has(path, "wrong"));
    u8_remove(path);
    ia_record_path(bag, path, sizeof(path));
    u8_remove(path);

    /* the queue is full: waited for and tried again, and it goes through */
    item_reset();
    memset(&h, 0, sizeof(h));
    upload_case("Antico", "slowdown", "antico-ms-dos-ita", IA_OK, &h, &res);
    CHECK(h.logs > 2);

    /* "appears to be spam": not tried again */
    item_reset();
    memset(&h, 0, sizeof(h));
    upload_case("Antico", "spam", "antico-ms-dos-ita", IA_ERR_SPAM, &h, &res);
    CHECK(strstr(res.detail, "spam") != NULL);

    /* the item reports another MD5: the verification says which file */
    item_reset();
    memset(&h, 0, sizeof(h));
    upload_case("Antico", "badmd5", "antico-ms-dos-ita", IA_ERR_VERIFY, &h, &res);
    CHECK(res.mismatch >= 1);
    CHECK(strstr(res.detail, "MD5") != NULL);
    bag_of("Antico", bag, sizeof(bag));
    ia_record_path(bag, path, sizeof(path));
    CHECK(has(path, "\"state\": \"failed\""));
    CHECK(ia_box_state(bag, NULL) == IA_ST_FAILED);

    /* a file under data/ that the package has not */
    item_reset();
    memset(&h, 0, sizeof(h));
    upload_case("Antico", "extra", "antico-ms-dos-ita", IA_ERR_VERIFY, &h, &res);
    CHECK_STR(res.file, "data/intruder.bin");

    /* a task of Archive.org in error */
    item_reset();
    memset(&h, 0, sizeof(h));
    upload_case("Antico", "taskerror", "antico-ms-dos-ita", IA_ERR_VERIFY, &h, &res);
    CHECK(strstr(res.detail, "task") != NULL);

    /* the tasks never end: uploaded, verification still to come */
    item_reset();
    memset(&h, 0, sizeof(h));
    upload_case("Antico", "busy", "antico-ms-dos-ita", IA_ERR_PENDING, &h, &res);
    CHECK(ia_box_state(bag, NULL) == IA_ST_PENDING);

    /* the item belongs to somebody else: a 403 on an item that exists */
    item_reset();
    memset(&h, 0, sizeof(h));
    upload_case("Antico", "forbidden", "antico-ms-dos-ita", IA_ERR_KEYS, &h, &res);

    /* Stop, partway: the journal stays, the state is interrupted */
    item_reset();
    memset(&h, 0, sizeof(h));
    h.cancel_after = 3;
    upload_case("Antico", NULL, "antico-ms-dos-ita", IA_ERR_CANCELED, &h, &res);
    CHECK(h.canceled);
    ia_journal_path(bag, path, sizeof(path));
    CHECK(u8_exists(path));
    CHECK(ia_box_state(bag, NULL) == IA_ST_INTERRUPTED);
    u8_remove(path);
    sb_free(&err);
}

/* a file changed on the disk since the box was closed: nothing is sent */
static void test_changed_file(void)
{
    char bag[400], path[700];
    hostctx h;
    ia_result res;
    strbuf err;
    sb_init(&err);
    item_reset();
    memset(&h, 0, sizeof(h));
    bag_of("Vecchio", bag, sizeof(bag));
    snprintf(path, sizeof(path), "%s/data/FLOPPY01.dsk", bag);
    put(path, "somebody edited this by hand");
    upload_case("Vecchio", NULL, "vecchio-ms-dos-ita", IA_ERR_CHANGED, &h, &res);
    CHECK_STR(res.file, "data/FLOPPY01.dsk");
    /* that file never left: the journal does not name it */
    ia_journal_path(bag, path, sizeof(path));
    CHECK(u8_exists(path) && !has(path, "FLOPPY01.dsk"));
    u8_remove(path);
    sb_free(&err);
}

/* Stop, then Resume: the files already sent are not sent again, and
 * uploads/ leaves the bag valid (a digest recheck does not see it) */
static void test_resume(void)
{
    char bag[400], path[700];
    hostctx h;
    ia_result first, second;
    arc_check c;
    strbuf err;

    sb_init(&err);
    item_reset();
    meta_delete(make_box("Ripresa", "Ripresa", "", 3, 0, "261026", NULL));
    bag_of("Ripresa", bag, sizeof(bag));
    memset(&h, 0, sizeof(h));
    h.cancel_after = 5;
    upload_case("Ripresa", NULL, "ripresa-ms-dos-ita", IA_ERR_CANCELED, &h, &first);
    CHECK(first.files_sent >= 1);
    ia_journal_path(bag, path, sizeof(path));
    CHECK(u8_exists(path));

    memset(&h, 0, sizeof(h));
    upload_case("Ripresa", NULL, "ripresa-ms-dos-ita", IA_OK, &h, &second);
    /* the ones already up were not sent again */
    CHECK(second.files_sent >= 1 && first.files_sent >= 1);
    CHECK(first.files_sent + second.files_sent >= 8);
    CHECK(!u8_exists(path));
    CHECK(ia_box_state(bag, NULL) == IA_ST_VERIFIED);

    /* the record and the journal live outside the package: the bag is
     * still whole and nothing under data/ is in excess */
    CHECK(arc_verify(bag, &c, &err) == 0);
    CHECK(c.changed == 0 && c.lost == 0 && c.extra == 0);
    sb_free(&err);
}

static int count(const char *path, const char *needle)
{
    const char *s = slurp(path), *p;
    int n = 0;
    for (p = s; p && (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

/* the item's fields as Archive.org reports them, and the merge of an update */
static void test_item_fields(void)
{
    ia_meta live, sent, proposed, out;
    unsigned site;
    CHECK(ia_meta_from_item("{}", &live) == -1);
    CHECK(ia_meta_from_item("{\"metadata\":{\"title\":\"T\",\"description\":\"a<br />b &amp; c<br>d\","
                            "\"subject\":\"one\",\"creator\":[\"x\",\"y\"],\"collection\":"
                            "[\"test_collection\",\"other\"]}}", &live) == 0);
    CHECK_STR(live.description, "a\nb & c\nd");
    CHECK(live.nsubj == 1 && !strcmp(live.subject[0], "one"));
    CHECK_STR(live.creator, "x; y");
    CHECK_STR(live.collection, "test_collection");

    memset(&sent, 0, sizeof(sent));
    strcpy(sent.title, "Old");
    strcpy(sent.creator, "Microsoft");
    sent.nsubj = 2; strcpy(sent.subject[0], "a"); strcpy(sent.subject[1], "b");
    proposed = sent;
    strcpy(proposed.title, "New");                    /* changed in the box */
    live = sent;
    strcpy(live.creator, "Microsoft Corporation");    /* changed on the site */
    strcpy(live.subject[0], "b"); strcpy(live.subject[1], "a");   /* same, other order */
    site = ia_meta_merge(&proposed, &sent, &live, 0, &out);
    CHECK(site == (1u << IA_FD_CREATOR));
    CHECK_STR(out.title, "New");
    CHECK_STR(out.creator, "Microsoft Corporation");
    CHECK(ia_meta_diff(&out, &live) == (1u << IA_FD_TITLE));
    /* left to the site last time: it stays the site's even now that the
     * site has what we sent */
    site = ia_meta_merge(&proposed, &live, &live, 1u << IA_FD_CREATOR, &out);
    CHECK(site == (1u << IA_FD_CREATOR));
    CHECK_STR(out.creator, "Microsoft Corporation");
    /* unless the box now says the same */
    strcpy(proposed.creator, "Microsoft Corporation");
    CHECK(ia_meta_merge(&proposed, &live, &live, 1u << IA_FD_CREATOR, &out) == 0);
    /* a description that came back with spaces where the line breaks were
     * (a header carried it, before the <br>; seen on the VM) is not a change
     * made on the site: the box's goes up, with its line breaks */
    strcpy(sent.description, "floppy 1: clean\nRead with: SAMdisk");
    proposed = sent;
    strcpy(proposed.description, "floppy 1: clean\nRead with: SAMdisk\nNote: new");
    live = sent;
    strcpy(live.description, "floppy 1: clean Read with: SAMdisk");
    CHECK(ia_meta_merge(&proposed, &sent, &live, 0, &out) == 0);
    CHECK_STR(out.description, proposed.description);
    CHECK(ia_meta_diff(&out, &live) == (1u << IA_FD_DESC));
    strcpy(live.description, "floppy 1: clean Read with: SAMdisk!");
    CHECK(ia_meta_merge(&proposed, &sent, &live, 0, &out) == (1u << IA_FD_DESC));
}

/* the box changes after the upload: an update sends only what changed,
 * keeps the old versions in the item's history, deletes what the box no
 * longer has, and writes only the fields that differ, leaving to
 * Archive.org the one changed there */
static void test_update(void)
{
    char bag[400], md[200], s3[200], path[700], saved[700], *rec_text;
    ia_meta m, live, final;
    ia_config cfg;
    ia_result res;
    ia_host host;
    hostctx h;
    strbuf err;
    ia_record rec;
    unsigned site;
    FILE *f;

    sb_init(&err);
    item_reset();
    meta_delete(make_box("Aggiorna", "Aggiorna", "", 2, 0, "261026", NULL));
    bag_of("Aggiorna", bag, sizeof(bag));
    memset(&h, 0, sizeof(h));
    upload_case("Aggiorna", NULL, "aggiorna-ms-dos-ita", IA_OK, &h, &res);
    CHECK(!res.update);

    /* an edit: the second floppy is gone, a CD came, the title changed. The
     * record lives outside the package and stays */
    ia_record_path(bag, path, sizeof(path));
    rec_text = strdup(slurp(path));
    snprintf(saved, sizeof(saved), "%s", path);
    u8_rmtree(bag);
    meta_delete(make_box("Aggiorna", "Aggiorna di nuovo", "", 1, 1, "261104", NULL));
    {
        char dir[700];
        ia_upload_dir(bag, dir, sizeof(dir));
        u8_mkdirs(dir);
    }
    if (rec_text) put(saved, rec_text);
    free(rec_text);
    CHECK(ia_box_state(bag, NULL) == IA_ST_OUTDATED);

    /* meanwhile somebody added a publisher on the site */
    if ((f = u8_fopen("tmp_unit/fake_ia/meta.txt", "ab")) != NULL) {
        fputs("publisher\tSomeone Else\n", f);
        fclose(f);
    }
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    cfg = make_cfg(bag, &m, "aggiorna-ms-dos-ita", md, s3, NULL);
    CHECK(ia_item_read(&cfg, "aggiorna-ms-dos-ita", &live, &res) == IA_OK);
    CHECK(res.ident == IA_ID_OURS);
    CHECK_STR(live.publisher, "Microsoft; Someone Else");
    CHECK(ia_record_load(bag, &rec) == 1);
    CHECK(ia_record_made(&rec));
    site = ia_meta_merge(&m, &rec.meta, &live, rec.kept, &final);
    CHECK(site == (1u << IA_FD_PUBLISHER));
    CHECK_STR(final.title, "Aggiorna di nuovo");
    ia_record_free(&rec);
    cfg.meta = &final;
    cfg.kept = ia_meta_diff(&final, &m) & ~ia_meta_diff(&final, &live);
    CHECK(cfg.kept == (1u << IA_FD_PUBLISHER));

    /* stopped partway: the record still describes the item as it was made */
    memset(&h, 0, sizeof(h));
    host = make_host(&h);
    h.cancel_after = 2;
    CHECK(ia_upload(&cfg, &host, &res) == IA_ERR_CANCELED);
    CHECK(res.update);
    CHECK(ia_box_state(bag, NULL) == IA_ST_INTERRUPTED);
    CHECK(ia_record_load(bag, &rec) == 1);
    CHECK(ia_files_find(&rec.files, "data/FLOPPY02.dsk") != NULL);
    ia_record_free(&rec);

    /* taken up again */
    memset(&h, 0, sizeof(h));
    host = make_host(&h);
    CHECK(ia_upload(&cfg, &host, &res) == IA_OK);
    CHECK(res.update && res.mismatch == 0);
    CHECK(res.files_removed == 1);
    CHECK(res.fields_changed >= 1);
    /* the floppy that did not change went up once, the old versions are
     * in the item's history, the floppy no longer in the box is deleted */
    CHECK(count("tmp_unit/fake_ia/puts.txt", "data/FLOPPY01.dsk") == 1);
    CHECK(has("tmp_unit/fake_ia/puts.txt", "data/CD01/x.bin"));
    CHECK(has("tmp_unit/fake_ia/keep.txt", "data/metadata.json"));
    CHECK(has("tmp_unit/fake_ia/keep.txt", "data/FLOPPY02.dsk"));
    CHECK(has("tmp_unit/fake_ia/deletes.txt", "data/FLOPPY02.dsk"));
    CHECK(has("tmp_unit/fake_ia/files.txt", "history/files/data/metadata.json.~1~"));
    CHECK(has("tmp_unit/fake_ia/files.txt", "history/files/data/FLOPPY02.dsk.~1~"));
    /* the new title is on the item, the site's publisher too */
    CHECK(has("tmp_unit/fake_ia/meta.txt", "title\tAggiorna di nuovo"));
    CHECK(has("tmp_unit/fake_ia/meta.txt", "publisher\tSomeone Else"));
    CHECK(!has("tmp_unit/fake_ia/patches.txt", "/publisher"));

    ia_record_path(bag, path, sizeof(path));
    CHECK(has(path, "\"action\": \"update\""));
    CHECK(has(path, "\"kept_from_archive_org\""));
    CHECK(ia_box_state(bag, NULL) == IA_ST_VERIFIED);
    CHECK(ia_record_load(bag, &rec) == 1);
    CHECK(rec.kept == (1u << IA_FD_PUBLISHER));
    CHECK(ia_files_find(&rec.files, "data/FLOPPY02.dsk") == NULL);
    CHECK(ia_files_find(&rec.files, "data/CD01/x.bin") != NULL);
    CHECK_STR(rec.meta.title, "Aggiorna di nuovo");
    ia_record_free(&rec);

    /* nothing changed since: an update sends no file and no field */
    memset(&h, 0, sizeof(h));
    host = make_host(&h);
    CHECK(ia_upload(&cfg, &host, &res) == IA_OK);
    CHECK(res.files_sent == 0 && res.files_removed == 0 && res.fields_changed == 0);
    sb_free(&err);
}

/* the CA bundle the setup has not installed yet, and TLS when it is there */
static void test_certs(void)
{
    char bag[400], md[200], s3[200];
    ia_meta m;
    ia_config cfg;
    ia_result res;
    ia_host host;
    hostctx h;
    strbuf err;

    sb_init(&err);
    memset(&h, 0, sizeof(h));
    host = make_host(&h);
    item_reset();
    bag_of("Antico", bag, sizeof(bag));
    CHECK(ia_meta_from_box(bag, NULL, &m, &err) == 0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.bagdir = bag;
    cfg.access = "goodaccess";
    cfg.secret = "goodsecret";
    cfg.identifier = "antico-ms-dos-ita";
    cfg.meta = &m;
    cfg.connect_ms = 5000;
    cfg.io_ms = 10000;
    cfg.task_wait_s = 1;
    cfg.task_poll_s = 1;
    snprintf(md, sizeof(md), "https://localhost:%d", port_tls);
    snprintf(s3, sizeof(s3), "https://localhost:%d/s3", port_tls);
    cfg.md_base = md;
    cfg.s3_base = s3;
    cfg.cacert = "tls/missing.pem";
    CHECK(ia_upload(&cfg, &host, &res) == IA_ERR_NO_CERTS);
    cfg.cacert = NULL;
    CHECK(ia_upload(&cfg, &host, &res) == IA_ERR_NO_CERTS);
    CHECK(ia_ident_check(&cfg, "antico-ms-dos-ita", NULL, &res) == IA_ERR_NO_CERTS);

    /* with the test CA the whole upload goes over TLS */
    cfg.cacert = "tls/ca.pem";
    item_reset();
    CHECK(ia_upload(&cfg, &host, &res) == IA_OK);
    sb_free(&err);
}

void test_iaupload(void)
{
    u8_mkdir("tmp_unit");
    u8_rmtree(IROOT);
    remove(PORTS);
    test_ident();
    test_ia_fields();
    test_ia_history();
    test_plan();
    test_can_upload();
    test_item_fields();
    if (!server_start() || !read_ports()) {
        CHECK(!"fake_https did not start");
        server_stop();
        return;
    }
    test_upload_ok();
    test_ident_remote();
    test_upload_problems();
    test_changed_file();
    test_resume();
    test_update();
    test_certs();
    item_reset();
    server_stop();
}
