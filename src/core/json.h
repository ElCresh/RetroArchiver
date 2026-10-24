/* json - indented writing and minimal reading, just enough for
 * metadata.json. Strings are UTF-8 and pass through as they are. */
#ifndef JSON_H
#define JSON_H
#include <stddef.h>
#include "strbuf.h"

#define JSON_MAXDEPTH 32

typedef struct {
    strbuf out;
    int depth;
    int count[JSON_MAXDEPTH];    /* elements already written at each level */
    int skipped;                 /* openings beyond the limit, to close without going up */
    int overflow;                /* 1 if the structure exceeded JSON_MAXDEPTH */
} json_writer;

void jw_init(json_writer *w);
void jw_free(json_writer *w);
/* key = NULL inside an array or for the root */
void jw_begin_obj(json_writer *w, const char *key);
void jw_end_obj(json_writer *w);
void jw_begin_arr(json_writer *w, const char *key);
void jw_end_arr(json_writer *w);
void jw_str(json_writer *w, const char *key, const char *val);  /* NULL -> null */
void jw_int(json_writer *w, const char *key, long long v);
void jw_null(json_writer *w, const char *key);
void jw_bool(json_writer *w, const char *key, int v);

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype t;
    char *key;              /* member name, if inside an object */
    char *s;                /* J_STR */
    double n;               /* J_NUM */
    int b;                  /* J_BOOL */
    struct jval *child;     /* first element of J_ARR and J_OBJ */
    struct jval *next;      /* next element */
} jval;

/* NULL if the text is not valid JSON; err says where */
jval *json_parse(const char *text, char *err, size_t errlen);
void  json_free(jval *v);
const jval *json_get(const jval *obj, const char *key);
const char *json_str(const jval *obj, const char *key);  /* NULL if missing */
long long   json_int(const jval *obj, const char *key, long long def);
/* writes again a value that was read (an object, an array...) */
void json_write_val(json_writer *w, const char *key, const jval *v);

#endif
