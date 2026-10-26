/* The record of an upload: see iarec.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iarec.h"
#include "fsu8.h"
#include "json.h"
#include "messages.h"
#include "sha256.h"

#ifdef _WIN32
#include <io.h>
#define SYNC_FILE(f) _commit(_fileno(f))
#else
#include <unistd.h>
#define SYNC_FILE(f) fsync(fileno(f))
#endif

#define CP(dst, src) str_copy(dst, sizeof(dst), src)

static const char *const state_keys[] = {
    "", "interrupted", "pending", "verified", "failed", "outdated"
};

const char *ia_state_key(int state)
{
    return state >= IA_ST_NONE && state <= IA_ST_OUTDATED ? state_keys[state] : "";
}

int ia_state_parse(const char *key)
{
    int i;
    for (i = IA_ST_INTERRUPTED; key && i <= IA_ST_OUTDATED; i++)
        if (!strcmp(key, state_keys[i])) return i;
    return IA_ST_NONE;
}

const char *ia_what_key(int what)
{
    static const char *const keys[] = { "same", "new", "changed", "removed" };
    return what >= IA_F_SAME && what <= IA_F_REMOVED ? keys[what] : "same";
}

/* ---- the list of files ---- */

void ia_files_free(ia_files *p)
{
    free(p->f);
    memset(p, 0, sizeof(*p));
}

ia_file *ia_files_add(ia_files *p, const ia_file *f)
{
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 32;
        ia_file *nf = (ia_file *)realloc(p->f, (size_t)cap * sizeof(ia_file));
        if (!nf) return NULL;
        p->f = nf; p->cap = cap;
    }
    p->f[p->n] = *f;
    return &p->f[p->n++];
}

const ia_file *ia_files_find(const ia_files *p, const char *name)
{
    int i;
    for (i = 0; i < p->n; i++) if (!strcmp(p->f[i].name, name)) return &p->f[i];
    return NULL;
}

long long ia_files_bytes(const ia_files *p, int what_mask)
{
    long long sum = 0; int i;
    for (i = 0; i < p->n; i++)
        if (what_mask < 0 || (what_mask & (1 << p->f[i].what))) sum += p->f[i].size;
    return sum;
}

/* ---- the record ---- */

void ia_record_init(ia_record *r)
{
    memset(r, 0, sizeof(*r));
}

void ia_record_free(ia_record *r)
{
    ia_files_free(&r->files);
    memset(r, 0, sizeof(*r));
}

int ia_record_event(ia_record *r, const char *entry)
{
    size_t have = strlen(r->events), add = strlen(entry);
    if (have + add + (have ? 1 : 0) + 1 > sizeof(r->events)) return -1;
    if (have) r->events[have++] = ',';
    memcpy(r->events + have, entry, add + 1);
    return 0;
}

void ia_upload_dir(const char *bagdir, char *out, size_t n)
{
    u8_join(out, n, bagdir, IA_UPLOAD_DIR);
}

static void in_upload_dir(const char *bagdir, const char *name, char *out, size_t n)
{
    char dir[1024];
    ia_upload_dir(bagdir, dir, sizeof(dir));
    u8_join(out, n, dir, name);
}

void ia_record_path(const char *bagdir, char *out, size_t n)
{
    in_upload_dir(bagdir, IA_RECORD_NAME, out, n);
}

void ia_journal_path(const char *bagdir, char *out, size_t n)
{
    in_upload_dir(bagdir, IA_JOURNAL_NAME, out, n);
}

