#include <stdlib.h>
#include "unit.h"
#include "core/media.h"
#include "core/fsu8.h"
#include "core/iso9660.h"
#include "core/sha256.h"
#include "core/archive.h"

static strbuf said;
static void keep(void *ctx, const char *line) { (void)ctx; sb_puts(&said, line); sb_puts(&said, "\n"); }

static int nfiles;
static void count_rel(void *ctx, const char *rel) { (void)ctx; (void)rel; nfiles++; }

static int has_file(const iso_info *fi, const char *path, long long size, int isdir)
{
    int i;
    for (i = 0; i < fi->nfiles; i++)
        if (!strcmp(fi->files[i].path, path)) return fi->files[i].isdir == isdir && (isdir || fi->files[i].size == size);
    return 0;
}

static void test_iso(void)
{
    iso_info fi;
    CHECK(iso_read("fixtures/iso/prova.iso", 2048, 0, &fi) == 1);
    CHECK_STR(fi.volume_label, "PROVA_CD");
    CHECK_STR(fi.created, "1996-05-01T10:20:00");
    CHECK_STR(fi.publisher, "EDITORE DI PROVA");
    CHECK(fi.nfiles == 5);
    CHECK(has_file(&fi, "LEGGIMI.TXT", 35, 0));
    CHECK(has_file(&fi, "GIOCO", 0, 1));
    CHECK(has_file(&fi, "GIOCO/DATI/MONDO.DAT", 5000, 0));
    CHECK(fi.nfiles > 0 && !strcmp(fi.files[0].path, "GIOCO"));    /* in order */
    CHECK(fi.nfiles > 0 && !strcmp(fi.files[0].date, "1996-05-01T10:20:00"));
    iso_free(&fi);
    /* the same image inside the raw data track of a CD */
    CHECK(iso_read("fixtures/dic/cd_good/DISC (Track 1).bin", 2352, 16, &fi) == 1);
    CHECK_STR(fi.volume_label, "PROVA_CD");
    CHECK(has_file(&fi, "INSTALL.BAT", 38, 0));
    iso_free(&fi);
    /* an audio track has no file system */
    CHECK(iso_read("fixtures/dic/cd_good/DISC (Track 2).bin", 2352, 16, &fi) == 0);
    CHECK(strstr(fi.note, "no ISO 9660 file system") != NULL);
    CHECK(iso_read("fixtures/non-esiste.iso", 2048, 0, &fi) == 0);
    CHECK(iso_read("fixtures/iso/prova.iso", 2048, 0, &fi) == 1 && fi.joliet == 0);
    iso_free(&fi);

    /* with Joliet: long names and label in UTF-8 */
    CHECK(iso_read("fixtures/iso/joliet.iso", 2048, 0, &fi) == 1);
    CHECK(fi.joliet == 1);
    CHECK_STR(fi.volume_label, "Prova citt\xc3\xa0");
    CHECK(fi.nfiles == 5);
    CHECK(has_file(&fi, "Leggimi prima di installare.txt", 35, 0));
    CHECK(has_file(&fi, "Installa il gioco.bat", 38, 0));
    CHECK(has_file(&fi, "Gioco completo", 0, 1));
    CHECK(has_file(&fi, "Gioco completo/Dati del mondo", 0, 1));
    CHECK(has_file(&fi, "Gioco completo/Dati del mondo/Citt\xc3\xa0 del mondo \xe2\x80\x93 versione italiana.dat",
                   5000, 0));
    CHECK(fi.nfiles > 0 && !strcmp(fi.files[0].date, "1996-05-01T10:20:00"));
    CHECK_STR(fi.publisher, "EDITORE DI PROVA");        /* the rest from the primary */
    iso_free(&fi);

    {   /* unreadable Joliet (image cut before its folders): 8.3 names */
        FILE *in = u8_fopen("fixtures/iso/joliet.iso", "rb"), *out;
        static unsigned char buf[30 * 2048];
        size_t got = in ? fread(buf, 1, sizeof(buf), in) : 0;
        if (in) fclose(in);
        u8_mkdir("tmp_unit");
        out = u8_fopen("tmp_unit/joliet_rotto.iso", "wb");
        if (out) { fwrite(buf, 1, got, out); fclose(out); }
        CHECK(got == sizeof(buf));
        CHECK(iso_read("tmp_unit/joliet_rotto.iso", 2048, 0, &fi) == 1);
        CHECK(fi.joliet == 0 && fi.nfiles == 5 && fi.note[0] == 0);
        CHECK_STR(fi.volume_label, "PROVA_CD");
        CHECK(has_file(&fi, "GIOCO/DATI/MONDO.DAT", 5000, 0));
        iso_free(&fi);
    }
}

