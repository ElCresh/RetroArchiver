#include <stdlib.h>
#include "unit.h"
#include "core/archive.h"
#include "core/bagit.h"
#include "core/fsu8.h"
#include "core/sha256.h"

#define ROOT "tmp_unit/archivio"

static void put(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* a disk with its image in data/ and the fingerprint taken "after the read" */
static void disk(meta_box *b, const char *bag, int n, const char *status, const char *at)
{
    meta_disk *d = meta_disk_get(b, n);
    char p[600];
    str_copy(d->status, sizeof(d->status), status);
    if (!strcmp(status, "missing")) return;
    snprintf(d->master, sizeof(d->master), "DISK%02d.dsk", n);
    d->nreads = 1;
    str_copy(d->reads[0].file, sizeof(d->reads[0].file), d->master);
    str_copy(d->reads[0].at, sizeof(d->reads[0].at), at);
    snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
    put(p, status);                         /* the content does not matter, the size does */
    sha256_file(p, d->reads[0].sha256, &d->reads[0].bytes);
}

static meta_box *box(const char *title, const char *platform, int expected)
{
    meta_box *b = meta_new();
    str_copy(b->title, sizeof(b->title), title);
    str_copy(b->platform, sizeof(b->platform), platform);
    str_copy(b->started, sizeof(b->started), "2026-10-10T10:00:00");
    b->media_expected = expected;
    return b;
}

/* the package as the session leaves it: closed if closed is not NULL */
static void bag(meta_box *b, const char *dir, const char *closed)
{
    strbuf err; int bad;
    sb_init(&err);
    if (closed) str_copy(b->closed, sizeof(b->closed), closed);
    {
        char data[600];
        snprintf(data, sizeof(data), "%s/data", dir);
        CHECK(meta_save(b, data, NULL, NULL, &err) == 0);
    }
    if (closed) CHECK(bag_close(dir, b, NULL, &err, &bad) == 0);
    sb_free(&err);
    meta_delete(b);
}

static void make_archive(void)
{
    strbuf err; meta_box *b;
    sb_init(&err);
    u8_mkdirs(ROOT);

    CHECK(bag_create(ROOT "/Completo", &err) == 0);
    b = box("Completo", "MS-DOS", 2);
    str_copy(b->publisher, sizeof(b->publisher), "Lucasfilm Games");
    str_copy(b->year, sizeof(b->year), "1990");
    disk(b, ROOT "/Completo", 1, "clean", "2026-10-12T10:00:00");
    disk(b, ROOT "/Completo", 2, "clean", "2026-10-12T10:05:00");
    bag(b, ROOT "/Completo", "2026-10-12T10:06:00");

    CHECK(bag_create(ROOT "/Incompleto", &err) == 0);
    b = box("Incompleto", "Windows 3.x", 3);
    disk(b, ROOT "/Incompleto", 1, "clean", "2026-10-13T10:00:00");
    disk(b, ROOT "/Incompleto", 2, "error", "2026-10-13T10:05:00");
    disk(b, ROOT "/Incompleto", 3, "missing", "");
    bag(b, ROOT "/Incompleto", "2026-10-13T11:00:00");

    CHECK(bag_create(ROOT "/Aperto", &err) == 0);
    b = box("Aperto", "MS-DOS", 4);
    disk(b, ROOT "/Aperto", 1, "clean", "2026-10-14T09:00:00");
    disk(b, ROOT "/Aperto", 2, "marginal", "2026-10-14T09:30:00");
    bag(b, ROOT "/Aperto", NULL);

    /* mixed: three floppies done, the CD still to read */
    CHECK(bag_create(ROOT "/Misto", &err) == 0);
    b = box("Misto", "Windows NT", 0);
    meta_set_counts(b, 3, 1, 0);
    disk(b, ROOT "/Misto", 1, "clean", "2026-10-15T09:00:00");
    disk(b, ROOT "/Misto", 2, "clean", "2026-10-15T09:05:00");
    disk(b, ROOT "/Misto", 3, "marginal", "2026-10-15T09:10:00");
    bag(b, ROOT "/Misto", NULL);

    /* opened by an earlier version: retroarchiver-floppy-1 */
    CHECK(bag_create(ROOT "/Vecchio", &err) == 0);
    put(ROOT "/Vecchio/data/metadata.json",
        "{\"schema\": \"retroarchiver-floppy-1\",\n"
        " \"box\": {\"title\": \"Vecchio\", \"platform\": \"MS-DOS\", \"media_type\": \"floppy\",\n"
        "         \"media_expected\": 2, \"started\": \"2026-10-11T08:00:00\", \"closed\": null},\n"
        " \"media\": [{\"n\": 1, \"label\": \"\", \"note\": \"\", \"status\": \"pulito\",\n"
        "            \"master\": \"DISK001.dsk\", \"scan\": null, \"compare\": null,\n"
        "            \"reads\": [{\"file\": \"DISK001.dsk\", \"bytes\": 6, \"sha256\": \"x\",\n"
        "                       \"at\": \"2026-10-11T08:10:00\"}]}]}\n");

    /* open but without metadata.json: the first save did not succeed */
    CHECK(bag_create(ROOT "/Rotto", &err) == 0);
    /* any folder and a file: they are not boxes */
    u8_mkdir(ROOT "/altro");
    put(ROOT "/leggimi.txt", "x");
    sb_free(&err);
}

static const arc_box *find(const arc_list *l, const char *title)
{
    int i;
    for (i = 0; i < l->n; i++) if (!strcmp(l->box[i].title, title)) return &l->box[i];
    return NULL;
}

static void check_list(void)
{
    arc_list l; const arc_box *b; char t[400];

    CHECK(arc_scan(ROOT "/non-esiste", &l) == -1);
    CHECK(arc_scan(ROOT, &l) == 0);
    CHECK(l.n == 6);
    /* most recent first: the mixed one was read on the 15th, the broken one has no date */
    CHECK(l.n == 6 && !strcmp(l.box[0].title, "Misto"));
    CHECK(l.n == 6 && !strcmp(l.box[1].title, "Aperto"));
    CHECK(l.n == 6 && !strcmp(l.box[2].title, "Incompleto"));
    CHECK(l.n == 6 && !strcmp(l.box[3].title, "Completo"));
    CHECK(l.n == 6 && !strcmp(l.box[4].title, "Vecchio"));

    b = find(&l, "Completo");
    CHECK(b && b->state == ARC_COMPLETE);
    CHECK(b && b->expected == 2 && b->archived == 2 && b->clean == 2);
    CHECK(b && !strcmp(b->disks, "pp") && b->first_todo == 0);
    CHECK(b && !strcmp(b->kinds, "ff") && b->count[MEDIA_FLOPPY] == 2);
    if (b) { arc_media(b, t, sizeof(t)); CHECK_STR(t, "2 / 2 floppy"); }
    CHECK(b && b->bytes == 10 + (long long)u8_filesize(ROOT "/Completo/data/metadata.json"));
    CHECK(b && !strcmp(b->date, "2026-10-12T10:06:00"));
    CHECK(b && !strcmp(b->publisher, "Lucasfilm Games") && !strcmp(b->year, "1990"));
    CHECK(b && arc_tone(b) == TONE_OK);
    if (b) { arc_outcome(b, t, sizeof(t)); CHECK_STR(t, "2 clean"); }
    if (b) { arc_advice(b, t, sizeof(t)); CHECK_STR(t, "Closed complete: all floppies read and verified."); }

    b = find(&l, "Incompleto");
    CHECK(b && b->state == ARC_INCOMPLETE);
    CHECK(b && !strcmp(b->disks, "pe-") && b->archived == 2 && b->errors == 1);
    CHECK(b && arc_tone(b) == TONE_BAD);
    if (b) { arc_outcome(b, t, sizeof(t)); CHECK_STR(t, "1 clean, 1 with errors"); }
    if (b) { arc_missing(b, t, sizeof(t)); CHECK_STR(t, "floppy 3"); }
    if (b) {
        arc_advice(b, t, sizeof(t));
        CHECK_STR(t, "Closed incomplete: floppy 3 is missing. Package closed and verified. "
                     "Floppy 2 has damaged data: keep it to try again with another drive.");
    }

    b = find(&l, "Aperto");
    CHECK(b && b->state == ARC_OPEN);
    CHECK(b && !strcmp(b->disks, "pm--") && b->first_todo == 3);
    CHECK(b && !strcmp(b->date, "2026-10-14T09:30:00"));
    CHECK(b && arc_tone(b) == TONE_OPEN);
    if (b) { arc_outcome(b, t, sizeof(t)); CHECK_STR(t, "1 clean, 1 marginal"); }
    if (b) { arc_missing(b, t, sizeof(t)); CHECK_STR(t, "floppy 3, 4"); }
    if (b) { arc_advice(b, t, sizeof(t)); CHECK_STR(t, "Open: 2 floppies out of 4 archived. Resume from floppy 3."); }

    b = find(&l, "Misto");
    CHECK(b && b->state == ARC_OPEN && b->expected == 4 && b->archived == 3);
    CHECK(b && !strcmp(b->disks, "ppm-") && !strcmp(b->kinds, "fffc") && b->first_todo == 4);
    CHECK(b && b->count[MEDIA_CD] == 1 && b->kind_archived[MEDIA_FLOPPY] == 3 && b->kind_archived[MEDIA_CD] == 0);
    if (b) { arc_media(b, t, sizeof(t)); CHECK_STR(t, "3 / 3 floppy, 0 / 1 CD"); }
    if (b) { arc_missing(b, t, sizeof(t)); CHECK_STR(t, "CD 1"); }
    if (b) {
        arc_advice(b, t, sizeof(t));
        CHECK_STR(t, "Open: 3 floppies out of 3 archived, CD 1 is missing. "
                     "Resume it when reading CDs is available.");
    }

    b = find(&l, "Vecchio");
    CHECK(b && b->state == ARC_OPEN && b->expected == 2 && b->count[MEDIA_FLOPPY] == 2);
    CHECK(b && !strcmp(b->disks, "p-") && !strcmp(b->kinds, "ff"));
    if (b) { arc_media(b, t, sizeof(t)); CHECK_STR(t, "1 / 2 floppy"); }
    if (b) { arc_advice(b, t, sizeof(t)); CHECK_STR(t, "Open: 1 floppy out of 2 archived. Resume from floppy 2."); }

    b = find(&l, "Rotto");
    CHECK(b && b->state == ARC_BROKEN && b->error[0]);
    CHECK(b && arc_tone(b) == TONE_BAD);
    if (b) { arc_advice(b, t, sizeof(t)); CHECK(!strncmp(t, "Cannot read this box: ", 22)); }
    arc_free(&l);
    CHECK(l.n == 0 && l.box == NULL);
}

/* texts with plurals and edge cases, on hand-written summaries */
static void check_texts(void)
{
    arc_box b; char t[400];
    memset(&b, 0, sizeof(b));
    b.state = ARC_OPEN; b.expected = 9;
    strcpy(b.disks, "---------"); b.first_todo = 1;
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Open: no floppy archived yet. Resume from floppy 1.");
    arc_outcome(&b, t, sizeof(t));
    CHECK_STR(t, "no media read");
    strcpy(b.disks, "p--------"); b.clean = 1; b.first_todo = 2;
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Open: 1 floppy out of 9 archived. Resume from floppy 2.");
    strcpy(b.disks, "ppppppppp"); b.clean = 9; b.first_todo = 0;
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Open: all 9 floppies archived, only the closing is left. Resume to close it.");

    memset(&b, 0, sizeof(b));
    b.state = ARC_INCOMPLETE; b.expected = 7;
    strcpy(b.disks, "pppp-p-"); b.clean = 5;
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Closed incomplete: floppy 5, 7 are missing. Package closed and verified.");
    CHECK(arc_tone(&b) == TONE_WARN);

    /* mixed: the names give the type, the box is resumable once the read exists */
    memset(&b, 0, sizeof(b));
    b.state = ARC_INCOMPLETE; b.expected = 4;
    b.count[MEDIA_FLOPPY] = 2; b.count[MEDIA_CD] = 1; b.count[MEDIA_DVD] = 1;
    strcpy(b.disks, "p-p-"); b.clean = 2;
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Closed incomplete: floppy 2, DVD 1 are missing. Package closed and verified.");
    b.state = ARC_OPEN; strcpy(b.disks, "pp--"); b.first_todo = 3;
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Open: 2 floppies out of 2 archived, CD 1, DVD 1 are missing. "
                 "Resume it when reading CDs and DVDs is available.");
    b.kind_archived[MEDIA_FLOPPY] = 2;
    arc_media(&b, t, sizeof(t));
    CHECK_STR(t, "2 / 2 floppy, 0 / 1 CD, 0 / 1 DVD");
    strcpy(b.disks, "pppp"); b.clean = 4; b.first_todo = 0;
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Open: all 4 media archived, only the closing is left. Resume to close it.");

    memset(&b, 0, sizeof(b));
    b.state = ARC_COMPLETE; b.expected = 4;
    strcpy(b.disks, "pmmf"); b.clean = 1; b.marginal = 2; b.fragile = 1;
    CHECK(arc_tone(&b) == TONE_WARN);
    arc_outcome(&b, t, sizeof(t));
    CHECK_STR(t, "1 clean, 2 marginal, 1 fragile");
    arc_advice(&b, t, sizeof(t));
    CHECK_STR(t, "Closed complete. Floppy 2, 3 are marginal: the reads did not fully match. "
                 "Floppy 4 is fragile: only one cautious read.");
    strcpy(b.disks, "eeff"); b.clean = b.marginal = 0; b.errors = 2; b.fragile = 2;
    arc_outcome(&b, t, sizeof(t));
    CHECK_STR(t, "2 fragile, 2 with errors");
}