static void write_meta(json_writer *w, const ia_meta *m)
{
    int i;
    jw_begin_obj(w, "metadata");
    jw_str(w, "mediatype", m->mediatype);
    jw_str(w, "collection", m->collection);
    jw_str(w, "title", m->title);
    jw_str(w, "creator", m->creator);
    jw_str(w, "publisher", m->publisher);
    jw_str(w, "date", m->date);
    jw_str(w, "platform", m->platform);
    jw_str(w, "edition", m->edition);
    jw_str(w, "media", m->media);
    jw_str(w, "rights", m->rights);
    jw_str(w, "scanner", m->scanner);
    jw_str(w, "bagit-profile", m->profile);
    jw_begin_arr(w, "language");
    for (i = 0; i < m->nlang; i++) jw_str(w, NULL, m->language[i]);
    jw_end_arr(w);
    jw_begin_arr(w, "subject");
    for (i = 0; i < m->nsubj; i++) jw_str(w, NULL, m->subject[i]);
    jw_end_arr(w);
    jw_str(w, "description", m->description);
    jw_end_obj(w);
}

static void read_meta(const jval *o, ia_meta *m)
{
    const jval *a, *e;
    if (!o || o->t != J_OBJ) return;
    CP(m->mediatype, json_str(o, "mediatype"));
    CP(m->collection, json_str(o, "collection"));
    CP(m->title, json_str(o, "title"));
    CP(m->creator, json_str(o, "creator"));
    CP(m->publisher, json_str(o, "publisher"));
    CP(m->date, json_str(o, "date"));
    CP(m->platform, json_str(o, "platform"));
    CP(m->edition, json_str(o, "edition"));
    CP(m->media, json_str(o, "media"));
    CP(m->rights, json_str(o, "rights"));
    CP(m->scanner, json_str(o, "scanner"));
    CP(m->profile, json_str(o, "bagit-profile"));
    CP(m->description, json_str(o, "description"));
    a = json_get(o, "language");
    for (e = a && a->t == J_ARR ? a->child : NULL; e && m->nlang < IA_MAX_LANG; e = e->next)
        if (e->t == J_STR) CP(m->language[m->nlang++], e->s);
    a = json_get(o, "subject");
    for (e = a && a->t == J_ARR ? a->child : NULL; e && m->nsubj < IA_MAX_SUBJECT; e = e->next)
        if (e->t == J_STR) CP(m->subject[m->nsubj++], e->s);
}