static void test_tree(void)
{
    FILE *f; char a[65], b[65]; long long n;
    u8_mkdirs("tmp_unit/albero/sotto");
    if ((f = fopen("tmp_unit/albero/uno.txt", "w")) != NULL) { fputs("uno", f); fclose(f); }
    if ((f = fopen("tmp_unit/albero/sotto/due.txt", "w")) != NULL) { fputs("due!", f); fclose(f); }
    nfiles = 0;
    CHECK(u8_walk("tmp_unit/albero", count_rel, NULL) == 0 && nfiles == 2);
    CHECK(u8_treesize("tmp_unit/albero") == 7);
    CHECK(sha256_path("tmp_unit/albero", a, &n) == 0 && n == 7);
    /* a folder: fingerprint of the "fingerprint  path" lines in order */
    {
        sha256_ctx c; unsigned char d[32]; char h1[65], h2[65];
        sha256_file("tmp_unit/albero/sotto/due.txt", h1, NULL);
        sha256_file("tmp_unit/albero/uno.txt", h2, NULL);
        sha256_init(&c);
        sha256_tree_line(&c, h1, "sotto/due.txt");
        sha256_tree_line(&c, h2, "uno.txt");
        sha256_final(&c, d);
        sha256_hex(d, b);
        CHECK_STR(a, b);
    }
    /* a changed file changes the folder fingerprint */
    if ((f = fopen("tmp_unit/albero/sotto/due.txt", "w")) != NULL) { fputs("tre!", f); fclose(f); }
    CHECK(sha256_path("tmp_unit/albero", b, NULL) == 0 && strcmp(a, b));
    CHECK(sha256_path("tmp_unit/albero/uno.txt", a, NULL) == 0);
    sha256_file("tmp_unit/albero/uno.txt", b, NULL);
    CHECK_STR(a, b);
    CHECK(u8_rmtree("tmp_unit/albero") == 0 && !u8_exists("tmp_unit/albero"));
}

static void test_progress(void)
{
    long a = 0, b = 0;
    CHECK(optical_progress("Creating .scm (LBA)   1234/ 300000", &a, &b) && a == 1234 && b == 300000);
    CHECK(optical_progress("Checking sectors:    420/   420", &a, &b) && a == 420 && b == 420);
    CHECK(!optical_progress("Remains 3 c2 error. Rereading times:   20/  20 extra", NULL, NULL));
    CHECK(!optical_progress("No C2 errors", NULL, NULL));
    CHECK(cd_media.phase_of("In the process of correction (c2)") != NULL);
    CHECK(cd_media.phase_of("No C2 errors") != NULL);
    CHECK(cd_media.phase_of("/c2 val2 was omitted. set [8]") == NULL);
    CHECK(cd_media.phase_of("Creating .scm (LBA)   1234/ 300000") == NULL);
}

static const char *jread(json_writer *w, const media_ops *m, const char *dir)
{
    jw_free(w);
    jw_init(w);
    jw_begin_obj(w, NULL);
    m->tech(NULL, dir, w);
    jw_end_obj(w);
    return w->out.s;
}

static int cmp(const media_ops *m, const char *a, const char *b, const char *c, compare_result *r)
{
    const char *imgs[3]; int n = 0;
    imgs[n++] = a;
    if (b) imgs[n++] = b;
    if (c) imgs[n++] = c;
    compare_free(r);
    compare_init(r);
    return m->compare(imgs, n, 0, r);
}

static void touch(const char *path)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) fclose(f);
}

static int count_of(const char *s, const char *what)
{
    int n = 0;
    while ((s = strstr(s, what)) != NULL) { n++; s += strlen(what); }
    return n;
}

