/* test_trust.c - Trust list gating of project-local Lua config */

#include "test_framework.h"
#include "loki/core.h"
#include "loki/lua.h"
#include "trust.h"
#include <lua.h>
#include <lauxlib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_home[64];
static char g_project[64];
static char g_orig_cwd[PATH_MAX];

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* Fresh HOME and project directory per test. The project ships a .loki
 * init.lua and a module in its root, both of which set globals. */
static void setup(void) {
    char path[PATH_MAX];
    snprintf(g_home, sizeof(g_home), "/tmp/loki_trust_home_XXXXXX");
    snprintf(g_project, sizeof(g_project), "/tmp/loki_trust_proj_XXXXXX");
    if (!mkdtemp(g_home) || !mkdtemp(g_project)) abort();
    setenv("HOME", g_home, 1);

    snprintf(path, sizeof(path), "%s/.loki", g_project);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/.loki/init.lua", g_project);
    write_file(path, "PROJECT_INIT_RAN = true\n");
    snprintf(path, sizeof(path), "%s/probe_mod.lua", g_project);
    write_file(path, "PROBE_MOD_RAN = true\nreturn {}\n");

    if (!getcwd(g_orig_cwd, sizeof(g_orig_cwd))) abort();
    if (chdir(g_project) != 0) abort();
}

static void teardown(void) {
    char cmd[256];
    if (chdir(g_orig_cwd) != 0) abort();
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", g_home, g_project);
    (void)!system(cmd);
}

static lua_State *bootstrap(void) {
    struct loki_lua_opts opts = {0};
    opts.load_config = 1;
    return loki_lua_bootstrap(NULL, &opts);
}

static int global_is_true(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    int v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

TEST(trust_list_starts_empty) {
    setup();
    ASSERT_FALSE(loki_trust_is_trusted("."));
    teardown();
}

TEST(trust_add_then_check) {
    setup();
    ASSERT_EQ(loki_trust_add("."), 0);
    ASSERT_TRUE(loki_trust_is_trusted("."));
    ASSERT_TRUE(loki_trust_is_trusted(g_project));   /* same dir, other spelling */
    ASSERT_FALSE(loki_trust_is_trusted(g_home));
    teardown();
}

TEST(trust_add_is_idempotent) {
    setup();
    ASSERT_EQ(loki_trust_add("."), 0);
    ASSERT_EQ(loki_trust_add(g_project), 0);

    char path[PATH_MAX], line[PATH_MAX + 2];
    snprintf(path, sizeof(path), "%s/.loki/trusted", g_home);
    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);
    int lines = 0;
    while (fgets(line, sizeof(line), f)) lines++;
    fclose(f);
    ASSERT_EQ(lines, 1);
    teardown();
}

TEST(untrusted_project_config_does_not_run) {
    setup();
    lua_State *L = bootstrap();
    ASSERT_NOT_NULL(L);
    ASSERT_FALSE(global_is_true(L, "PROJECT_INIT_RAN"));

    /* require() must not resolve to files in the checkout either. */
    ASSERT_NEQ(luaL_dostring(L, "require('probe_mod')"), LUA_OK);
    ASSERT_FALSE(global_is_true(L, "PROBE_MOD_RAN"));
    lua_close(L);
    teardown();
}

TEST(trusted_project_config_runs) {
    setup();
    ASSERT_EQ(loki_trust_add("."), 0);
    lua_State *L = bootstrap();
    ASSERT_NOT_NULL(L);
    ASSERT_TRUE(global_is_true(L, "PROJECT_INIT_RAN"));
    lua_close(L);
    teardown();
}

TEST(untrusted_project_falls_back_to_home_config) {
    setup();
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.loki", g_home);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/.loki/init.lua", g_home);
    write_file(path, "HOME_INIT_RAN = true\n");

    lua_State *L = bootstrap();
    ASSERT_NOT_NULL(L);
    ASSERT_TRUE(global_is_true(L, "HOME_INIT_RAN"));
    ASSERT_FALSE(global_is_true(L, "PROJECT_INIT_RAN"));
    lua_close(L);
    teardown();
}

BEGIN_TEST_SUITE("Trust list")
    RUN_TEST(trust_list_starts_empty);
    RUN_TEST(trust_add_then_check);
    RUN_TEST(trust_add_is_idempotent);
    RUN_TEST(untrusted_project_config_does_not_run);
    RUN_TEST(trusted_project_config_runs);
    RUN_TEST(untrusted_project_falls_back_to_home_config);
END_TEST_SUITE()