int ia_record_save(const char *bagdir, const ia_record *r, strbuf *err)
{
    json_writer w; char dir[1024], path[1200], tmp[1240]; FILE *f; int i, ok;

    jw_init(&w);
    jw_begin_obj(&w, NULL);
    jw_str(&w, "schema", IA_RECORD_SCHEMA);
    jw_str(&w, "service", IA_SERVICE);
    jw_str(&w, "identifier", r->identifier);
    jw_str(&w, "url", r->url);
    jw_str(&w, "collection", r->collection);
    jw_str(&w, "created", r->created);
    jw_str(&w, "updated", r->updated[0] ? r->updated : NULL);
    jw_str(&w, "state", ia_state_key(r->state));
    jw_str(&w, "verified", r->verified[0] ? r->verified : NULL);
    jw_str(&w, "tagmanifest_sha256", r->tagmanifest);
    write_meta(&w, &r->meta);
    jw_begin_arr(&w, "files");
    for (i = 0; i < r->files.n; i++) {
        const ia_file *fl = &r->files.f[i];
        jw_begin_obj(&w, NULL);
        jw_str(&w, "name", fl->name);
        jw_int(&w, "size", fl->size);
        jw_str(&w, "sha256", fl->sha256);
        jw_str(&w, "md5", fl->md5);
        jw_str(&w, "sha1", fl->sha1);
        jw_str(&w, "uploaded", fl->at[0] ? fl->at : NULL);
        jw_end_obj(&w);
    }
    jw_end_arr(&w);
    /* the events are already written objects: a record of a newer version
     * keeps the ones it had */
    if (r->events[0]) {
        char buf[sizeof(r->events) + 4], perr[160]; jval *ev;
        snprintf(buf, sizeof(buf), "[%s]", r->events);
        if ((ev = json_parse(buf, perr, sizeof(perr))) != NULL) json_write_val(&w, "events", ev);
        json_free(ev);
    }
    jw_end_obj(&w);

    ia_upload_dir(bagdir, dir, sizeof(dir));
    ia_record_path(bagdir, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    ok = u8_mkdirs(dir) == 0;
    f = ok ? u8_fopen(tmp, "wb") : NULL;
    ok = f && fwrite(w.out.s, 1, w.out.len, f) == w.out.len;
    if (ok && (fflush(f) != 0 || SYNC_FILE(f) != 0)) ok = 0;
    if (f && fclose(f) != 0) ok = 0;
    if (ok && u8_rename_replace(tmp, path) != 0) ok = 0;
    if (!ok) {
        if (err) sb_printf(err, M_META_WRITE, path);
        u8_remove(tmp);
    }
    jw_free(&w);
    return ok ? 0 : -1;
}

static char *slurp(const char *path)
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

int ia_record_load(const char *bagdir, ia_record *r)
{
    char path[1200]; char *text; jval *root; const jval *a, *e;

    ia_record_init(r);
    ia_record_path(bagdir, path, sizeof(path));
    if ((text = slurp(path)) == NULL) return 0;
    root = json_parse(text, NULL, 0);
    free(text);
    if (!root) return 0;
    CP(r->identifier, json_str(root, "identifier"));
    CP(r->url, json_str(root, "url"));
    CP(r->collection, json_str(root, "collection"));
    CP(r->created, json_str(root, "created"));
    CP(r->updated, json_str(root, "updated"));
    CP(r->verified, json_str(root, "verified"));
    CP(r->tagmanifest, json_str(root, "tagmanifest_sha256"));
    r->state = ia_state_parse(json_str(root, "state"));
    read_meta(json_get(root, "metadata"), &r->meta);
    a = json_get(root, "files");
    for (e = a && a->t == J_ARR ? a->child : NULL; e; e = e->next) {
        ia_file fl;
        memset(&fl, 0, sizeof(fl));
        CP(fl.name, json_str(e, "name"));
        fl.size = json_int(e, "size", 0);
        CP(fl.sha256, json_str(e, "sha256"));
        CP(fl.md5, json_str(e, "md5"));
        CP(fl.sha1, json_str(e, "sha1"));
        CP(fl.at, json_str(e, "uploaded"));
        if (fl.name[0]) ia_files_add(&r->files, &fl);
    }
    a = json_get(root, "events");
    if (a && a->t == J_ARR) {
        json_writer w;
        for (e = a->child; e; e = e->next) {
            jw_init(&w);
            json_write_val(&w, NULL, e);
            if (w.out.s) ia_record_event(r, w.out.s);
            jw_free(&w);
        }
    }
    json_free(root);
    return r->identifier[0] != 0;
}

int ia_tagmanifest_digest(const char *bagdir, char hex[65])
{
    char path[1200];
    hex[0] = 0;
    u8_join(path, sizeof(path), bagdir, "tagmanifest-sha256.txt");
    return sha256_file(path, hex, NULL);
}

int ia_box_state(const char *bagdir, ia_record *r)
{
    ia_record own; char path[1200], hex[65]; int state;
    ia_record *rec = r ? r : &own;

    if (!ia_record_load(bagdir, rec)) {
        if (!r) ia_record_free(rec);
        return IA_ST_NONE;
    }
    state = rec->state;
    ia_journal_path(bagdir, path, sizeof(path));
    if (u8_exists(path)) state = IA_ST_INTERRUPTED;
    else if ((state == IA_ST_VERIFIED || state == IA_ST_PENDING) &&
             rec->tagmanifest[0] && ia_tagmanifest_digest(bagdir, hex) == 0 &&
             strcmp(hex, rec->tagmanifest))
        state = IA_ST_OUTDATED;
    if (!r) ia_record_free(rec);
    return state;
}