/* the copy protection signature files among the disc's files */
static void test_signatures(void)
{
    char out[600], out2[600], line[200], pat[100], path[400]; FILE *f; int n = 0;

    optical_set_signatures("tmp_unit/firme-che-non-esistono.txt");      /* the built-in list */
    u8_mkdirs("tmp_unit/disco/GIOCO/DATI");
    touch("tmp_unit/disco/LEGGIMI.TXT");
    CHECK(optical_protection_files("tmp_unit/disco", out, sizeof(out)) == 0 && !out[0]);
    CHECK(optical_protection_files("tmp_unit/disco-che-non-esiste", out, sizeof(out)) == -1);
    /* in subfolders, any case, endings and folders */
    touch("tmp_unit/disco/GIOCO/DATI/clcd32.dll");
    touch("tmp_unit/disco/GIOCO/GIOCO.ICD");
    touch("tmp_unit/disco/CMS_NT.DLL");
    u8_mkdir("tmp_unit/disco/LaserLok");
    CHECK(optical_protection_files("tmp_unit/disco", out, sizeof(out)) == 4);
    CHECK(strstr(out, "clcd32.dll") && strstr(out, "GIOCO.ICD") && count_of(out, " (SafeDisc)") == 1);
    CHECK(strstr(out, "CMS_NT.DLL (SecuROM)") && strstr(out, "LaserLok (LaserLock)"));
    /* a file named like a signature folder is not one */
    u8_mkdirs("tmp_unit/disco2");
    touch("tmp_unit/disco2/LASERLOK");
    touch("tmp_unit/disco2/SETUP.ICDX");
    CHECK(optical_protection_files("tmp_unit/disco2", out, sizeof(out)) == 0);

    /* a list of one's own: comments and spaces */
    f = u8_fopen("tmp_unit/firme.txt", "wb");
    if (f) { fputs("# prova\r\n\r\n  GIOCO/   Protezione di prova\r\nLEGGIMI.TXT\r\n", f); fclose(f); }
    optical_set_signatures("tmp_unit/firme.txt");
    CHECK(optical_protection_files("tmp_unit/disco", out, sizeof(out)) == 2);
    CHECK(strstr(out, "GIOCO (Protezione di prova)") && strstr(out, "LEGGIMI.TXT (unknown protection)"));

    /* the installed file and the built-in list say the same: one file per
     * signature, and both find all of them with the same names */
    f = u8_fopen("../installer/protection-signatures.txt", "rb");
    CHECK(f != NULL);
    u8_mkdirs("tmp_unit/firme-tutte");
    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%99s", pat) != 1) continue;
        if (pat[strlen(pat) - 1] == '/') {
            pat[strlen(pat) - 1] = 0;
            snprintf(path, sizeof(path), "tmp_unit/firme-tutte/d%d/%s", n, pat);
            u8_mkdirs(path);
        } else {
            snprintf(path, sizeof(path), "tmp_unit/firme-tutte/d%d", n);
            u8_mkdirs(path);
            snprintf(path, sizeof(path), "tmp_unit/firme-tutte/d%d/%s", n, pat[0] == '*' ? "X" : "");
            str_copy(path + strlen(path), sizeof(path) - strlen(path), pat[0] == '*' ? pat + 1 : pat);
            touch(path);
        }
        n++;
    }
    if (f) fclose(f);
    CHECK(n >= 20);
    optical_set_signatures("../installer/protection-signatures.txt");
    CHECK(optical_protection_files("tmp_unit/firme-tutte", out, sizeof(out)) == n);
    optical_set_signatures("tmp_unit/firme-che-non-esistono.txt");
    CHECK(optical_protection_files("tmp_unit/firme-tutte", out2, sizeof(out2)) == n);
    CHECK_STR(out, out2);
    optical_set_signatures("");

    /* the drive's offset: a number of samples, as DIC reads it */
    CHECK(optical_set_drive_offset("+6") == 0);
    CHECK(optical_set_drive_offset(" -472 ") == 0);
    CHECK(optical_set_drive_offset("0") == 0);
    CHECK(optical_set_drive_offset("sei") == -1);
    CHECK(optical_set_drive_offset("6 6") == -1);
    CHECK(optical_set_drive_offset("1234567") == -1);
    CHECK(optical_set_drive_offset("") == 0);
}

