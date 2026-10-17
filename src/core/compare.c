/* compare - the comparison that used to live inside dskdiff.
 * The text stays identical (tests/golden.sh checks it), but it ends up in
 * r->detail instead of on stdout.
 * Text without accents: it also goes to the Windows XP console. */
#include <stdlib.h>
#include <string.h>
#include "compare.h"
#include "edsk.h"

void compare_init(compare_result *r)
{
    memset(r, 0, sizeof(*r));
    sb_init(&r->detail);
}

void compare_free(compare_result *r) { sb_free(&r->detail); }

const char *verdict_name(verdict v)
{
    static const char *names[] = {
        "identical", "noise_only", "data_differ", "unstable", "unmapped", "error"
    };
    return names[v];
}

static int finish(compare_result *r, verdict v)
{
    r->v = v;
    if (v == V_IDENTICAL || v == V_NOISE_ONLY) r->exit_code = 0;
    else if (v == V_ERROR)                     r->exit_code = 2;
    else                                       r->exit_code = 1;
    return r->exit_code;
}

/* the name of read i in the text */
#define SHOWN(r, files, i) ((r)->names ? (r)->names[i] : (files)[i])

static void geometry(compare_result *r, const edsk *d, int stdcyl)
{
    r->cylinders = d->cyls; r->heads = d->heads;
    r->nomcyl = d->nomcyl;  r->nsec0 = d->nsec0; r->stdcyl = stdcyl;
}

/* Comparison of 3 or more reads of the same floppy.
 * For each track it counts how many distinct variants appear and how
 * many reads agree with each other: that is what tells whether a
 * majority exists or the track is unstable on every pass. */
static int multi(const char *const *files, int nf, int forced, compare_result *r)
{
    edsk *d = (edsk *)calloc(COMPARE_MAXF, sizeof(edsk));
    unsigned long h[COMPARE_MAXF][EDSK_MAXTRK];
    int i, t, j, k, stdcyl, nvar, best, grp[COMPARE_MAXF], loaded = 0;
    int unst_dati = 0, unst_extra = 0, shown = 0, ntrk, nsz = 0, worst = nf, rc;
    char s1[32];
    strbuf *o = &r->detail;

    if (!d) { sb_puts(o, "  ERRORE: memoria insufficiente\n"); return finish(r, V_ERROR); }
    for (i = 0; i < nf; i++) {
        if (!edsk_load_as(files[i], SHOWN(r, files, i), &d[i], o)) { rc = finish(r, V_ERROR); goto out; }
        loaded++;
    }

    sb_printf(o, "  %d letture a confronto\n\n", nf);
    for (i = 0; i < nf; i++) {
        fmt_thousands(d[i].len, s1);
        sb_printf(o, "  %d) %-24s %14s byte\n", i + 1, SHOWN(r, files, i), s1);
    }
    sb_puts(o, "\n");

    if (!d[0].ok) {
        sb_puts(o, "  ESITO: intestazione non riconosciuta.\n\n");
        rc = finish(r, V_ERROR); goto out;
    }

    stdcyl = forced ? forced : (d[0].nomcyl ? edsk_snap(d[0].nomcyl) : 80);
    geometry(r, &d[0], stdcyl);
    sb_printf(o, "  tracce formattate: %d cilindri", d[0].nomcyl);
    if (d[0].nsec0) sb_printf(o, ", %d settori per traccia", d[0].nsec0);
    sb_printf(o, "\n  area standard considerata: 0-%d\n\n", stdcyl - 1);

    ntrk = d[0].ntrk;
    for (i = 0; i < nf; i++)
        for (t = 0; t < ntrk; t++) {
            if (t >= d[i].ntrk || d[i].siz[t] <= 0 ||
                d[i].off[t] + d[i].siz[t] > d[i].len) { h[i][t] = 0; continue; }
            h[i][t] = edsk_fnv(d[i].buf + d[i].off[t], d[i].siz[t]);
        }

    for (t = 0; t < ntrk; t++) {
        int szdiff = 0;
        for (i = 1; i < nf; i++)
            if (t < d[i].ntrk && d[i].siz[t] != d[0].siz[t]) { szdiff = 1; break; }
        nvar = 0; best = 0;
        for (i = 0; i < nf; i++) grp[i] = 0;
        for (i = 0; i < nf; i++) {
            if (grp[i]) continue;
            nvar++; k = 1; grp[i] = nvar;
            for (j = i + 1; j < nf; j++)
                if (!grp[j] && h[j][t] == h[i][t]) { grp[j] = nvar; k++; }
            if (k > best) best = k;
        }
        if (nvar <= 1) continue;

        if (d[0].cyl[t] < stdcyl) { unst_dati++; if (best < worst) worst = best; }
        else { unst_extra++; continue; }

        if (shown < 30) {
            sb_printf(o, "  cil %2d faccia %d : %d varianti su %d letture",
                      d[0].cyl[t], d[0].head[t], nvar, nf);
            if (best > 1) sb_printf(o, "   (%d concordano)", best);
            else          sb_puts(o, "   (nessuna coppia concorde)");
            if (szdiff) { sb_puts(o, "   DIMENSIONE DIVERSA"); nsz++; }
            sb_puts(o, "\n");
            shown++;
        } else if (shown == 30) { sb_puts(o, "  ... e altre tracce\n"); shown++; }
    }
    if (unst_extra)
        sb_printf(o, "\n  tracce instabili nei cilindri extra: %d (rumore, normale)\n", unst_extra);
    sb_puts(o, "\n");

    r->unstable_tracks = unst_dati;
    r->agree = unst_dati ? worst : nf;
    if (!unst_dati) {
        sb_puts(o, "  ESITO: l'area dati e' IDENTICA in tutte le letture.\n\n");
        sb_puts(o, "  Il disco e' stabile. Catalogalo come pulito.\n\n");
        rc = finish(r, V_IDENTICAL); goto out;
    }

    sb_printf(o, "  ESITO: %d tracce dell'area dati cambiano fra una lettura e l'altra.\n\n", unst_dati);
    if (nsz) {
        sb_printf(o, "  %d di queste hanno DIMENSIONE DIVERSA fra una lettura e l'altra.\n", nsz);
        sb_puts(o, "  In un EDSK una traccia cresce quando SAMdisk salva piu' copie di un\n");
        sb_puts(o, "  settore che a riletture successive ha dato dati diversi. Vuol dire\n");
        sb_puts(o, "  che li' SAMdisk ha incontrato errori e ha dovuto insistere, anche se\n");
        sb_puts(o, "  non te l'ha detto a video: e' il segno di un supporto degradato.\n");
        sb_puts(o, "  Verifica con:  samdisk scan NOMEFILE.DSK\n");
        sb_puts(o, "  e cerca i flag m1, m2 o dc sui cilindri elencati sopra.\n\n");
    }
    sb_puts(o, "  Se le letture segnalano errori dc o nd, il supporto e' degradato:\n");
    sb_puts(o, "  rileggi con --retries=30 --rescans=5, pulisci le testine, prova un altro drive.\n\n");
    sb_puts(o, "  Se invece NON segnalano errori, sono settori deboli scritti apposta\n");
    sb_puts(o, "  per essere instabili, cioe' una protezione anticopia. In quel caso\n");
    sb_puts(o, "  nessun tentativo in piu' aiuta: serve la cattura in flux.\n\n");
    sb_puts(o, "  Dove alcune letture concordano puoi tenere la variante di maggioranza.\n\n");
    rc = finish(r, V_UNSTABLE);
out:
    for (i = 0; i < loaded; i++) edsk_free(&d[i]);
    free(d);
    return rc;
}

