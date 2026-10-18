/* compare - the comparison that used to live inside dskdiff.
 * The text keeps the layout of dskdiff's historical output (tests/golden.sh
 * checks it), but it ends up in r->detail instead of on stdout.
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

    if (!d) { sb_puts(o, "  ERROR: out of memory\n"); return finish(r, V_ERROR); }
    for (i = 0; i < nf; i++) {
        if (!edsk_load_as(files[i], SHOWN(r, files, i), &d[i], o)) { rc = finish(r, V_ERROR); goto out; }
        loaded++;
    }

    sb_printf(o, "  %d reads compared\n\n", nf);
    for (i = 0; i < nf; i++) {
        fmt_thousands(d[i].len, s1);
        sb_printf(o, "  %d) %-24s %14s bytes\n", i + 1, SHOWN(r, files, i), s1);
    }
    sb_puts(o, "\n");

    if (!d[0].ok) {
        sb_puts(o, "  RESULT: header not recognized.\n\n");
        rc = finish(r, V_ERROR); goto out;
    }

    stdcyl = forced ? forced : (d[0].nomcyl ? edsk_snap(d[0].nomcyl) : 80);
    geometry(r, &d[0], stdcyl);
    sb_printf(o, "  formatted tracks: %d cylinders", d[0].nomcyl);
    if (d[0].nsec0) sb_printf(o, ", %d sectors per track", d[0].nsec0);
    sb_printf(o, "\n  standard area checked: 0-%d\n\n", stdcyl - 1);

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
            sb_printf(o, "  cyl %2d side %d : %d variants in %d reads",
                      d[0].cyl[t], d[0].head[t], nvar, nf);
            if (best > 1) sb_printf(o, "   (%d agree)", best);
            else          sb_puts(o, "   (no two agree)");
            if (szdiff) { sb_puts(o, "   SIZE DIFFERS"); nsz++; }
            sb_puts(o, "\n");
            shown++;
        } else if (shown == 30) { sb_puts(o, "  ... and more tracks\n"); shown++; }
    }
    if (unst_extra)
        sb_printf(o, "\n  unstable tracks in the extra cylinders: %d (noise, normal)\n", unst_extra);
    sb_puts(o, "\n");

    r->unstable_tracks = unst_dati;
    r->agree = unst_dati ? worst : nf;
    if (!unst_dati) {
        sb_puts(o, "  RESULT: the data area is IDENTICAL in all reads.\n\n");
        sb_puts(o, "  The disk is stable. Catalog it as clean.\n\n");
        rc = finish(r, V_IDENTICAL); goto out;
    }

    sb_printf(o, "  RESULT: %d tracks of the data area change from one read to the next.\n\n", unst_dati);
    if (nsz) {
        sb_printf(o, "  In %d of them the SIZE DIFFERS from one read to the next.\n", nsz);
        sb_puts(o, "  In an EDSK a track grows when SAMdisk saves several copies of a\n");
        sb_puts(o, "  sector that gave different data on later rereads. It means that\n");
        sb_puts(o, "  SAMdisk ran into errors there and had to insist, even if it did\n");
        sb_puts(o, "  not tell you on screen: it is the sign of degraded media.\n");
        sb_puts(o, "  Check with:  samdisk scan FILENAME.DSK\n");
        sb_puts(o, "  and look for the m1, m2 or dc flags on the cylinders listed above.\n\n");
    }
    sb_puts(o, "  If the reads report dc or nd errors, the media is degraded:\n");
    sb_puts(o, "  reread with --retries=30 --rescans=5, clean the heads, try another drive.\n\n");
    sb_puts(o, "  If instead they report NO errors, these are weak sectors written\n");
    sb_puts(o, "  to be unstable on purpose, that is, copy protection. In that case\n");
    sb_puts(o, "  no amount of retries helps: you need a flux capture.\n\n");
    sb_puts(o, "  Where some reads agree you can keep the majority variant.\n\n");
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

    if (!a) { sb_puts(o, "  ERROR: out of memory\n"); return finish(r, V_ERROR); }
    b = a + 1;
    if (!edsk_load_as(fl[0], SHOWN(r, fl, 0), a, o)) { rc = finish(r, V_ERROR); goto out; }
    if (!edsk_load_as(fl[1], SHOWN(r, fl, 1), b, o)) { rc = finish(r, V_ERROR); goto out; }

    fmt_thousands(a->len, s1); fmt_thousands(b->len, s2);
    sb_printf(o, "  A: %-24s %14s bytes", SHOWN(r, fl, 0), s1);
    if (a->ok) sb_printf(o, "   %d cylinders, %d sides", a->cyls, a->heads);
    sb_puts(o, "\n");
    sb_printf(o, "  B: %-24s %14s bytes", SHOWN(r, fl, 1), s2);
    if (b->ok) sb_printf(o, "   %d cylinders, %d sides", b->cyls, b->heads);
    sb_puts(o, "\n");

    if (a->ok && a->nomcyl) {
        sb_printf(o, "  formatted tracks: %d cylinders", a->nomcyl);
        if (a->nsec0) sb_printf(o, ", %d sectors per track", a->nsec0);
        sb_puts(o, "\n");
        if (edsk_snap(a->nomcyl) != a->nomcyl)
            sb_printf(o, "  standard area checked: 0-%d  (the rest is extra territory)\n",
                      edsk_snap(a->nomcyl) - 1);
    } else if (a->ok) {
        sb_puts(o, "  WARNING: no formatted track recognized.\n");
        sb_puts(o, "           Using the classic 80-cylinder threshold.\n");
    }
    sb_puts(o, "\n");

    if (a->len != b->len) {
        sb_puts(o, "  WARNING: the files have different sizes.\n");
        if (a->ok && b->ok && a->cyls != b->cyls)
            sb_printf(o, "           The two reads captured a different number of\n"
                         "           cylinders (%d versus %d). For a clean comparison\n"
                         "           reread both with -c80.\n", a->cyls, b->cyls);
        sb_puts(o, "           Comparing only the part the two files share.\n\n");
    }

    stdcyl = forced ? forced : (a->nomcyl ? edsk_snap(a->nomcyl) : 80);
    geometry(r, a, stdcyl);
    if (forced) sb_printf(o, "  threshold forced to %d cylinders\n\n", stdcyl);

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
        sb_puts(o, "  RESULT: the two files are IDENTICAL.\n\n"
                   "  The read is perfectly reproducible. Catalog it as clean.\n\n");
        r->agree = 2;
        rc = finish(r, V_IDENTICAL); goto out;
    }

    fmt_thousands(ndiff, s1);
    sb_printf(o, "  differing bytes: %s", s1);
    if (firstdiff >= 0) sb_printf(o, "   (the first at 0x%08lX)", firstdiff);
    sb_puts(o, "\n\n");

    if (nhdr) {
        fmt_thousands(nhdr, s1);
        sb_printf(o, "  header .................... %s\n", s1);
    }
    shown = 0;
    for (t = 0; t < EDSK_MAXTRK; t++) {
        if (!cnt[t]) continue;
        if (map->cyl[t] < stdcyl) unstable++;
        if (shown == 24) { sb_puts(o, "  ... and more tracks\n"); shown++; continue; }
        if (shown > 24) continue;
        fmt_thousands(cnt[t], s1);
        sb_printf(o, "  cyl %2d side %d ............. %s%s\n",
                  map->cyl[t], map->head[t], s1,
                  map->cyl[t] >= stdcyl ? "   (extra cylinder)" : "");
        shown++;
    }
    sb_puts(o, "\n");
    r->unstable_tracks = unstable;

    if (!map->ok) {
        sb_puts(o, "  RESULT: cannot map the tracks in these files.\n\n");
        r->agree = 1;
        rc = finish(r, V_UNMAPPED); goto out;
    }

    if (!worst) {
        sb_printf(o, "  RESULT: cylinders 0-%d are IDENTICAL.\n\n", stdcyl - 1);
        sb_puts(o, "  All the differences are in the extra cylinders or in the header.\n");
        sb_puts(o, "  There the disk is not formatted and the head reads magnetic noise,\n");
        sb_puts(o, "  different on every pass: it says nothing about the media.\n\n");
        sb_puts(o, "  The disk is stable. Catalog it as clean.\n\n");
        r->agree = 2; r->unstable_tracks = 0;
        rc = finish(r, V_NOISE_ONLY); goto out;
    }

    sb_puts(o, "  RESULT: there are differences in the disk DATA.\n\n");
    sb_puts(o, "  The two reads of the same floppy do not match on cylinders\n");
    sb_puts(o, "  inside the standard area: the signal is marginal and the controller\n");
    sb_puts(o, "  reads it differently on every pass.\n\n");
    sb_puts(o, "  Reread with  --retries=30 --rescans=5  and catalog it as marginal.\n\n");
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
        sb_printf(&r->detail, "  ERROR: 2 to %d reads are needed\n", COMPARE_MAXF);
        return finish(r, V_ERROR);
    }
    return n > 2 ? multi(paths, n, forced_cyl, r) : pair(paths, forced_cyl, r);
}
