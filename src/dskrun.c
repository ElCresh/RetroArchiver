/* dskrun - batch archiving of floppies, from the console.
 *
 * For each disk: beep, wait, then scan, two reads, comparison and, if
 * needed, a third read, all automatically. The result is one BagIt
 * package per box (images, scans, metadata.json) plus one row per disk
 * in archivio.csv. The logic lives in core/session.c, shared with the
 * GUI. A box can also hold CDs and DVDs (-C, -D): they are asked for
 * after the floppies and read with DiscImageCreator (-X, -d).
 *
 * On-screen text has no accents: the Windows XP console does not render
 * them well.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/fsu8.h"
#include "core/session.h"

#ifdef _WIN32
#include <windows.h>
/* only on a console: when output goes to a file or pipe (WinRM, tests) stay silent */
static void beep2(void)
{
    DWORD mode;
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) return;
    Beep(880, 200); Sleep(90); Beep(1320, 260);
}
/* argv and the console are not UTF-8 on Windows: convert from the code page */
static void to_utf8(const char *in, char *out, size_t n, UINT cp)
{
    wchar_t w[1024]; char buf[4096];
    /* wide buffer first, then str_copy cuts on a UTF-8 boundary */
    if (!MultiByteToWideChar(cp, 0, in, -1, w, 1024) ||
        !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), NULL, NULL))
        str_copy(out, n, in);
    else
        str_copy(out, n, buf);
}
#define ARG_CP     CP_ACP
#define CONSOLE_CP GetConsoleCP()
#else
#include <unistd.h>
static void beep2(void) { if (isatty(1)) { printf("\a"); fflush(stdout); } }
static void to_utf8(const char *in, char *out, size_t n, unsigned cp) { (void)cp; str_copy(out, n, in); }
#define ARG_CP     0
#define CONSOLE_CP 0
#endif

/* Ctrl+C: the first one stops SAMdisk (started without a console, it
 * would not receive it) and the session saves and closes; the second one
 * in the same phase really exits. Reset at every phase. */
static volatile int ctrlc;
#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (ctrlc++) return FALSE;
    runner_cancel();
    return TRUE;
}
static void catch_ctrlc(void) { SetConsoleCtrlHandler(on_ctrl, TRUE); }
#else
#include <signal.h>
static void on_sigint(int sig)
{
    if (ctrlc++) { signal(sig, SIG_DFL); raise(sig); return; }
    runner_cancel();
}
static void catch_ctrlc(void) { signal(SIGINT, on_sigint); }
#endif

/* The program text is UTF-8 (titles, labels, SAMdisk and DiscImageCreator
 * lines, file names). On a real Windows console it goes out in its code
 * page (850 on an Italian XP), otherwise accents come out as two garbage
 * characters; on a file or pipe it stays UTF-8. */
#ifdef _WIN32
static void put_text(const char *s)
{
    static int console = -1; DWORD mode; char *c;
    if (console < 0) console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
    if (!console || !(c = u8_to_cp(s, GetConsoleOutputCP()))) { fputs(s, stdout); return; }
    fputs(c, stdout);
    free(c);
}
#else
static void put_text(const char *s) { fputs(s, stdout); }
#endif

static void say(const char *fmt, ...)
{
    strbuf b; va_list ap;
    sb_init(&b);
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    put_text(b.s);
    sb_free(&b);
}

static const meta_box *g_box;           /* used to name the media */
static int npulito, nmarginale, nerrore, naltro;
/* last outcome of each disk: with -r a retried disk is counted once */
static char outcome[META_MAXDISK + 1];

static void count_outcome(int disk, const char *text)
{
    int i; char o = meta_status_class(text);
    if (o == 'f') o = 'm';                  /* fragile, read once: needs a look */
    else if (o == '-') o = 'a';
    if (disk >= 1 && disk <= META_MAXDISK) outcome[disk] = o;
    npulito = nmarginale = nerrore = naltro = 0;
    for (i = 1; i <= META_MAXDISK; i++) {
        if (outcome[i] == 'p') npulito++;
        else if (outcome[i] == 'm') nmarginale++;
        else if (outcome[i] == 'e') nerrore++;
        else if (outcome[i] == 'a') naltro++;
    }
}

/* "floppy 2", "CD 1" */
static const char *who(int n)
{
    static char out[32]; int kind, num;
    meta_slot(g_box, n, &kind, &num);
    meta_disk_name(kind, num, out, sizeof(out));
    return out;
}

