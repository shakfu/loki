/* test_spawn.c - loki.spawn: pipes, exit status, kill, errors, shutdown */

#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "spawn.h"
#include "lua_helpers.h"
#include <signal.h>
#include <sys/wait.h>

static lua_State *L;

#define PUMP_MS 5000

TEST(spawn_collects_output_then_exit) {
    ASSERT_LUA(L,
        "done = false; out = ''; err = ''\n"
        "local p = assert(loki.spawn{cmd = 'sh',\n"
        "  args = {'-c', 'printf hi; printf oops >&2; exit 3'},\n"
        "  on_stdout = function(_, s) out = out .. s end,\n"
        "  on_stderr = function(_, s) err = err .. s end,\n"
        "  on_exit = function(_, code, sig)\n"
        "    status = code; signal = sig\n"
        "    out_at_exit = out; done = true\n"
        "  end})\n"
        "assert(p:pid() > 0)");
    ASSERT_TRUE(lua_test_pump(L, PUMP_MS));
    ASSERT_LUA(L,
        "assert(out_at_exit == 'hi', out_at_exit)\n"
        "assert(err == 'oops', err)\n"
        "assert(status == 3 and signal == nil)");
}

TEST(spawn_stdin_round_trip) {
    ASSERT_LUA(L,
        "done = false; out = ''\n"
        "p = assert(loki.spawn{cmd = 'cat',\n"
        "  on_stdout = function(_, s) out = out .. s end,\n"
        "  on_exit = function() done = true end})\n"
        "assert(p:write('line one\\n'))\n"
        "assert(p:write('line two\\n'))\n"
        "p:close_stdin()\n"
        "local ok, msg = p:write('late')\n"
        "assert(not ok and msg == 'stdin is closed')");
    ASSERT_TRUE(lua_test_pump(L, PUMP_MS));
    ASSERT_LUA(L, "assert(out == 'line one\\nline two\\n', out)");
}

TEST(spawn_kill_reports_signal) {
    ASSERT_LUA(L,
        "done = false\n"
        "p = assert(loki.spawn{cmd = 'sleep', args = {'30'},\n"
        "  on_exit = function(_, code, sig) signal = sig; done = true end})\n"
        "assert(p:running())\n"
        "assert(p:kill('TERM'))");
    ASSERT_TRUE(lua_test_pump(L, PUMP_MS));
    ASSERT_LUA(L,
        "assert(signal == 15, tostring(signal))\n"
        "assert(not p:running())\n"
        "assert(not p:kill())");
}

TEST(spawn_missing_command_returns_nil) {
    ASSERT_LUA(L,
        "local p, err = loki.spawn{cmd = 'loki-no-such-command'}\n"
        "assert(p == nil and err:find('loki%-no%-such%-command'), tostring(err))");
}

/* A failed spawn has pid 0 until its handles close; shutdown must not
 * send kill(0, SIGTERM), which signals the whole process group. */
TEST(spawn_shutdown_after_failed_spawn_spares_group) {
    ASSERT_LUA(L, "assert(loki.spawn{cmd = 'loki-no-such-command'} == nil)");
    /* Block SIGTERM so a regression shows up as pending, not as death. */
    sigset_t term, old, pending;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    sigprocmask(SIG_BLOCK, &term, &old);
    loki_spawn_shutdown();
    sigpending(&pending);
    int got_term = sigismember(&pending, SIGTERM);
    if (got_term) signal(SIGTERM, SIG_IGN);  /* discard it on unblock */
    sigprocmask(SIG_SETMASK, &old, NULL);
    signal(SIGTERM, SIG_DFL);
    for (int i = 0; i < 50 && loki_spawn_count() > 0; i++)
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    ASSERT_FALSE(got_term);
}

TEST(spawn_rejects_bad_arguments) {
    ASSERT_LUA(L,
        "assert(not pcall(loki.spawn, {cmd = 'true', on_exit = 42}))\n"
        "assert(not pcall(loki.spawn, {cmd = 'true', args = {{}}}))\n"
        "assert(not pcall(loki.spawn, {}))");
}

TEST(spawn_env_and_cwd) {
    ASSERT_LUA(L,
        "done = false; out = ''\n"
        "assert(loki.spawn{cmd = 'sh', args = {'-c', 'printf \"%s:%s:%s\" \"$LOKI_T\" \"$HOME\" \"$(pwd)\"'},\n"
        "  cwd = '/', env = {LOKI_T = 'set', HOME = false},\n"
        "  on_stdout = function(_, s) out = out .. s end,\n"
        "  on_exit = function() done = true end})");
    ASSERT_TRUE(lua_test_pump(L, PUMP_MS));
    ASSERT_LUA(L, "assert(out == 'set::/', out)");
}

TEST(spawn_child_has_no_controlling_tty) {
    /* A new session cannot open /dev/tty, so it cannot fight the editor
     * for the terminal. Vacuous where the test itself has no tty. */
    ASSERT_LUA(L,
        "done = false\n"
        "assert(loki.spawn{cmd = 'sh', args = {'-c', 'exec 3</dev/tty'},\n"
        "  on_exit = function(_, code) status = code; done = true end})");
    ASSERT_TRUE(lua_test_pump(L, PUMP_MS));
    ASSERT_LUA(L, "assert(status ~= 0)");
}

TEST(spawn_callback_error_does_not_stop_process) {
    ASSERT_LUA(L,
        "done = false\n"
        "assert(loki.spawn{cmd = 'sh', args = {'-c', 'echo x'},\n"
        "  on_stdout = function() error('boom') end,\n"
        "  on_exit = function() done = true end})");
    ASSERT_TRUE(lua_test_pump(L, PUMP_MS));
}

TEST(spawn_handles_released_after_exit) {
    /* All earlier children have finished; their handles must be closed. */
    for (int i = 0; i < 50 && loki_spawn_count() > 0; i++)
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    ASSERT_EQ(loki_spawn_count(), 0);
}

TEST(spawn_shutdown_kills_children) {
    ASSERT_LUA(L,
        "victim = assert(loki.spawn{cmd = 'sleep', args = {'30'}})\n"
        "victim_pid = victim:pid()");
    lua_getglobal(L, "victim_pid");
    int pid = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    loki_spawn_shutdown();
    for (int i = 0; i < 50 && loki_spawn_count() > 0; i++)
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    ASSERT_EQ(loki_spawn_count(), 0);
    /* Closing the handle stops libuv reaping it; reap here to see how the
     * child ended. */
    int st = 0, reaped = 0;
    for (int i = 0; i < 200 && !reaped; i++) {
        reaped = waitpid(pid, &st, WNOHANG) == pid;
        if (!reaped) nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
    }
    ASSERT_TRUE(reaped);
    ASSERT_TRUE(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
}

BEGIN_TEST_SUITE("loki.spawn")
    L = lua_test_state(NULL);
    RUN_TEST(spawn_collects_output_then_exit);
    RUN_TEST(spawn_stdin_round_trip);
    RUN_TEST(spawn_kill_reports_signal);
    RUN_TEST(spawn_missing_command_returns_nil);
    RUN_TEST(spawn_shutdown_after_failed_spawn_spares_group);
    RUN_TEST(spawn_rejects_bad_arguments);
    RUN_TEST(spawn_env_and_cwd);
    RUN_TEST(spawn_child_has_no_controlling_tty);
    RUN_TEST(spawn_callback_error_does_not_stop_process);
    RUN_TEST(spawn_handles_released_after_exit);
    RUN_TEST(spawn_shutdown_kills_children);
    lua_close(L);
END_TEST_SUITE()
