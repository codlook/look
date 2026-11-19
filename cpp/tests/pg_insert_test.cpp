// PostgreSQL INSERT identity — look/pg_insert.h table test (no server). Live behavior:
// tests/behavioral/pg_insert_id.lk (real postgres:16).
//   Build: g++ -std=c++17 -Iinclude tests/pg_insert_test.cpp -o /tmp/pgins && /tmp/pgins
#include "look/pg_insert.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
static void eq(const char* what, const std::string& got, const std::string& want) {
    ran++;
    bool ok = (got == want);
    printf("  %-48s -> [%s]  %s\n", what, got.c_str(), ok ? "OK" : ("FAIL want [" + want + "]").c_str());
    if (!ok) fails++;
}
static void yes(const char* what, bool got, bool want) { eq(what, got ? "true" : "false", want ? "true" : "false"); }

int main() {
    printf("INSERT tespiti:\n");
    yes("INSERT INTO t VALUES (1)",          pg_is_insert("INSERT INTO t VALUES (1)"), true);
    yes("  insert into t ... (whitespace, lowercase)", pg_is_insert("  insert into t values (1)"), true);
    yes("INSERTX ... (a different word)",          pg_is_insert("INSERTX INTO t"), false);
    yes("SELECT ...",                          pg_is_insert("SELECT 1"), false);
    yes("UPDATE ...",                          pg_is_insert("UPDATE t SET a = 1"), false);

    printf("Target table:\n");
    eq("INSERT INTO t (a) VALUES (?)",          pg_insert_target("INSERT INTO t (a) VALUES (?)"), "t");
    eq("INSERT INTO t(a) VALUES (?)",           pg_insert_target("INSERT INTO t(a) VALUES (?)"), "t");
    eq("insert into public.orders values",      pg_insert_target("insert into public.orders values (1)"), "public.orders");
    eq("INSERT INTO \"Lid Q\" (v)",             pg_insert_target("INSERT INTO \"Lid Q\" (v) VALUES (1)"), "\"Lid Q\"");
    eq("INSERT INTO s.\"A\"\"b\" (v)",          pg_insert_target("INSERT INTO s.\"A\"\"b\" (v) VALUES (1)"), "s.\"A\"\"b\"");
    eq("INSERT INTO tbl_$1 DEFAULT VALUES",     pg_insert_target("INSERT INTO tbl_$1 DEFAULT VALUES"), "tbl_$1");
    eq("INSERT INTO t",                         pg_insert_target("INSERT INTO t"), "t");
    eq("unrecognized: INSERT INTO (SELECT …)",    pg_insert_target("INSERT INTO (x)"), "");
    eq("unrecognized: INSERT INTO t;DROP …",      pg_insert_target("INSERT INTO t;DROP TABLE x"), "");
    eq("unrecognized: unclosed quote",         pg_insert_target("INSERT INTO \"abc VALUES"), "");
    eq("unrecognized: empty quote \"\"",           pg_insert_target("INSERT INTO \"\" VALUES (1)"), "");
    eq("WITH … INSERT (CTE) → none",             pg_insert_target("WITH x AS (SELECT 1) INSERT INTO t SELECT * FROM x"), "");

    printf("The user's RETURNING:\n");
    yes("… RETURNING id",                       pg_has_returning("INSERT INTO t (v) VALUES (1) RETURNING id"), true);
    yes("… returning *",                        pg_has_returning("insert into t values (1) returning *"), true);
    yes("RETURNING inside a string literal ('RETURNING')", pg_has_returning("INSERT INTO t (v) VALUES ('RETURNING')"), false);
    yes("RETURNING inside a quoted name",         pg_has_returning("INSERT INTO \"returning\" (v) VALUES (1)"), false);
    yes("RETURNING yorumda (-- returning)",     pg_has_returning("INSERT INTO t VALUES (1) -- returning x"), false);
    yes("RETURNING yorumda (/* returning */)",  pg_has_returning("INSERT INTO t /* returning */ VALUES (1)"), false);
    yes("returning_id column (not the keyword)",   pg_has_returning("INSERT INTO t (returning_id) VALUES (1)"), false);

    printf("RETURNING append:\n");
    eq("trailing ; and whitespace are dropped",            pg_with_returning("INSERT INTO t VALUES (1); \n", "id"), "INSERT INTO t VALUES (1)\nRETURNING \"id\"");
    eq("quoted column name is escaped",          pg_with_returning("INSERT INTO t VALUES (1)", "a\"b"), "INSERT INTO t VALUES (1)\nRETURNING \"a\"\"b\"");
    eq("SQL ending in a -- comment (newline)",     pg_with_returning("INSERT INTO t VALUES (1) -- not", "id"), "INSERT INTO t VALUES (1) -- not\nRETURNING \"id\"");

    printf("%s: %d/%d\n", fails ? "FAIL" : "PASS", ran - fails, ran);
    return fails ? 1 : 0;
}
