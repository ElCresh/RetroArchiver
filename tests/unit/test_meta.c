#include <stdlib.h>
#include "unit.h"
#include "core/meta.h"
#include "core/fsu8.h"

static void fake_tech(void *ctx, const meta_disk *d, const char *master, json_writer *w)
{
    (void)ctx; (void)d;
    jw_begin_obj(w, "geometry");
    jw_str(w, "format", strstr(master, "disco01.dsk") ? "1.44M" : "?");
    jw_end_obj(w);
}

static char *slurp_test(const char *path)
{
    static char buf[16384]; FILE *f = u8_fopen(path, "rb"); size_t n;
    if (!f) return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    return buf;
}

static void put_text(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* mixed box: composition, type and number of each medium, names */
static void test_meta_mixed(void)
{
    meta_box *b = meta_new(), *c = meta_new();
    strbuf err; char t[80]; int kind, num; const char *text;
    sb_init(&err);

    CHECK_STR(meta_kind_key(MEDIA_CD), "cd");
    CHECK_STR(meta_kind_label(MEDIA_DVD), "DVD");
    CHECK(meta_kind_parse("dvd") == MEDIA_DVD && meta_kind_parse("zip") == -1);
    meta_disk_name(MEDIA_CD, 1, t, sizeof(t));
    CHECK_STR(t, "CD 1");

    str_copy(b->title, sizeof(b->title), "Windows NT Workstation 4.0");
    str_copy(b->platform, sizeof(b->platform), "Windows NT");
    meta_set_counts(b, 3, 1, 2);
    CHECK(b->media_expected == 6);
    meta_composition(b, t, sizeof(t));
    CHECK_STR(t, "3 floppy, 1 CD, 2 DVD");
    meta_slot(b, 3, &kind, &num);
    CHECK(kind == MEDIA_FLOPPY && num == 3);
    meta_slot(b, 4, &kind, &num);
    CHECK(kind == MEDIA_CD && num == 1);
    meta_slot(b, 6, &kind, &num);
    CHECK(kind == MEDIA_DVD && num == 2);
    CHECK(meta_disk_get(b, 5)->kind == MEDIA_DVD && meta_disk_get(b, 5)->num == 1);
    str_copy(meta_disk_get(b, 4)->label, sizeof(b->disk[0].label), "NTWKS40_IT");
    str_copy(b->names[MEDIA_FLOPPY], sizeof(b->names[0]), "FLOPPY{nn}");
    str_copy(b->names[MEDIA_CD], sizeof(b->names[0]), "CD{nn}");

    u8_mkdirs("tmp_unit/meta_misto");
    CHECK(meta_save(b, "tmp_unit/meta_misto", NULL, NULL, &err) == 0);
    text = slurp_test("tmp_unit/meta_misto/metadata.json");
    CHECK(text && strstr(text, "\"type\": \"cd\",\n        \"count\": 1") != NULL);
    CHECK(text && strstr(text, "\"cd\": \"CD{nn}\"") != NULL);
    CHECK(text && strstr(text, "\"type\": \"dvd\",\n      \"number\": 1") != NULL);

    CHECK(meta_load(c, "tmp_unit/meta_misto", &err) == 0);
    CHECK(c->count[MEDIA_FLOPPY] == 3 && c->count[MEDIA_CD] == 1 && c->count[MEDIA_DVD] == 2);
    CHECK(c->media_expected == 6);
    CHECK_STR(c->names[MEDIA_CD], "CD{nn}");
    CHECK_STR(c->names[MEDIA_DVD], "");
    CHECK(c->ndisk == 2 && c->disk[0].kind == MEDIA_CD && c->disk[0].num == 1);
    CHECK_STR(c->disk[0].label, "NTWKS40_IT");
    CHECK(c->disk[1].kind == MEDIA_DVD && c->disk[1].num == 1);

    /* the copy protection scan: the box's choice, and a disk forced on */
    CHECK_STR(meta_prot_key(META_PROT_OFF), "off");
    CHECK(meta_prot_parse("on") == META_PROT_ON && meta_prot_parse("forse") == -1 && meta_prot_parse(NULL) == -1);
    b->copy_protection = META_PROT_OFF;
    meta_disk_get(b, 4)->copy_protection = META_PROT_ON;
    meta_disk_get(b, 5)->copy_protection = META_PROT_OFF;
    CHECK(meta_save(b, "tmp_unit/meta_misto", NULL, NULL, &err) == 0);
    text = slurp_test("tmp_unit/meta_misto/metadata.json");
    CHECK(text && strstr(text, "\"copy_protection_scan\": \"off\"") != NULL);
    CHECK(meta_load(c, "tmp_unit/meta_misto", &err) == 0);
    CHECK(c->copy_protection == META_PROT_OFF);
    CHECK(c->disk[0].copy_protection == META_PROT_ON && c->disk[1].copy_protection == META_PROT_OFF);

    /* a schema it does not know: it comes from a newer version */
    put_text("tmp_unit/meta_misto/metadata.json", "{\"schema\": \"retroarchiver-4\"}");
    sb_clear(&err);
    CHECK(meta_load(c, "tmp_unit/meta_misto", &err) == -1);
    CHECK(strstr(err.s, "newer version") != NULL);

    meta_delete(b); meta_delete(c);
    sb_free(&err);
}

/* a metadata.json written before mixed media is still readable: all floppies */
static void test_meta_old(void)
{
    meta_box *b = meta_new(); strbuf err; char t[80];
    sb_init(&err);
    u8_mkdirs("tmp_unit/meta_vecchio");
    put_text("tmp_unit/meta_vecchio/metadata.json",
        "{\n  \"schema\": \"retroarchiver-floppy-1\",\n"
        "  \"box\": {\"title\": \"Ultima VII\", \"publisher\": null, \"year\": null,\n"
        "    \"platform\": \"MS-DOS\", \"language\": null, \"edition\": null, \"notes\": null,\n"
        "    \"media_type\": \"floppy\", \"media_expected\": 5,\n"
        "    \"started\": \"2026-10-14T09:00:00\", \"closed\": null},\n"
        "  \"software\": {\"retroarchiver\": \"1.0\", \"samdisk\": null},\n"
        "  \"media\": [\n"
        "    {\"n\": 1, \"label\": \"Disk 1\", \"note\": \"\", \"status\": \"pulito\",\n"
        "     \"master\": \"DISK001.dsk\", \"scan\": \"DISK001.scan.txt\",\n"
        "     \"reads\": [{\"file\": \"DISK001.dsk\", \"bytes\": 3, \"sha256\": \"x\", \"at\": \"2026-10-14T09:00:00\"}],\n"
        "     \"compare\": null},\n"
        "    {\"n\": 2, \"label\": \"\", \"note\": \"\", \"status\": \"da leggere\",\n"
        "     \"master\": null, \"scan\": null, \"reads\": [], \"compare\": null}\n"
        "  ]\n}\n");
    b->copy_protection = META_PROT_ON;
    CHECK(meta_load(b, "tmp_unit/meta_vecchio", &err) == 0);
    CHECK(b->media_expected == 5 && b->count[MEDIA_FLOPPY] == 5 && b->count[MEDIA_CD] == 0);
    CHECK(b->copy_protection == META_PROT_AUTO);        /* before the choice existed: auto */
    meta_composition(b, t, sizeof(t));
    CHECK_STR(t, "5 floppy");
    CHECK(b->ndisk == 2 && b->disk[0].kind == MEDIA_FLOPPY && b->disk[0].num == 1);
    CHECK(b->disk[1].kind == MEDIA_FLOPPY && b->disk[1].num == 2);
    CHECK_STR(b->disk[0].master, "DISK001.dsk");
    /* the outcomes of 261017 in Italian are read in English; "da leggere" is "" */
    CHECK_STR(b->disk[0].status, ST_CLEAN);
    CHECK_STR(b->disk[1].status, "");
    CHECK_STR(b->names[MEDIA_FLOPPY], "");     /* the session derives it from the files */
    meta_delete(b);
    sb_free(&err);
}

/* the history survives a save and a load, in order; a full one refuses */
static void history_roundtrip(void)
{
    meta_box *b = meta_new(), *c = meta_new(); strbuf err; char *big; size_t n;
    sb_init(&err);
    str_copy(b->title, sizeof(b->title), "H");
    str_copy(b->platform, sizeof(b->platform), "X");
    b->media_expected = 1;
    CHECK(meta_history_add(b, "{\"action\": \"edit\", \"n\": 1}") == 0);
    CHECK(meta_history_add(b, "{\"action\": \"edit\", \"n\": 2}") == 0);
    u8_mkdirs("tmp_unit/hist");
    CHECK(meta_save(b, "tmp_unit/hist", NULL, NULL, &err) == 0);
    CHECK(meta_load(c, "tmp_unit/hist", &err) == 0);
    CHECK_STR(c->schema, "retroarchiver-3");
    {
        char e[160]; jval *h = json_parse(c->history, e, sizeof(e));
        CHECK(h && h->t == J_ARR && h->child && json_int(h->child, "n", 0) == 1
              && h->child->next && json_int(h->child->next, "n", 0) == 2);
        json_free(h);
    }
    n = sizeof(b->history);
    big = (char *)malloc(n + 16);
    memset(big, ' ', n); memcpy(big, "{\"x\": 1", 7); strcpy(big + n - 2, "}");
    {
        char before[64];
        str_copy(before, sizeof(before), b->history);
        CHECK(meta_history_add(b, big) == -1);
        CHECK(!strncmp(b->history, before, strlen(before)));
    }
    free(big);
    meta_delete(b); meta_delete(c);
    sb_free(&err);
}

void test_meta(void)
{
    meta_box *b = meta_new(), *c = meta_new();
    meta_disk *d; strbuf err; char now[20]; const char *text;

    sb_init(&err);
    str_copy(b->title, sizeof(b->title), "Citt\xc3\xa0 perduta");
    str_copy(b->platform, sizeof(b->platform), "MS-DOS");
    b->media_expected = 3;
    str_copy(b->started, sizeof(b->started), "2026-10-15T21:04:00");

    d = meta_disk_get(b, 3);
    str_copy(d->status, sizeof(d->status), "missing");
    d = meta_disk_get(b, 1);
    CHECK(b->ndisk == 2 && b->disk[0].n == 1 && b->disk[1].n == 3);
    CHECK(meta_disk_get(b, 1) == d);
    CHECK(meta_disk_get(b, 0) == NULL);
    str_copy(d->label, sizeof(d->label), "Disk 1");
    str_copy(d->status, sizeof(d->status), "marginal");
    str_copy(d->master, sizeof(d->master), "disco01.dsk");
    d->nreads = 2;
    str_copy(d->reads[0].file, sizeof(d->reads[0].file), "disco01.dsk");
    d->reads[0].bytes = 1515776;
    str_copy(d->reads[0].sha256, sizeof(d->reads[0].sha256),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    str_copy(d->reads[1].file, sizeof(d->reads[1].file), "disco01_b.dsk");
    str_copy(d->verdict, sizeof(d->verdict), "data_differ");
    d->agree = 1; d->unstable_tracks = 2;
    CHECK(meta_disk_done(d));
    CHECK(!meta_disk_done(meta_disk_get(b, 3)));

    meta_now(now);
    CHECK(strlen(now) == 19 && now[10] == 'T');

    u8_mkdir("tmp_unit");
    u8_mkdir("tmp_unit/meta");
    CHECK(meta_save(b, "tmp_unit/meta", fake_tech, NULL, &err) == 0);
    CHECK(!u8_exists("tmp_unit/meta/metadata.json.tmp"));
    text = slurp_test("tmp_unit/meta/metadata.json");
    CHECK(text != NULL);
    if (text) {
        CHECK(strstr(text, "\"schema\": \"retroarchiver-3\"") != NULL);
        /* media_expected only: all floppies */
        CHECK(strstr(text, "\"type\": \"floppy\",\n        \"count\": 3") != NULL);
        CHECK(strstr(text, "\"format\": \"1.44M\"") != NULL);
        CHECK(strstr(text, "\"closed\": null") != NULL);
    }

    CHECK(meta_load(c, "tmp_unit/meta", &err) == 0);
    CHECK_STR(c->title, "Citt\xc3\xa0 perduta");
    CHECK(c->media_expected == 3);
    CHECK(c->ndisk == 2);
    CHECK_STR(c->disk[0].label, "Disk 1");
    CHECK(c->disk[0].nreads == 2);
    CHECK(c->disk[0].reads[0].bytes == 1515776);
    CHECK_STR(c->disk[0].verdict, "data_differ");
    CHECK(c->disk[0].unstable_tracks == 2);
    CHECK_STR(c->disk[1].status, "missing");
    CHECK_STR(c->closed, "");

    /* destination that does not exist: clear error, no crash */
    sb_clear(&err);
    CHECK(meta_save(b, "tmp_unit/non/esiste", NULL, NULL, &err) == -1);
    CHECK(strstr(err.s, "full or write-protected") != NULL);
    sb_clear(&err);
    CHECK(meta_load(c, "tmp_unit/non/esiste", &err) == -1);
    CHECK(strstr(err.s, "cannot find") != NULL);
    CHECK(strstr(err.s, "move its folder away") != NULL);
    /* damaged: the message also says what to do */
    {
        FILE *f = u8_fopen("tmp_unit/meta/metadata.json", "wb");
        if (f) { fputs("{ rotto", f); fclose(f); }
    }
    sb_clear(&err);
    CHECK(meta_load(c, "tmp_unit/meta", &err) == -1);
    CHECK(strstr(err.s, "cannot be read") != NULL && strstr(err.s, "move its folder away") != NULL);

    meta_delete(b); meta_delete(c);
    sb_free(&err);
    test_meta_mixed();
    test_meta_old();
    history_roundtrip();
}