static void on_event(void *ctx, event_kind k, int disk, const char *text)
{
    (void)ctx;
    switch (k) {
    case EV_PHASE:
        ctrlc = 0;
        say("\n  --- %s: %s ---\n", who(disk), text);
        break;
    case EV_LINE:
        say("  %s\n", text);
        break;
    case EV_DISK_DONE:
        count_outcome(disk, text);
        say("\n  >>> %s: %s\n", who(disk), text);
        say("  >>> finora: %d puliti, %d marginali, %d con errori, %d saltati o da rifare\n",
               npulito, nmarginale, nerrore, naltro);
        break;
    case EV_BOX_DONE:
        say("\n  >>> cofanetto %s: pacchetto chiuso e verificato.\n", text);
        break;
    case EV_ERROR:
        say("\n  ERRORE: %s\n", text);
        break;
    }
    fflush(stdout);
}

static int wait_disk(void *ctx, const disk_turn *t, char *label, size_t llen, char *note, size_t nlen)
{
    char line[160]; size_t k; const char *kind = meta_kind_label(t->kind);
    (void)ctx; (void)note; (void)nlen;
    beep2();
    say("\n");
    say("  ============================================================\n");
    if (t->readable) {
        say("    Inserisci il %s %d di %d\n", kind, t->num, t->count);
        say("    INVIO per leggerlo (oppure scrivi l'etichetta e INVIO)\n");
        say("    S salta   -   F chiudi il cofanetto   -   Q esci e riprendi dopo\n");
    } else {
        say("    %s %d di %d: la lettura dei %s non c'e' ancora\n", kind, t->num, t->count, kind);
        say("    Il %s resta da leggere e il cofanetto aperto: lo riprenderai\n", kind);
        say("    quando dskrun sapra' leggerlo. INVIO o S per andare avanti\n");
        say("    (puoi scrivere l'etichetta e INVIO)\n");
        say("    F chiudi il cofanetto (il %s risultera' mancante)   -   Q esci\n", kind);
    }
    say("  ============================================================\n");
    say("  > ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return CMD_QUIT;
    k = strcspn(line, "\r\n");
    line[k] = 0;
    if (k == 1 && (line[0] == 'q' || line[0] == 'Q')) return CMD_QUIT;
    if (k == 1 && (line[0] == 's' || line[0] == 'S')) return CMD_SKIP;
    if (k == 1 && (line[0] == 'f' || line[0] == 'F')) return CMD_FINISH;
    if (k) to_utf8(line, label, llen, CONSOLE_CP);
    return CMD_READ;
}

static void usage(void)
{
    say("  uso: dskrun NUMERO_DISCHI -t TITOLO [opzioni]\n\n");
    say("    NUMERO_DISCHI  quanti floppy ha il cofanetto (anche 0 con -C o -D)\n");
    say("    -f N       come NUMERO_DISCHI: i floppy del cofanetto\n");
    say("    -C N       i CD del cofanetto          (default 0)\n");
    say("    -D N       i DVD del cofanetto         (default 0)\n");
    say("    -t TITOLO  titolo del cofanetto: diventa la cartella del pacchetto\n");
    say("    -P NOME    piattaforma (MS-DOS, Amiga, ...)\n");
    say("    -p SCHEMA  nome dei file dei floppy, con segnaposto (default FLOPPY{nn})\n");
    say("    -o DIR     cartella di archiviazione (default quella corrente)\n");
    say("    -s N       comincia dal supporto N (default 1; i CD vengono dopo i floppy)\n");
    say("    -m N       letture massime per disco   (default 3)\n");
    say("    -c N       forza i cilindri per il confronto\n");
    say("    -k         conserva tutte le letture, non solo i master\n");
    say("    -r         un disco non riuscito si richiede subito: INVIO riprova, S salta\n");
    say("    -x FILE    programma SAMdisk da usare  (default samdisk)\n");
    say("    -a SORG    da dove leggere: unita' o file immagine (default a:)\n");
    say("    -X FILE    programma DiscImageCreator  (default DiscImageCreator)\n");
    say("    -d UNITA   lettore dei CD e DVD        (default d:)\n");
    say("    -R DIR     cartella dei DAT di redump  (default quella di dskrun)\n\n");
    say("  Segnaposto dello schema:\n");
    say("    {n} {nn} {nnn}   numero del floppy, con zeri davanti\n");
    say("    {set}            il titolo\n");
    say("    {data}           la data di oggi\n\n");
    say("  Esempi:\n");
    say("    dskrun 4 -t \"Monkey Island\" -P MS-DOS -o D:\\Archivio\n");
    say("    dskrun 3 -C 1 -t \"Windows NT Workstation 4.0\" -P \"Windows NT\"\n\n");
    say("  Ogni cofanetto diventa un pacchetto BagIt in DIR\\TITOLO, con le\n");
    say("  immagini, le scansioni e metadata.json. Se lo interrompi (Q),\n");
    say("  rilancia lo stesso comando per riprendere dal disco mancante.\n");
    say("  I CD e i DVD si chiedono dopo i floppy e si leggono con\n");
    say("  DiscImageCreator, ognuno nella sua cartella (data\\CD01\\...):\n");
    say("  se il disco e' nel DAT di redump basta una lettura, altrimenti\n");
    say("  se ne fanno due e si confrontano.\n\n");
    say("  Uscita: 0 cofanetto chiuso, 1 lasciato aperto, 2 errore.\n\n");
}

int main(int argc, char **argv)
{
    session_cfg cfg; session_host host; meta_box *box;
    char title[256] = "", platform[64] = "non indicata", tpl[256] = "FLOPPY{nn}", outdir[512] = ".";
    char samdisk[512] = "", source[512] = "", comp[80], dic[512] = "", drive[16] = "", dats[512] = "";
    int i, rc, tot = 0, ncd = 0, ndvd = 0;

    say("\ndskrun 2.2 - archiviazione a lotti di floppy, CD e DVD\n\n");
    memset(&cfg, 0, sizeof(cfg));
    memset(&host, 0, sizeof(host));
    cfg.maxread = 3; cfg.start = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc)      to_utf8(argv[++i], tpl, sizeof(tpl), ARG_CP);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) to_utf8(argv[++i], outdir, sizeof(outdir), ARG_CP);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) to_utf8(argv[++i], title, sizeof(title), ARG_CP);
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) to_utf8(argv[++i], platform, sizeof(platform), ARG_CP);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) cfg.start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) cfg.maxread = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cfg.forced_cyl = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) tot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-C") && i + 1 < argc) ncd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) ndvd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) to_utf8(argv[++i], samdisk, sizeof(samdisk), ARG_CP);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) to_utf8(argv[++i], source, sizeof(source), ARG_CP);
        else if (!strcmp(argv[i], "-X") && i + 1 < argc) to_utf8(argv[++i], dic, sizeof(dic), ARG_CP);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) to_utf8(argv[++i], drive, sizeof(drive), ARG_CP);
        else if (!strcmp(argv[i], "-R") && i + 1 < argc) to_utf8(argv[++i], dats, sizeof(dats), ARG_CP);
        else if (!strcmp(argv[i], "-k"))                 cfg.keepall = 1;
        else if (!strcmp(argv[i], "-r"))                 host.ask_again = 1;
        else if (argv[i][0] != '-')                      tot = atoi(argv[i]);
    }
    if (cfg.maxread < 2) cfg.maxread = 2;
    if (cfg.maxread > META_MAXREAD) cfg.maxread = META_MAXREAD;
    if (tot < 0 || ncd < 0 || ndvd < 0 || tot + ncd + ndvd < 1 ||
        tot + ncd + ndvd > META_MAXDISK || !title[0]) { usage(); return 2; }

    box = meta_new();
    if (!box) { say("  ERRORE: memoria insufficiente\n"); return 2; }
    str_copy(box->title, sizeof(box->title), title);
    str_copy(box->platform, sizeof(box->platform), platform);
    meta_set_counts(box, tot, ncd, ndvd);
    g_box = box;
    if (samdisk[0]) floppy_set_samdisk(samdisk);
    if (source[0]) {
        floppy_set_source(source);
        say("  Lettura da: %s\n", source);
    }

    cfg.box = box; cfg.outroot = outdir;
    cfg.names[MEDIA_FLOPPY] = tpl;          /* CD and DVD: CD{nn}, DVD{nn} */
    cfg.media[MEDIA_FLOPPY] = &floppy_media;
    cfg.media[MEDIA_CD] = &cd_media;
    cfg.media[MEDIA_DVD] = &dvd_media;
    if (dic[0]) optical_set_dic(dic);
    if (drive[0]) optical_set_drive(drive);
    if (dats[0]) optical_set_datdir(dats);
    host.event = on_event; host.wait_disk = wait_disk; host.ctx = NULL;

    meta_composition(box, comp, sizeof(comp));
    say("  Cofanetto: %s\n", title);
    say("  %s, massimo %d letture per supporto.\n", comp, cfg.maxread);
    if (ncd || ndvd) {
        char dat[600], date[40];
        if (optical_find_dat(dat, sizeof(dat), date, sizeof(date)))
            say("  DAT di redump: %s (%s)\n", dat, date[0] ? date : "senza data");
        else
            say("  Nessun DAT di redump: ogni CD e DVD si legge due volte.\n");
    }
    say("  Cartella di archiviazione: %s\n", outdir);
    catch_ctrlc();
    rc = session_run(&cfg, &host);

    beep2();
    say("\n  ============================================================\n");
    say("    %s\n", rc == SESSION_CLOSED ? "Cofanetto chiuso." :
                       rc == SESSION_OPEN ? "Cofanetto aperto: si puo' riprendere." :
                       "Archiviazione non riuscita: leggi l'errore qui sopra.");
    say("    %d puliti, %d marginali, %d con errori, %d saltati o da rifare\n",
           npulito, nmarginale, nerrore, naltro);
    say("  ============================================================\n\n");
    meta_delete(box);
    return rc;
}