void test_optical(void)
{
    compare_result r; strbuf msg; json_writer w; FILE *f; char p[600], date[40];
    const char *s;

    test_iso();
    test_signatures();
    test_tree();
    test_progress();
    CHECK_STR(cd_media.name, "cd");
    CHECK_STR(dvd_media.name, "dvd");
    CHECK_STR(cd_media.ext, "");
    CHECK(cd_media.one_read == 1);
    /* CD outcomes in the archive classes */
    CHECK(meta_status_class("redump-verified") == 'p');
    CHECK(meta_status_class("unstable") == 'm');
    CHECK(meta_status_class("read-once") == 'f');
    CHECK(meta_status_class("errors") == 'e');
    CHECK(meta_status_class("read-failed") == '-');
    CHECK(meta_status_done("read-once") && !meta_status_done("read-failed"));
    /* what 261017 wrote, in Italian */
    CHECK(!strcmp(meta_status_english("verificato redump"), ST_REDUMP));
    CHECK(!strcmp(meta_status_english("lettura fallita"), ST_FAILED));
    CHECK(!strcmp(meta_status_english("mancante"), ST_MISSING) && !strcmp(meta_status_english("duplicato"), ST_DUPLICATE));
    CHECK(!strcmp(meta_status_english("clean"), "clean") && !strcmp(meta_status_english("boh"), "boh"));
    {
        arc_box b; char t[200];
        memset(&b, 0, sizeof(b));
        b.clean = 2; b.redump = 1; b.fragile = 1;
        strcpy(b.disks, "ppf"); strcpy(b.kinds, "fcc");
        b.expected = 3; b.count[MEDIA_FLOPPY] = 1; b.count[MEDIA_CD] = 2;
        arc_outcome(&b, t, sizeof(t));
        CHECK_STR(t, "2 clean (1 verified with redump), 1 fragile");
        b.state = ARC_COMPLETE;
        arc_advice(&b, t, sizeof(t));
        CHECK(strstr(t, "CD 2 was read only once") != NULL);
    }

    /* the .cue and .dat names from DIC are ANSI: convert to UTF-8 (here as Latin-1) */
    {
        char a[64] = "Gioco \xe8 qui.bin", b[64] = "Gi\xc3\xb2" "co.bin";
        optical_name_utf8(a, sizeof(a));
        CHECK_STR(a, "Gioco \xc3\xa8 qui.bin");
        optical_name_utf8(b, sizeof(b));             /* already UTF-8: stays */
        CHECK_STR(b, "Gi\xc3\xb2" "co.bin");
#ifndef _WIN32
        sb_init(&msg);
        CHECK(optical_dic_path("cartella", "CD01.bin", a, sizeof(a), &msg) == 0);
        CHECK_STR(a, "cartella/CD01.bin");
        sb_free(&msg);
#else
        /* a folder with a letter outside the code page (omega) and a long
         * name, which also has an 8.3 short name: DIC gets that one, ASCII only.
         * (A name that is already short like "Omega" with the omega has no 8.3
         * name: there optical_dic_path refuses with a message) */
        char out[600]; const unsigned char *q; int ascii = 1;
        sb_init(&msg);
        u8_mkdirs("tmp_unit/Archivio \xce\xa9mega di prova");
        CHECK(optical_dic_path("tmp_unit\\Archivio \xce\xa9mega di prova", "CD01.bin", out, sizeof(out), &msg) == 0);
        for (q = (const unsigned char *)out; *q; q++) if (*q >= 0x80) ascii = 0;
        CHECK(ascii && strstr(out, "CD01.bin") != NULL);
        if (!ascii || !strstr(out, "CD01.bin")) printf("  dic_path: [%s] %s\n", out, msg.s ? msg.s : "");
        u8_rmtree("tmp_unit/Archivio \xce\xa9mega di prova");
        u8_mkdirs("tmp_unit/\xce\xa9mega");
        sb_clear(&msg);
        CHECK(optical_dic_path("tmp_unit\\\xce\xa9mega", "CD01.bin", out, sizeof(out), &msg) == -1);
        CHECK(msg.s && strstr(msg.s, "8.3 short name") != NULL);
        u8_rmtree("tmp_unit/\xce\xa9mega");
        sb_free(&msg);
#endif
    }

    /* redump DATs: the real one yes, DIC's default.dat no */
    optical_set_datdir("fixtures/redump");
    CHECK(optical_find_dat(p, sizeof(p), date, sizeof(date)) == 1);
    CHECK(strstr(p, "IBM - PC compatible") != NULL);
    CHECK_STR(date, "2026-09-01 10-00-00");
    optical_set_datdir("fixtures/dic");
    CHECK(optical_find_dat(p, sizeof(p), date, sizeof(date)) == 0);

#ifndef _WIN32
    sb_init(&said);
    sb_init(&msg);
    compare_init(&r);
    jw_init(&w);
    u8_rmtree("tmp_unit/ottici");
    u8_mkdirs("tmp_unit/ottici");
    optical_set_dic("../build/fake_dic");
    optical_set_drive("E:");
    f = fopen("tmp_unit/ottici/script.txt", "w");
    {   /* one fake DIC read per line */
        static const char *const script[] = {
            "cd_good", "cd_good", "cd_noise", "FAIL", "WRONG", "cd_c2", "cd_c2", "dvd_good",
            "EXIT3 cd_good", "FAIL", "cd_c2fixed", "cd_noc2", "cd_noise", "cd_good", NULL
        };
        int i;
        if (f) { for (i = 0; script[i]; i++) fprintf(f, "%s%c", script[i], 10); fclose(f); }
    }
    setenv("FAKE_DIC_DIR", "fixtures/dic", 1);
    setenv("FAKE_DIC_SCRIPT", "tmp_unit/ottici/script.txt", 1);

    CHECK(cd_media.check(&msg) == 0);
    setenv("FAKE_DIC_NODRIVE", "1", 1);
    CHECK(cd_media.check(&msg) == -1);
    CHECK(strstr(msg.s, "access denied") != NULL);
    unsetenv("FAKE_DIC_NODRIVE");
    CHECK(cd_media.scan("tmp_unit/ottici/CD01.scan.txt", keep, NULL) == 0);
    CHECK(u8_filesize("tmp_unit/ottici/CD01.scan.txt") > 0);

    /* first read, without a redump DAT: not enough */
    optical_set_datdir("tmp_unit/ottici");
    CHECK(cd_media.read("tmp_unit/ottici/CD01", keep, NULL) == 0);
    CHECK(u8_filesize("tmp_unit/ottici/CD01/CD01.cue") > 0);
    CHECK(u8_filesize("tmp_unit/ottici/CD01/CD01 (Track 1).bin") == 300 * 2352);
    CHECK(u8_filesize("tmp_unit/ottici/CD01/CD01_drive.txt") > 0);
    CHECK(u8_filesize("tmp_unit/ottici/CD01/dic-console.txt") > 0);
    sb_clear(&said);
    {   /* no progress lines in the read log */
        char line[400]; int prog = 0, c2 = 0;
        f = fopen("tmp_unit/ottici/CD01/dic-console.txt", "r");
        while (f && fgets(line, sizeof(line), f)) {
            if (strstr(line, "(LBA)")) prog++;
            if (strstr(line, "No C2 errors")) c2++;
        }
        if (f) fclose(f);
        CHECK(prog == 0 && c2 == 1);
    }
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD01", NULL, NULL, &r) == 1);
    CHECK_STR(r.outcome, "read-once");
    CHECK_STR(r.verdict_key, "single");
    CHECK(strstr(r.detail.s, "no redump DAT") != NULL);

    /* second read identical: clean */
    CHECK(cd_media.read("tmp_unit/ottici/CD01_b", keep, NULL) == 0);
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD01", "tmp_unit/ottici/CD01_b", NULL, &r) == 0);
    CHECK_STR(r.outcome, "clean");
    CHECK(r.agree == 2);
    /* third read with a different audio track: unstable */
    CHECK(cd_media.read("tmp_unit/ottici/CD01_c", keep, NULL) == 0);
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD01", "tmp_unit/ottici/CD01_b", "tmp_unit/ottici/CD01_c", &r) == 1);
    CHECK_STR(r.outcome, "unstable");
    CHECK(r.agree == 2 && r.unstable_tracks == 1);

    /* failed read: the folder stays with DIC's logs (the session sets it aside) */
    CHECK(cd_media.read("tmp_unit/ottici/CD09", keep, NULL) == -1);
    CHECK(u8_exists("tmp_unit/ottici/CD09/dic-console.txt"));
    /* a DVD in the drive instead of the CD */
    sb_clear(&said);
    CHECK(cd_media.read("tmp_unit/ottici/CD08", keep, NULL) == -1);
    CHECK(strstr(said.s, "does not hold a CD") != NULL);

    /* with the redump DAT one read is enough */
    optical_set_datdir("fixtures/redump");
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD01", NULL, NULL, &r) == 0);
    CHECK_STR(r.outcome, "redump-verified");
    CHECK(strstr(r.detail.s, "Gioco di Prova (Italy)") != NULL);
    s = jread(&w, &cd_media, "tmp_unit/ottici/CD01");
    CHECK(strstr(s, "\"tool\": \"DiscImageCreator 20260101T233129\"") != NULL);
    CHECK(strstr(s, "\"drive\": \"PLEXTOR DVDR   PX-716A 1.11\"") != NULL);
    CHECK(strstr(s, "\"mode\": \"MODE1/2352\"") != NULL);
    CHECK(strstr(s, "\"mode\": \"AUDIO\"") != NULL);
    CHECK(strstr(s, "Lead-out") == NULL);
    CHECK(strstr(s, "\"c2_errors\": 0") != NULL);
    CHECK(strstr(s, "\"edc_errors\": 0") != NULL);
    CHECK(strstr(s, "\"game\": \"Gioco di Prova (Italy)\"") != NULL);
    CHECK(strstr(s, "\"volume_label\": \"PROVA_CD\"") != NULL);
    CHECK(strstr(s, "\"name\": \"GIOCO/DATI/MONDO.DAT\"") != NULL);
    CHECK(strstr(s, "\"sectors\": 420") != NULL);

    /* C2 errors and SafeDisc: two identical reads stay with errors... */
    optical_set_datdir("tmp_unit/ottici");
    CHECK(cd_media.read("tmp_unit/ottici/CD02", keep, NULL) == 0);
    CHECK(cd_media.read("tmp_unit/ottici/CD02_b", keep, NULL) == 0);
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD02", "tmp_unit/ottici/CD02_b", NULL, &r) == 2);
    CHECK_STR(r.outcome, "errors");
    s = jread(&w, &cd_media, "tmp_unit/ottici/CD02");
    CHECK(strstr(s, "\"protection\": \"SafeDisc\"") != NULL);
    CHECK(strstr(s, "\"c2_errors\": 3") != NULL);
    CHECK(strstr(s, "\"redump\": null") != NULL);
    /* ...but if redump has the same disc, the errors are the intended ones */
    optical_set_datdir("fixtures/redump");
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD02", NULL, NULL, &r) == 0);
    CHECK_STR(r.outcome, "redump-verified");

    /* DVD: the iso, the file system and redump */
    CHECK(dvd_media.read("tmp_unit/ottici/DVD01", keep, NULL) == 0);
    CHECK(u8_filesize("tmp_unit/ottici/DVD01/DVD01.iso") == 300 * 2048);
    CHECK(cmp(&dvd_media, "tmp_unit/ottici/DVD01", NULL, NULL, &r) == 0);
    CHECK(strstr(r.detail.s, "Gioco & DVD di Prova (Italy)") != NULL);
    s = jread(&w, &dvd_media, "tmp_unit/ottici/DVD01");
    CHECK(strstr(s, "\"volume_label\": \"PROVA_CD\"") != NULL);
    CHECK(strstr(s, "\"sectors\": 300") != NULL);

    /* with several reads each is looked up in redump: the right one becomes the master */
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD01_c", "tmp_unit/ottici/CD01", NULL, &r) == 0);
    CHECK_STR(r.outcome, "redump-verified");
    CHECK(r.master == 2);
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD01_c", "tmp_unit/ottici/CD01_b", NULL, &r) == 0);
    CHECK(r.master == 2);                       /* even if the first one is the wrong one */
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD01_c", "tmp_unit/ottici/CD01_c", NULL, &r) == 0);
    CHECK_STR(r.outcome, "clean");             /* two identical reads that redump does not know */
    CHECK(r.master == 0);

    /* DIC exits with an error but the read is complete: it is kept, with the error */
    optical_set_datdir("tmp_unit/ottici");
    sb_clear(&said);
    CHECK(cd_media.read("tmp_unit/ottici/CD03", keep, NULL) == 0);
    CHECK(u8_filesize("tmp_unit/ottici/CD03/CD03.cue") > 0);
    CHECK(strstr(said.s, "the read is complete") != NULL);
    s = jread(&w, &cd_media, "tmp_unit/ottici/CD03");
    CHECK(strstr(s, "\"dic_exit_code\": 3") != NULL);
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD03", "tmp_unit/ottici/CD01", NULL, &r) == 2);
    CHECK_STR(r.outcome, "errors");
    CHECK(strstr(r.detail.s, "exited with error 3") != NULL);
    /* without a .dat there is no read, and the message does not blame only the disc */
    sb_clear(&said);
    CHECK(cd_media.read("tmp_unit/ottici/CD04", keep, NULL) == -1);
    CHECK(u8_exists("tmp_unit/ottici/CD04/dic-console.txt"));
    CHECK(strstr(said.s, "Last line: ") != NULL && strstr(said.s, "MB free") != NULL);
    CHECK(strstr(said.s, "NOT_READY") != NULL);

    /* C2 errors corrected on re-reads; drive that does not report C2 */
    CHECK(cd_media.read("tmp_unit/ottici/CD05", keep, NULL) == 0);
    s = jread(&w, &cd_media, "tmp_unit/ottici/CD05");
    CHECK(strstr(s, "\"c2_errors\": 0") != NULL && strstr(s, "\"c2_check\": 1") != NULL);
    CHECK(cd_media.read("tmp_unit/ottici/CD06", keep, NULL) == 0);
    s = jread(&w, &cd_media, "tmp_unit/ottici/CD06");
    CHECK(strstr(s, "\"c2_check\": 0") != NULL);
    CHECK(cmp(&cd_media, "tmp_unit/ottici/CD06", NULL, NULL, &r) == 1);
    CHECK(strstr(r.detail.s, "C2 check not available on this drive") != NULL);

    /* the redump cache follows the read, not just the folder */
    optical_set_datdir("fixtures/redump");
    CHECK(cd_media.read("tmp_unit/ottici/CD09", keep, NULL) == 0);       /* cd_noise */
    s = jread(&w, &cd_media, "tmp_unit/ottici/CD09");
    CHECK(strstr(s, "\"redump\": null") != NULL);
    u8_rmtree("tmp_unit/ottici/CD09");
    CHECK(cd_media.read("tmp_unit/ottici/CD09", keep, NULL) == 0);       /* cd_good */
    s = jread(&w, &cd_media, "tmp_unit/ottici/CD09");
    CHECK(strstr(s, "\"game\": \"Gioco di Prova (Italy)\"") != NULL);

    /* space and file system before the read */
    sb_clear(&msg);
    CHECK(optical_room("tmp_unit", 1LL << 60, 0, &msg) == -1);
    CHECK(strstr(msg.s, "MB free") != NULL);
    CHECK(optical_room("tmp_unit", 1024, 0, &msg) == 0);
    sb_clear(&msg);
    CHECK(optical_fs_ok("FAT32", 5000LL * 1048576, &msg) == -1 && strstr(msg.s, "NTFS") != NULL);
    CHECK(optical_fs_ok("FAT32", 700LL * 1048576, &msg) == 0);
    CHECK(optical_fs_ok("NTFS", 9000LL * 1048576, &msg) == 0);

    /* the script is finished: DIC fails */
    CHECK(cd_media.read("tmp_unit/ottici/CD07", keep, NULL) == -1);
    /* DIC that is missing */
    sb_clear(&said);
    optical_set_dic("dic-che-non-esiste");
    CHECK(cd_media.read("tmp_unit/ottici/CD06", keep, NULL) == MEDIA_NOTOOL);
    CHECK(strstr(said.s, "Cannot find dic-che-non-esiste") != NULL);
    CHECK(cd_media.scan("tmp_unit/ottici/CD06.scan.txt", keep, NULL) == MEDIA_NOTOOL);
    sb_clear(&msg);
    CHECK(cd_media.check(&msg) == -1 && strstr(msg.s, "Cannot find") != NULL);

    unsetenv("FAKE_DIC_SCRIPT");
    unsetenv("FAKE_DIC_DIR");
    optical_set_dic("DiscImageCreator");
    jw_free(&w);
    compare_free(&r);
    sb_free(&msg);
    sb_free(&said);
#else
    (void)r; (void)msg; (void)w; (void)f; (void)s; (void)jread; (void)cmp; (void)keep;
#endif
    optical_set_datdir("");
}
