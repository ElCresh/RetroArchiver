#include <limits.h>
#include "unit.h"
#include "core/json.h"

static const char *expected =
    "{\n"
    "  \"titolo\": \"Citt\xc3\xa0 \\\"perduta\\\"\",\n"
    "  \"dischi\": 4,\n"
    "  \"chiuso\": null,\n"
    "  \"letture\": [\n"
    "    {\n"
    "      \"file\": \"a.dsk\"\n"
    "    }\n"
    "  ],\n"
    "  \"vuoto\": []\n"
    "}\n";

void test_json(void)
{
    json_writer w; jval *root; const jval *arr; char err[160];

    jw_init(&w);
    jw_begin_obj(&w, NULL);
    jw_str(&w, "titolo", "Citt\xc3\xa0 \"perduta\"");
    jw_int(&w, "dischi", 4);
    jw_null(&w, "chiuso");
    jw_begin_arr(&w, "letture");
    jw_begin_obj(&w, NULL);
    jw_str(&w, "file", "a.dsk");
    jw_end_obj(&w);
    jw_end_arr(&w);
    jw_begin_arr(&w, "vuoto");
    jw_end_arr(&w);
    jw_end_obj(&w);
    CHECK_STR(w.out.s, expected);

    root = json_parse(w.out.s, err, sizeof(err));
    CHECK(root != NULL);
    CHECK_STR(json_str(root, "titolo"), "Citt\xc3\xa0 \"perduta\"");
    CHECK(json_int(root, "dischi", 0) == 4);
    CHECK(json_get(root, "chiuso") && json_get(root, "chiuso")->t == J_NULL);
    CHECK(json_str(root, "manca") == NULL);
    CHECK(json_int(root, "manca", -7) == -7);
    arr = json_get(root, "letture");
    CHECK(arr && arr->t == J_ARR && arr->child);
    if (arr && arr->child) CHECK_STR(json_str(arr->child, "file"), "a.dsk");
    json_free(root);
    jw_free(&w);

    root = json_parse("{\"a\": \"\\u00e0\\ud83d\\ude00\\n\", \"b\": [true, false, -1.5]}", err, sizeof(err));
    CHECK(root != NULL);
    CHECK_STR(json_str(root, "a"), "\xc3\xa0\xf0\x9f\x98\x80\n");
    json_free(root);

    CHECK(json_parse("{\"a\": }", err, sizeof(err)) == NULL);
    CHECK(err[0] != 0);
    CHECK(json_parse("[1, 2", err, sizeof(err)) == NULL);
    CHECK(json_parse("{} x", err, sizeof(err)) == NULL);
    CHECK(json_str(NULL, "a") == NULL);

    /* corrupt input: NULL and a message in err */
    CHECK(json_parse("{\"a\": \"abc", err, sizeof(err)) == NULL);
    CHECK(err[0] != 0);
    CHECK(json_parse("{\"a\": \"\\u12", err, sizeof(err)) == NULL);
    CHECK(err[0] != 0);
    CHECK(json_parse("{\"a\": \"abc\\", err, sizeof(err)) == NULL);
    CHECK(err[0] != 0);
    {
        char deep[81]; int i;
        for (i = 0; i < 40; i++) { deep[i] = '['; deep[40 + i] = ']'; }
        deep[80] = 0;
        CHECK(json_parse(deep, err, sizeof(err)) == NULL);
        CHECK(err[0] != 0);
    }

    /* sanity: a bare string at root level is a J_STR */
    root = json_parse("\"x\"", err, sizeof(err));
    CHECK(root != NULL && root->t == J_STR);
    CHECK_STR(root ? root->s : NULL, "x");
    json_free(root);

    /* out-of-range numbers: they saturate instead of an undefined cast */
    root = json_parse("{\"n\": 1e30}", err, sizeof(err));
    CHECK(root != NULL);
    CHECK(json_int(root, "n", 0) == LLONG_MAX);
    json_free(root);
    root = json_parse("{\"n\": 9.21e18}", err, sizeof(err));
    CHECK(root != NULL);
    CHECK(json_int(root, "n", 0) != LLONG_MAX);
    json_free(root);

    /* writing beyond JSON_MAXDEPTH: the subtree is dropped, the JSON stays valid */
    {
        json_writer w2; int i;
        jw_init(&w2);
        for (i = 0; i < 40; i++) jw_begin_arr(&w2, NULL);
        jw_int(&w2, NULL, 7);
        for (i = 0; i < 40; i++) jw_end_arr(&w2);
        CHECK(w2.overflow == 1);
        root = json_parse(w2.out.s, err, sizeof(err));
        CHECK(root != NULL);
        json_free(root);
        jw_free(&w2);
    }
}