static void check_verify(void)
{
    arc_check c; strbuf err;
    sb_init(&err);

    CHECK(arc_verify(ROOT "/Completo", &c, &err) == 0);
    /* 2 images + metadata.json, plus the 3 tag files */
    CHECK(c.files == 6 && c.changed == 0 && c.lost == 0 && c.extra == 0 && !c.first[0]);

    put(ROOT "/Completo/data/DISK02.dsk", "puliTo");
    put(ROOT "/Completo/data/avanzo.tmp", "x");
    CHECK(arc_verify(ROOT "/Completo", &c, &err) == 0);
    CHECK(c.changed == 1 && c.lost == 0 && c.extra == 1);
    CHECK_STR(c.first, "data/DISK02.dsk");
    u8_remove(ROOT "/Completo/data/DISK01.dsk");
    CHECK(arc_verify(ROOT "/Completo", &c, &err) == 0);
    CHECK(c.lost == 1 && c.changed == 1);
    CHECK_STR(c.first, "data/DISK01.dsk");

    /* open: the fingerprints of metadata.json */
    CHECK(arc_verify(ROOT "/Aperto", &c, &err) == 0);
    CHECK(c.files == 2 && c.changed == 0 && c.lost == 0);
    put(ROOT "/Aperto/data/DISK01.dsk", "rovinato");
    CHECK(arc_verify(ROOT "/Aperto", &c, &err) == 0);
    CHECK(c.changed == 1);
    CHECK_STR(c.first, "data/DISK01.dsk");

    /* without a manifest and without metadata.json it is not possible */
    u8_remove(ROOT "/Incompleto/manifest-sha256.txt");
    sb_clear(&err);
    CHECK(arc_verify(ROOT "/Incompleto", &c, &err) == -1);
    CHECK(strstr(err.s, "manifest-sha256.txt") != NULL);
    sb_clear(&err);
    CHECK(arc_verify(ROOT "/Rotto", &c, &err) == -1);
    CHECK(err.len > 0);
    sb_free(&err);
}