/* Comparison of two reads, byte by byte, with the track map. */
static int pair(const char *const *fl, int forced, compare_result *r)
{
    edsk *a = (edsk *)calloc(2, sizeof(edsk)), *b;
    const edsk *map;
    long i, lim, ndiff = 0, nhdr = 0, firstdiff = -1;
    long cnt[EDSK_MAXTRK];
    int t, worst = 0, shown, stdcyl, rc, unstable = 0;
    char s1[32], s2[32];
    strbuf *o = &r->detail;

    if (!a) { sb_puts(o, "  ERRORE: memoria insufficiente\n"); return finish(r, V_ERROR); }
    b = a + 1;
    if (!edsk_load_as(fl[0], SHOWN(r, fl, 0), a, o)) { rc = finish(r, V_ERROR); goto out; }
    if (!edsk_load_as(fl[1], SHOWN(r, fl, 1), b, o)) { rc = finish(r, V_ERROR); goto out; }

    fmt_thousands(a->len, s1); fmt_thousands(b->len, s2);
    sb_printf(o, "  A: %-24s %14s byte", SHOWN(r, fl, 0), s1);
    if (a->ok) sb_printf(o, "   %d cilindri, %d facce", a->cyls, a->heads);
    sb_puts(o, "\n");
    sb_printf(o, "  B: %-24s %14s byte", SHOWN(r, fl, 1), s2);
    if (b->ok) sb_printf(o, "   %d cilindri, %d facce", b->cyls, b->heads);
    sb_puts(o, "\n");

    if (a->ok && a->nomcyl) {
        sb_printf(o, "  tracce formattate: %d cilindri", a->nomcyl);
        if (a->nsec0) sb_printf(o, ", %d settori per traccia", a->nsec0);
        sb_puts(o, "\n");
        if (edsk_snap(a->nomcyl) != a->nomcyl)
            sb_printf(o, "  area standard considerata: 0-%d  (il resto e' territorio extra)\n",
                      edsk_snap(a->nomcyl) - 1);
    } else if (a->ok) {
        sb_puts(o, "  ATTENZIONE: nessuna traccia formattata riconosciuta.\n");
        sb_puts(o, "              Uso la soglia classica degli 80 cilindri.\n");
    }
    sb_puts(o, "\n");

    if (a->len != b->len) {
        sb_puts(o, "  ATTENZIONE: i file hanno dimensioni diverse.\n");
        if (a->ok && b->ok && a->cyls != b->cyls)
            sb_printf(o, "              Le due letture hanno catturato un numero di cilindri\n"
                         "              diverso (%d contro %d). Per un confronto pulito rileggi\n"
                         "              entrambe con -c80.\n", a->cyls, b->cyls);
        sb_puts(o, "              Confronto limitato alla parte in comune.\n\n");
    }

    stdcyl = forced ? forced : (a->nomcyl ? edsk_snap(a->nomcyl) : 80);
    geometry(r, a, stdcyl);
    if (forced) sb_printf(o, "  soglia forzata a %d cilindri\n\n", stdcyl);

    memset(cnt, 0, sizeof(cnt));
    lim = (a->len < b->len) ? a->len : b->len;
    map = a->ok ? a : b;

    for (i = 0; i < lim; i++) {
        if (a->buf[i] == b->buf[i]) continue;
        ndiff++;
        if (firstdiff < 0) firstdiff = i;
        t = edsk_track_of(map, i);
        if (t < 0) nhdr++;
        else {
            cnt[t]++;
            if (map->cyl[t] < stdcyl) worst = 1;
        }
    }

    if (ndiff == 0 && a->len == b->len) {
        sb_puts(o, "  ESITO: i due file sono IDENTICI.\n\n"
                   "  La lettura e' perfettamente riproducibile. Catalogalo come pulito.\n\n");
        r->agree = 2;
        rc = finish(r, V_IDENTICAL); goto out;
    }

    fmt_thousands(ndiff, s1);
    sb_printf(o, "  byte diversi: %s", s1);
    if (firstdiff >= 0) sb_printf(o, "   (il primo a 0x%08lX)", firstdiff);
    sb_puts(o, "\n\n");

    if (nhdr) {
        fmt_thousands(nhdr, s1);
        sb_printf(o, "  intestazione .............. %s\n", s1);
    }
    shown = 0;
    for (t = 0; t < EDSK_MAXTRK; t++) {
        if (!cnt[t]) continue;
        if (map->cyl[t] < stdcyl) unstable++;
        if (shown == 24) { sb_puts(o, "  ... e altre tracce\n"); shown++; continue; }
        if (shown > 24) continue;
        fmt_thousands(cnt[t], s1);
        sb_printf(o, "  cil %2d faccia %d ........... %s%s\n",
                  map->cyl[t], map->head[t], s1,
                  map->cyl[t] >= stdcyl ? "   (cilindro extra)" : "");
        shown++;
    }
    sb_puts(o, "\n");
    r->unstable_tracks = unstable;

    if (!map->ok) {
        sb_puts(o, "  ESITO: non riesco a mappare le tracce su questi file.\n\n");
        r->agree = 1;
        rc = finish(r, V_UNMAPPED); goto out;
    }

    if (!worst) {
        sb_printf(o, "  ESITO: i cilindri 0-%d sono IDENTICI.\n\n", stdcyl - 1);
        sb_puts(o, "  Tutte le differenze stanno nei cilindri extra o nell'intestazione.\n");
        sb_puts(o, "  Li' il disco non e' formattato e la testina legge rumore magnetico,\n");
        sb_puts(o, "  diverso a ogni passata: non dice nulla sullo stato del supporto.\n\n");
        sb_puts(o, "  Il disco e' stabile. Catalogalo come pulito.\n\n");
        r->agree = 2; r->unstable_tracks = 0;
        rc = finish(r, V_NOISE_ONLY); goto out;
    }

    sb_puts(o, "  ESITO: ci sono differenze nei DATI del disco.\n\n");
    sb_puts(o, "  Le due letture dello stesso dischetto non coincidono su cilindri\n");
    sb_puts(o, "  dentro l'area standard: il segnale e' al limite e il controller lo\n");
    sb_puts(o, "  interpreta diversamente a ogni passata.\n\n");
    sb_puts(o, "  Rileggi con  --retries=30 --rescans=5  e catalogalo come marginale.\n\n");
    r->agree = 1;
    rc = finish(r, V_DATA_DIFFER);
out:
    edsk_free(a); edsk_free(b);
    free(a);
    return rc;
}

int compare_files(const char *const *paths, int n, int forced_cyl, compare_result *r)
{
    if (n < 2 || n > COMPARE_MAXF) {
        sb_printf(&r->detail, "  ERRORE: servono da 2 a %d letture\n", COMPARE_MAXF);
        return finish(r, V_ERROR);
    }
    return n > 2 ? multi(paths, n, forced_cyl, r) : pair(paths, forced_cyl, r);
}