/* the good digests of a closed bag, and the flags of a box that cannot be edited */
static void check_digests(void)
{
    arc_check c; strbuf err; bag_digests ok; arc_box a; meta_box *b;
    memset(&ok, 0, sizeof(ok));
    sb_init(&err);
    u8_mkdirs(ROOT "2");
    CHECK(bag_create(ROOT "2/Due", &err) == 0);
    b = box("Due", "MS-DOS", 2);
    disk(b, ROOT "2/Due", 1, "clean", "2026-10-12T10:00:00");
    disk(b, ROOT "2/Due", 2, "fragile", "2026-10-12T10:05:00");
    bag(b, ROOT "2/Due", "2026-10-12T10:06:00");
    put(ROOT "2/Due/data/DISK02.dsk", "changed");
    CHECK(arc_verify_digests(ROOT "2/Due", &c, &ok, &err) == 0);
    CHECK(c.changed == 1);
    /* DISK01.dsk and metadata.json are good */
    CHECK(ok.n == 2);
    CHECK(bag_digests_find(&ok, "data/DISK01.dsk") >= 0);
    CHECK(bag_digests_find(&ok, "data/DISK02.dsk") == -1);
    CHECK(arc_load(ROOT "2/Due", &a) && !a.editing && !a.old_format);
    u8_mkdir(ROOT "2/Due/editing");
    CHECK(arc_load(ROOT "2/Due", &a) && a.editing);
    u8_mkdir(ROOT "2/Due/messi-da-parte");
    CHECK(arc_load(ROOT "2/Due", &a) && a.old_format);
    bag_digests_free(&ok);
    sb_free(&err);
}

void test_archive(void)
{
    arc_list l;
    u8_mkdir("tmp_unit");
    make_archive();
    check_list();
    check_texts();
    check_verify();
    check_digests();
    /* a new, empty archive: no boxes, but no error */
    u8_mkdirs("tmp_unit/archivio_vuoto");
    CHECK(arc_scan("tmp_unit/archivio_vuoto", &l) == 0 && l.n == 0);
    arc_free(&l);
}
