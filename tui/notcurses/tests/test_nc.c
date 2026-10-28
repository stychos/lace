#include "test.h"
#include "conn/connection.h"
#include "conn/mainq.h"
#include "model/page_cache.h"
#include "model/view.h"
#include "model/workspace.h"
#include "session.h"
#include "config/workspace_json.h"
#include "platform/platform.h"
#include "util/json_helpers.h"
#include "core/utf8.h"
#include "app.h"
#include "edit_ops.h"
#include "undo.h"
#include "ui/textbuf.h"
#include "core/sqlhl.h"
#include "core/conn_fields.h"
#include "ui/form_tab.h"
#include "ui/header.h"
#include "ui/input.h"
#include "ui/list.h"
#include "ui/palette.h"
#include "ui/sidebar.h"
#include "ui/tabstrip.h"
#include "ui/filter_panel.h"
#include "ui/form.h"
#include "util/mem.h"
#include "util/str.h"
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <notcurses/notcurses.h>
#include <locale.h>

static struct ncinput key(uint32_t id, unsigned mods) {
  struct ncinput ni = {0};
  ni.id = id;
  ni.modifiers = mods;
  ni.evtype = NCTYPE_PRESS;
  return ni;
}

static void test_input_translate(void) {
  UiEvent e;
  /* Ctrl+G arrives as 'G' + CTRL from notcurses; 'g' + CTRL is accepted too */
  struct ncinput ni = key('g', NCKEY_MOD_CTRL);
  CHECK(input_translate(&ni, ni.id, &e));
  CHECK(e.type == UI_EVENT_KEY && !e.key.is_special && e.key.key == 'G' && e.key.mods == UI_MOD_CTRL);
  ni = key('G', NCKEY_MOD_CTRL);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == 'G' && e.key.mods == UI_MOD_CTRL);
  /* a held key repeats: its repeat events are presses; a release is nothing */
  ni = key('a', 0);
  ni.evtype = NCTYPE_REPEAT;
  CHECK(input_translate(&ni, ni.id, &e) && e.type == UI_EVENT_KEY && e.key.key == 'a');
  ni = key(NCKEY_BACKSPACE, 0);
  ni.evtype = NCTYPE_REPEAT;
  CHECK(input_translate(&ni, ni.id, &e) && e.key.is_special && e.key.key == UI_KEY_BACKSPACE);
  ni = key('a', 0);
  ni.evtype = NCTYPE_RELEASE;
  CHECK(!input_translate(&ni, ni.id, &e));
  ni = key('3', NCKEY_MOD_ALT);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == '3' && e.key.mods == UI_MOD_ALT);
  /* A legacy ESC-prefixed Alt sets only the deprecated bool */
  ni = key('3', 0);
  ni.alt = true;
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == '3' && e.key.mods == UI_MOD_ALT);
  ni = key(NCKEY_PGDOWN, NCKEY_MOD_CTRL);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.is_special && e.key.key == UI_KEY_PAGEDOWN && e.key.mods == UI_MOD_CTRL);
  ni = key(NCKEY_ENTER, NCKEY_MOD_SHIFT);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == UI_KEY_ENTER && e.key.mods == UI_MOD_SHIFT);
  /* Super (Cmd on macOS) is a modifier like Ctrl: Cmd+c is 'C' + SUPER, bound as "CMD+C" */
  ni = key('c', NCKEY_MOD_SUPER);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == 'C' && e.key.mods == UI_MOD_SUPER);
  {
    Config *c = config_get_defaults();
    c->frontend = CONFIG_FE_NOTCURSES;
    CHECK(hotkey_matches(c, &e, HOTKEY_CELL_COPY) && !hotkey_matches(c, &e, HOTKEY_CELL_PASTE));
    ni = key('z', NCKEY_MOD_SUPER);
    CHECK(input_translate(&ni, ni.id, &e) && hotkey_matches(c, &e, HOTKEY_UNDO));
    config_free(c);
  }
  /* Shift on a letter is its case, never a modifier */
  ni = key('x', NCKEY_MOD_SHIFT);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == 'X' && e.key.mods == UI_MOD_NONE);
  /* The kitty protocol reports Shift+1 as '1' + SHIFT with the typed '!'
   * only in eff_text; Shift+[ likewise, so a '{' hotkey still matches
   * (an older notcurses has no eff_text) */
#ifdef NCINPUT_MAX_EFF_TEXT_CODEPOINTS
  ni = key('1', NCKEY_MOD_SHIFT);
  ni.eff_text[0] = '!';
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == '!' && e.key.mods == UI_MOD_NONE && !e.key.text[0]);
  ni = key('[', NCKEY_MOD_SHIFT);
  ni.eff_text[0] = '{';
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == '{' && e.key.mods == UI_MOD_NONE);
  ni = key('a', 0); /* a layout whose text differs from the key: the text wins */
  ni.eff_text[0] = 0xE9;
  CHECK(input_translate(&ni, ni.id, &e) && strcmp(e.key.text, "\xc3\xa9") == 0 && e.key.key == 0xc3);
  ni = key('1', NCKEY_MOD_ALT); /* a modifier keeps the base key: Alt+1 is a binding */
  ni.eff_text[0] = '!';
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == '1' && e.key.mods == UI_MOD_ALT);
#endif
  ni = key(NCKEY_ESC, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.is_special && e.key.key == UI_KEY_ESCAPE);
  ni = key(NCKEY_TAB, NCKEY_MOD_SHIFT);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == UI_KEY_BTAB && e.key.mods == UI_MOD_NONE);
  ni = key(NCKEY_DEL, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == UI_KEY_DELETE);
  ni = key(NCKEY_F04, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == UI_KEY_F4);
  /* A multibyte character: the event carries the first byte and the text */
  ni = key(0xE9, 0); /* e acute */
  snprintf(ni.utf8, sizeof ni.utf8, "%s", "\xc3\xa9");
  CHECK(input_translate(&ni, ni.id, &e) && !e.key.is_special);
  CHECK(strcmp(e.key.text, "\xc3\xa9") == 0);
  /* Releases and repeats are dropped, mouse becomes a mouse event */
  ni = key('a', 0); ni.evtype = NCTYPE_RELEASE;
  CHECK(!input_translate(&ni, ni.id, &e));
  ni = key(NCKEY_BUTTON1, 0); ni.y = 4; ni.x = 7;
  CHECK(input_translate(&ni, ni.id, &e) && e.type == UI_EVENT_MOUSE && e.mouse.button == UI_MOUSE_LEFT && e.mouse.y == 4 && e.mouse.x == 7);
  ni = key(NCKEY_SCROLL_DOWN, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.mouse.button == UI_MOUSE_SCROLL_DOWN);
  ni = key(NCKEY_RESIZE, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.type == UI_EVENT_RESIZE);
  ni = key(NCKEY_EOF, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.type == UI_EVENT_QUIT);
  /* Legacy bytes */
  ni = key(0x7f, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.is_special && e.key.key == UI_KEY_BACKSPACE);
  ni = key(1, 0);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == 'A' && e.key.mods == UI_MOD_CTRL);
  ni = key(1, 0); ni.alt = true;
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == 'A' && e.key.mods == (UI_MOD_CTRL | UI_MOD_ALT));
  ni = key(0, 0);
  CHECK(!input_translate(&ni, ni.id, &e));
  /* Ctrl+Shift+letter keeps Shift */
  ni = key('p', NCKEY_MOD_CTRL | NCKEY_MOD_SHIFT);
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == 'P' && e.key.mods == (UI_MOD_CTRL | UI_MOD_SHIFT));
  /* A mouse release passes, a key release does not; every repeat is a press */
  ni = key(NCKEY_BUTTON1, 0); ni.evtype = NCTYPE_RELEASE;
  CHECK(input_translate(&ni, ni.id, &e) && e.type == UI_EVENT_MOUSE && e.mouse.action == UI_MOUSE_RELEASE);
  ni = key(NCKEY_DOWN, 0); ni.evtype = NCTYPE_REPEAT;
  CHECK(input_translate(&ni, ni.id, &e) && e.key.key == UI_KEY_DOWN);
  ni = key(NCKEY_DOWN, 0); ni.evtype = NCTYPE_RELEASE;
  CHECK(!input_translate(&ni, ni.id, &e));
  /* A non-ASCII id with Ctrl keeps its text */
  ni = key(0xE9, NCKEY_MOD_CTRL);
  snprintf(ni.utf8, sizeof ni.utf8, "%s", "\xc3\xa9");
  CHECK(input_translate(&ni, ni.id, &e) && e.key.mods == UI_MOD_CTRL && strcmp(e.key.text, "\xc3\xa9") == 0);
  /* Synthesized ids we do not know (F13, NCKEY_INVALID) are ignored */
  ni = key(NCKEY_F13, 0);
  CHECK(!input_translate(&ni, ni.id, &e));
  ni = key(NCKEY_INVALID, 0);
  CHECK(!input_translate(&ni, ni.id, &e));
}


static int g_bumped;
static void bump_g(void *d) { (void)d; g_bumped++; }
static void *poster(void *q) {
  for (int i = 0; i < 100; i++) mainq_post(q, bump_g, NULL);
  return NULL;
}

static void test_mainq(void) {
  MainQueue *q = mainq_new();
  CHECK(q && mainq_fd(q) >= 0);
  /* pipe() plus fcntl: close-on-exec and non-blocking */
  CHECK((fcntl(mainq_fd(q), F_GETFD) & FD_CLOEXEC) != 0);
  CHECK((fcntl(mainq_fd(q), F_GETFL) & O_NONBLOCK) != 0);
  g_bumped = 0;
  pthread_t t;
  pthread_create(&t, NULL, poster, q);
  pthread_join(t, NULL);
  CHECK_SIZE(mainq_drain(q), 100);
  CHECK(g_bumped == 100);
  mainq_free(q);
  q = mainq_new();
  g_bumped = 0;
  mainq_post(q, bump_g, NULL);
  mainq_post(q, bump_g, NULL);
  struct pollfd p = {mainq_fd(q), POLLIN, 0};
  CHECK(poll(&p, 1, 1000) == 1);
  CHECK_SIZE(mainq_drain(q), 2);
  CHECK(g_bumped == 2);
  CHECK(poll(&p, 1, 0) == 0); /* drained: nothing readable */
  mainq_post(q, bump_g, NULL); /* freed with an item queued: it runs */
  mainq_free(q);
  CHECK(g_bumped == 3);
}

/* A client on a private stdio daemon, HOME in a temp dir */
static lace_client_t *test_client(char *home_out, size_t cap) {
  snprintf(home_out, cap, "/tmp/lace-nc-XXXXXX");
  if (!mkdtemp(home_out)) return NULL;
  setenv("HOME", home_out, 1);
  setenv("XDG_RUNTIME_DIR", "", 1);
  char *log = str_printf("%s/laced.log", home_out);
  setenv("LACED_LOG", log, 1);
  free(log);
  LaceClientConfig c = {0};
  c.mode = LACE_CONN_AUTO;
  c.spawn_mode = LACE_SPAWN_STDIO;
  c.daemon_path = "../../laced/build/laced";
  lace_client_t *cl = lace_client_create_with_config(&c);
  if (!lace_client_connected(cl)) {
    fprintf(stderr, "daemon: %s\n", lace_client_error(cl));
    lace_client_destroy(cl);
    return NULL;
  }
  return cl;
}

typedef struct { int done; int code; int64_t affected; LaceResult *result; } Got;
static void on_done(NcJob *job, void *data) {
  Got *g = data;
  g->done++;
  g->code = job->code;
  g->affected = job->affected;
  lace_result_free(g->result);
  g->result = nc_job_steal_result(job);
}
static void drain_until(MainQueue *q, int *flag, int want) {
  for (int i = 0; i < 200 && *flag < want; i++) {
    struct pollfd p = {mainq_fd(q), POLLIN, 0};
    if (poll(&p, 1, 100) > 0) mainq_drain(q);
  }
}

static int g_destroyed, g_closed_ev;
static void count_destroy(void *d) { (void)d; g_destroyed++; }
static void on_event(NcConnection *c, NcConnEvent ev, const char *msg, void *d) {
  (void)c; (void)msg; (void)d;
  if (ev == NC_CONN_EV_CLOSED) g_closed_ev++;
}

static void test_connection_jobs(void) {
  char home[64];
  lace_client_t *cl = test_client(home, sizeof home);
  CHECK(cl != NULL);
  if (!cl) return;
  char *db = str_printf("%s/t.sqlite", home);
  char *url = str_printf("sqlite://%s?create=1", db);
  int id = -1;
  CHECK(lace_connect(cl, url, NULL, &id) == LACE_OK);
  MainQueue *q = mainq_new();
  NcConnection *c = nc_connection_new(cl, q, id, url, NULL, "t", LACE_HISTORY_MODE_SESSION, 100);
  nc_connection_set_listener(c, on_event, NULL);
  CHECK(nc_connection_driver(c) == LACE_DRIVER_SQLITE);
  CHECK_STR(nc_connection_name(c), "t");
  CHECK_STR_FREE(nc_connection_short_name(c), "t.sqlite");
  Got g = {0};
  NcOwner *owner = nc_owner_new();
  nc_connection_submit(c, nc_job_exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)"), owner, on_done, &g);
  nc_connection_submit(c, nc_job_exec("INSERT INTO t VALUES (1, 'a'), (2, 'b')"), owner, on_done, &g);
  CHECK(nc_connection_busy(c));
  drain_until(q, &g.done, 2);
  CHECK(g.done == 2 && g.code == LACE_OK && g.affected == 2);
  CHECK(!nc_connection_busy(c));
  /* Jobs run in order and read results come back */
  nc_connection_submit(c, nc_job_read("SELECT v FROM t ORDER BY id"), owner, on_done, &g);
  drain_until(q, &g.done, 3);
  CHECK(g.result && g.result->num_rows == 2);
  if (g.result && g.result->num_rows == 2) CHECK_STR(g.result->rows[1].cells[0].text.data, "b");
  /* Tables */
  nc_connection_refresh_tables(c);
  for (int i = 0; i < 200 && !nc_connection_tables_loaded(c); i++) {
    struct pollfd p = {mainq_fd(q), POLLIN, 0};
    if (poll(&p, 1, 100) > 0) mainq_drain(q);
  }
  size_t nt = 0;
  char **tabs = nc_connection_tables(c, &nt);
  CHECK(nt == 1 && tabs && strcmp(tabs[0], "t") == 0);
  /* Verified update through row keys */
  nc_connection_submit(c, nc_job_read("SELECT * FROM t ORDER BY id"), owner, on_done, &g);
  drain_until(q, &g.done, 4);
  LaceSchema *schema = NULL;
  CHECK(lace_get_schema(cl, nc_connection_id(c), "t", &schema) == LACE_OK);
  LaceRowKey key; LaceRowSnapshot snap; const char *why = NULL;
  CHECK(g.result != NULL);
  CHECK(lace_row_key_build(&key, g.result, 0, schema, false, &why) == LACE_ROW_KEY_OK);
  lace_row_snapshot_build(&snap, g.result, 0, 1, schema);
  LaceValue nv = lace_value_text("z");
  nc_connection_submit(c, nc_job_update("t", &key, &snap, "v", &nv), owner, on_done, &g);
  drain_until(q, &g.done, 5);
  CHECK(g.code == LACE_OK && g.affected == 1);
  /* The snapshot is stale now ('a' became 'z'): a second update is refused */
  nc_connection_submit(c, nc_job_update("t", &key, &snap, "v", &nv), owner, on_done, &g);
  drain_until(q, &g.done, 6);
  CHECK(g.code == LACE_ERR_ROW_NOT_FOUND);
  lace_value_free(&nv); lace_row_key_free(&key); lace_row_snapshot_free(&snap);
  lace_result_free(g.result); g.result = NULL;
  /* History recorded the statements */
  {
    LaceHistory *h = nc_connection_history(c);
    CHECK(lace_history_count(h) >= 1);
    const char *newest = h->num_entries ? h->entries[h->num_entries - 1].sql : NULL;
    CHECK(newest && !strncmp(newest, "UPDATE", 6));
  }
  /* A released owner: its done callback is skipped, destroy_data still runs;
   * a later job of a live owner shows the first one has been handled */
  int before = g.done;
  g_destroyed = 0;
  NcOwner *dead = nc_owner_new();
  nc_connection_submit_full(c, nc_job_read("SELECT 1"), dead, on_done, &g, count_destroy);
  nc_owner_release(dead);
  nc_connection_submit(c, nc_job_read("SELECT 2"), owner, on_done, &g);
  drain_until(q, &g.done, before + 1);
  CHECK(g.done == before + 1 && g_destroyed == 1);
  lace_result_free(g.result); g.result = NULL;
  /* Close: the worker stops after DISCONNECT; later jobs fail at once */
  nc_connection_close(c);
  CHECK(nc_connection_closed(c));
  nc_connection_submit(c, nc_job_read("SELECT 3"), owner, on_done, &g);
  drain_until(q, &g.done, before + 2);
  CHECK(g.code == LACE_ERR_CONNECTION_CLOSED);
  for (int i = 0; i < 100 && !g_closed_ev; i++) {
    struct pollfd p = {mainq_fd(q), POLLIN, 0};
    if (poll(&p, 1, 100) > 0) mainq_drain(q);
  }
  CHECK(g_closed_ev == 1);
  nc_connection_free(c);
  nc_owner_release(owner);
  lace_schema_free(schema);
  mainq_free(q);
  lace_client_destroy(cl);
  str_secure_free(url); unlink(db); free(db);
  /* leave the temp HOME: it holds the daemon log if something failed */
}

/* A burst of queued jobs all complete once, each destroy_data runs once */
static void test_connection_burst(void) {
  char home[64];
  lace_client_t *cl = test_client(home, sizeof home);
  CHECK(cl != NULL);
  if (!cl) return;
  char *db = str_printf("%s/f.sqlite", home);
  char *url = str_printf("sqlite://%s?create=1", db);
  int id = -1;
  CHECK(lace_connect(cl, url, NULL, &id) == LACE_OK);
  MainQueue *q = mainq_new();
  NcConnection *c = nc_connection_new(cl, q, id, url, NULL, NULL, LACE_HISTORY_MODE_OFF, 10);
  g_destroyed = 0;
  Got g = {0};
  for (int i = 0; i < 20; i++)
    nc_connection_submit_full(c, nc_job_read("SELECT 1"), NULL, on_done, &g, count_destroy);
  drain_until(q, &g.done, 20);
  CHECK(g.done == 20);
  nc_connection_close(c);
  for (int i = 0; i < 100 && nc_connection_busy(c); i++) {
    struct pollfd p = {mainq_fd(q), POLLIN, 0};
    if (poll(&p, 1, 100) > 0) mainq_drain(q);
  }
  
  CHECK(g_destroyed == 20);
  lace_result_free(g.result);
  nc_connection_free(c);
  mainq_free(q);
  lace_client_destroy(cl);
  str_secure_free(url); unlink(db); free(db);
}

/* Freed with a statement running and jobs queued: no use after free, every
 * destroy_data runs once (ASan proves the rest) */
static void test_connection_free_running(void) {
  char home[64];
  lace_client_t *cl = test_client(home, sizeof home);
  CHECK(cl != NULL);
  if (!cl) return;
  char *db = str_printf("%s/r.sqlite", home);
  char *url = str_printf("sqlite://%s?create=1", db);
  int id = -1;
  CHECK(lace_connect(cl, url, NULL, &id) == LACE_OK);
  MainQueue *q = mainq_new();
  NcConnection *c = nc_connection_new(cl, q, id, url, NULL, NULL, LACE_HISTORY_MODE_SESSION, 10);
  g_destroyed = 0;
  Got g = {0};
  const char *slow = "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 30000000) SELECT count(*) FROM n";
  for (int i = 0; i < 4; i++) nc_connection_submit_full(c, nc_job_read(slow), NULL, on_done, &g, count_destroy);
  usleep(100000);
  nc_connection_free(c);
  mainq_drain(q);
  CHECK(g_destroyed >= 1 && g_destroyed <= 4);
  for (int i = 0; i < 2; i++) { struct pollfd p = {mainq_fd(q), POLLIN, 0}; if (poll(&p, 1, 0) > 0) mainq_drain(q); }
  lace_result_free(g.result);
  mainq_free(q);
  lace_client_destroy(cl);
  str_secure_free(url); unlink(db); free(db);
}

/* Freed without nc_connection_close (quitting): the database connection is
 * disconnected, also with a statement running, so the daemon does not keep it
 * (and an open transaction's locks) until the orphan reaper */
static bool conn_listed(lace_client_t *cl, int id) {
  LaceConnInfo *info = NULL;
  size_t n = 0;
  if (lace_list_connections(cl, &info, &n) != LACE_OK) return true;
  bool found = false;
  for (size_t i = 0; i < n; i++)
    if (info[i].id == id) found = true;
  lace_conn_info_array_free(info, n);
  return found;
}

static void test_connection_free_disconnects(void) {
  char home[64];
  lace_client_t *cl = test_client(home, sizeof home);
  CHECK(cl != NULL);
  if (!cl) return;
  char *db = str_printf("%s/d.sqlite", home);
  char *url = str_printf("sqlite://%s?create=1", db);
  int id = -1, id2 = -1;
  CHECK(lace_connect(cl, url, NULL, &id) == LACE_OK);
  CHECK(lace_connect(cl, url, NULL, &id2) == LACE_OK);
  MainQueue *q = mainq_new();
  /* Idle */
  NcConnection *c = nc_connection_new(cl, q, id, url, NULL, NULL, LACE_HISTORY_MODE_OFF, 10);
  CHECK(conn_listed(cl, id));
  nc_connection_free(c);
  mainq_drain(q);
  CHECK(!conn_listed(cl, id));
  /* A statement running */
  c = nc_connection_new(cl, q, id2, url, NULL, NULL, LACE_HISTORY_MODE_OFF, 10);
  const char *slow = "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 30000000) SELECT count(*) FROM n";
  nc_connection_submit(c, nc_job_read(slow), NULL, NULL, NULL);
  usleep(100000);
  nc_connection_free(c);
  mainq_drain(q);
  CHECK(!conn_listed(cl, id2));
  mainq_free(q);
  lace_client_destroy(cl);
  str_secure_free(url); unlink(db); free(db);
}

/* Closing a tab cancels a running statement: the connection is closed
 * (NC_CONN_EV_CLOSED, which frees it) long before the statement would end */
static void test_detach_cancels_running(void) {
  char home[64];
  lace_client_t *cl = test_client(home, sizeof home);
  CHECK(cl != NULL);
  if (!cl) return;
  char *db = str_printf("%s/d.sqlite", home);
  char *url = str_printf("sqlite://%s?create=1", db);
  int id = -1;
  CHECK(lace_connect(cl, url, NULL, &id) == LACE_OK);
  NcApp app = {0};
  app.mainq = mainq_new();
  NcConnection *c = nc_connection_new(cl, app.mainq, id, url, NULL, NULL, LACE_HISTORY_MODE_SESSION, 10);
  nc_connection_set_listener(c, app_conn_event, &app);
  Got g = {0};
  const char *slow = "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 400000000) SELECT count(*) FROM n";
  nc_connection_submit(c, nc_job_read(slow), NULL, on_done, &g);
  usleep(200000);
  CHECK(nc_connection_busy(c));
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  app_detach_connection(&app, c);
  CHECK(app.num_closing == 1);
  for (int i = 0; i < 100 && app.num_closing; i++) { /* at most 10 s */
    struct pollfd p = {mainq_fd(app.mainq), POLLIN, 0};
    if (poll(&p, 1, 100) > 0) mainq_drain(app.mainq);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  CHECK(app.num_closing == 0);
  CHECK(t1.tv_sec - t0.tv_sec < 8);
  mainq_drain(app.mainq);
  lace_result_free(g.result);
  free(app.closing);
  mainq_free(app.mainq);
  lace_client_destroy(cl);
  str_secure_free(url); unlink(db); free(db);
}

/* Which open connection a new request may share: a saved id by id, a typed
 * URL by its password-less string */
static void test_conn_key_matches(void) {
  CHECK(conn_key_matches("id1", "sqlite:///a", "id1", "sqlite:///b"));
  CHECK(!conn_key_matches("id1", "sqlite:///a", "id2", "sqlite:///a"));
  CHECK(conn_key_matches(NULL, "postgres://u:pw@h/db", "", "postgres://u@h/db"));
  CHECK(!conn_key_matches("id1", "postgres://u@h/db", NULL, "postgres://u@h/db"));
  CHECK(!conn_key_matches(NULL, "postgres://u@h/db", NULL, "postgres://u@h/other"));
}

/* Holders of a shared connection: the last release closes it */
static void test_conn_registry(void) {
  char home[64];
  lace_client_t *cl = test_client(home, sizeof home);
  CHECK(cl != NULL);
  if (!cl) return;
  char *db = str_printf("%s/r.sqlite", home);
  char *url = str_printf("sqlite://%s?create=1", db);
  int id = -1;
  CHECK(lace_connect(cl, url, NULL, &id) == LACE_OK);
  NcApp app = {0};
  app.mainq = mainq_new();
  NcConnection *c = nc_connection_new(cl, app.mainq, id, url, "sid", "t", LACE_HISTORY_MODE_SESSION, 10);
  nc_connection_set_listener(c, app_conn_event, &app);
  CHECK(app_conn_lookup(&app, "sid", url) == NULL);
  app_conn_hold(&app, c);
  app_conn_hold(&app, c);
  CHECK(app_conn_holders(&app, c) == 2);
  CHECK(app_conn_lookup(&app, "sid", NULL) == c);
  CHECK(app_conn_lookup(&app, "other", url) == NULL);
  app_conn_release(&app, c);
  CHECK(app_conn_holders(&app, c) == 1 && app.num_closing == 0);
  app_conn_release(&app, c);
  CHECK(app_conn_lookup(&app, "sid", url) == NULL && app.num_conns == 0 && app.num_closing == 1);
  for (int i = 0; i < 100 && app.num_closing; i++) {
    struct pollfd p = {mainq_fd(app.mainq), POLLIN, 0};
    if (poll(&p, 1, 100) > 0) mainq_drain(app.mainq);
  }
  CHECK(app.num_closing == 0);
  mainq_drain(app.mainq);
  free(app.closing);
  free(app.conns);
  for (size_t i = 0; i < app.num_log; i++) free(app.log[i].line);
  free(app.log);
  mainq_free(app.mainq);
  lace_client_destroy(cl);
  str_secure_free(url); unlink(db); free(db);
}

/* Esc while a script's COMMIT is in flight does not cancel the COMMIT: the
 * statement running on the connection keeps running until the flag clears */
static void test_escape_spares_commit(void) {
  char home[64];
  lace_client_t *cl = test_client(home, sizeof home);
  CHECK(cl != NULL);
  if (!cl) return;
  char *db = str_printf("%s/e.sqlite", home);
  char *url = str_printf("sqlite://%s?create=1", db);
  int id = -1;
  CHECK(lace_connect(cl, url, NULL, &id) == LACE_OK);
  NcApp app = {0};
  app.mainq = mainq_new();
  app.workspaces = safe_calloc(1, sizeof *app.workspaces);
  app.workspaces[0] = workspace_new("Workspace");
  app.num_workspaces = 1;
  NcConnection *c = nc_connection_new(cl, app.mainq, id, url, NULL, NULL, LACE_HISTORY_MODE_OFF, 10);
  ConnTab *t = workspace_add_tab(app.workspaces[0], c);
  View *v = conntab_add_view(t, view_new_query("COMMIT"));
  Got g = {0};
  const char *slow = "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 30000000) SELECT count(*) FROM n";
  nc_connection_submit(c, nc_job_read(slow), NULL, on_done, &g); /* stands in for the COMMIT */
  usleep(100000);
  v->running = true;
  v->committing = true;
  /* A second workspace's tab sharing the connection: its Esc spares the COMMIT too */
  app.workspaces = safe_realloc(app.workspaces, 2 * sizeof *app.workspaces);
  app.workspaces[1] = workspace_new("W2");
  app.num_workspaces = 2;
  workspace_add_tab(app.workspaces[1], c);
  app.current_ws = 1;
  CHECK(app_cancel_running(&app));
  CHECK(!v->cancel_run);
  usleep(200000);
  mainq_drain(app.mainq);
  CHECK(g.done == 0 && nc_connection_busy(c));
  app.current_ws = 0;
  CHECK(app_cancel_running(&app)); /* Esc is consumed */
  CHECK(!v->cancel_run);
  CHECK(app.status && strstr(app.status, "COMMIT in progress"));
  usleep(300000);
  mainq_drain(app.mainq);
  CHECK(g.done == 0 && nc_connection_busy(c)); /* not cancelled */
  v->committing = false;
  app.current_ws = 1; /* from the other tab: the script on the shared connection stops too */
  CHECK(app_cancel_running(&app));
  CHECK(v->cancel_run);
  drain_until(app.mainq, &g.done, 1);
  CHECK(g.done == 1 && g.code == LACE_ERR_QUERY_CANCELLED);
  v->running = false;
  nc_connection_free(c);
  t->conn = NULL;
  mainq_drain(app.mainq);
  lace_result_free(g.result);
  app.workspaces[1]->tabs[0]->conn = NULL;
  workspace_free(app.workspaces[1]);
  workspace_free(app.workspaces[0]);
  free(app.workspaces);
  free(app.status);
  for (size_t i = 0; i < app.num_log; i++) str_secure_free(app.log[i].line);
  free(app.log);
  mainq_free(app.mainq);
  lace_client_destroy(cl);
  str_secure_free(url); unlink(db); free(db);
}

static LaceResult *rows_result(size_t n, size_t first_id) {
  LaceResult *r = safe_calloc(1, sizeof *r);
  r->num_columns = 2;
  r->columns = safe_calloc(2, sizeof(LaceColumn));
  r->columns[0].name = str_dup("id"); r->columns[0].type = LACE_TYPE_INT;
  r->columns[1].name = str_dup("v");  r->columns[1].type = LACE_TYPE_TEXT;
  r->num_rows = n;
  r->rows = safe_calloc(n ? n : 1, sizeof(LaceRow));
  for (size_t i = 0; i < n; i++) {
    r->rows[i].num_cells = 2;
    r->rows[i].cells = safe_calloc(2, sizeof(LaceValue));
    r->rows[i].cells[0] = lace_value_int((int64_t)(first_id + i));
    char buf[32]; snprintf(buf, sizeof buf, "v%zu", first_id + i);
    r->rows[i].cells[1] = lace_value_text(buf);
  }
  r->rows_affected = -1;
  return r;
}

static size_t g_fetched[64]; static size_t g_nfetched;
static void fetch_rec(PageCache *pc, size_t page, void *d) { (void)pc; (void)d; if (g_nfetched < 64) g_fetched[g_nfetched++] = page; }

static void test_page_cache(void) {
  PageCache *pc = page_cache_new(10);
  page_cache_set_fetch(pc, fetch_rec, NULL);
  g_nfetched = 0;
  CHECK(!page_cache_total_known(pc) && page_cache_extent(pc) == 0);
  page_cache_request(pc, 25);
  CHECK(g_nfetched == 1 && g_fetched[0] == 2 && page_cache_page_pending(pc, 2));
  page_cache_request(pc, 27); /* same page, already pending */
  CHECK(g_nfetched == 1);
  uint64_t gen = page_cache_generation(pc);
  page_cache_page_loaded(pc, gen, 2, rows_result(10, 20));
  CHECK(page_cache_page_loaded_p(pc, 2) && !page_cache_page_pending(pc, 2));
  CHECK(page_cache_row_loaded(pc, 25) && !page_cache_row_loaded(pc, 5));
  CHECK(page_cache_cell(pc, 25, 0)->int_val == 25);
  CHECK(page_cache_cell(pc, 5, 0) == NULL);
  CHECK_SIZE(page_cache_num_columns(pc), 2);
  CHECK_STR(page_cache_column(pc, 1)->name, "v");
  /* A full page: the extent reaches one past it so the UI can scroll on */
  CHECK_SIZE(page_cache_extent(pc), 31);
  /* A short page fixes the total */
  page_cache_request(pc, 35);
  page_cache_page_loaded(pc, gen, 3, rows_result(4, 30));
  CHECK(page_cache_total_known(pc) && page_cache_total(pc) == 34 && page_cache_extent(pc) == 34);
  /* set_cell replaces a value in place */
  page_cache_set_cell(pc, 25, 1, lace_value_text("edited"));
  CHECK_STR(page_cache_cell(pc, 25, 1)->text.data, "edited");
  /* A failed page is requested again next time */
  page_cache_request(pc, 0);
  page_cache_page_failed(pc, gen, 0);
  CHECK(!page_cache_page_pending(pc, 0));
  size_t n = g_nfetched;
  page_cache_request(pc, 1);
  CHECK(g_nfetched == n + 1 && !page_cache_row_loaded(pc, 1));
  /* LRU: 8 slots; loading a ninth drops the least recently used (page 2
   * was read most recently through set_cell/cell) */
  for (size_t p = 4; p < 12; p++) { page_cache_request(pc, p * 10); page_cache_page_loaded(pc, gen, p, rows_result(10, p * 10)); }
  CHECK(page_cache_page_loaded_p(pc, 11));
  CHECK(!page_cache_page_loaded_p(pc, 3)); /* the oldest untouched one went */
  /* Reset: new generation; an old page landing is ignored and freed */
  page_cache_reset(pc);
  CHECK(page_cache_generation(pc) != gen && !page_cache_total_known(pc) && !page_cache_page_loaded_p(pc, 2));
  page_cache_page_loaded(pc, gen, 2, rows_result(10, 20)); /* old gen */
  CHECK(!page_cache_page_loaded_p(pc, 2));
  CHECK(page_cache_num_columns(pc) == 2); /* kept until a new page defines them */
  /* rows_removed drops pages from the row on and lowers the total */
  gen = page_cache_generation(pc);
  page_cache_set_total(pc, 34, true, false);
  page_cache_request(pc, 0); page_cache_page_loaded(pc, gen, 0, rows_result(10, 0));
  page_cache_request(pc, 15); page_cache_page_loaded(pc, gen, 1, rows_result(10, 10));
  page_cache_rows_removed(pc, 12, 1);
  CHECK(page_cache_page_loaded_p(pc, 0) && !page_cache_page_loaded_p(pc, 1) && page_cache_total(pc) == 33);
  CHECK(page_cache_generation(pc) != gen && page_cache_num_columns(pc) == 2);
  /* A fetch in flight across rows_removed lands stale */
  gen = page_cache_generation(pc);
  page_cache_request(pc, 25);
  CHECK(page_cache_page_pending(pc, 2));
  page_cache_rows_removed(pc, 3, 1);
  CHECK(!page_cache_page_pending(pc, 2) && page_cache_generation(pc) != gen);
  page_cache_page_loaded(pc, gen, 2, rows_result(10, 20));
  CHECK(!page_cache_page_loaded_p(pc, 2) && !page_cache_page_pending(pc, 2));
  /* NULL rows is a failure for that generation; stale failure has no effect */
  gen = page_cache_generation(pc);
  page_cache_request(pc, 15);
  page_cache_page_failed(pc, gen - 1, 1);
  CHECK(page_cache_page_pending(pc, 1));
  page_cache_page_loaded(pc, gen, 1, NULL);
  CHECK(!page_cache_page_pending(pc, 1) && !page_cache_page_loaded_p(pc, 1));
  /* Landing a page twice replaces it */
  page_cache_page_loaded(pc, gen, 2, rows_result(10, 20));
  page_cache_page_loaded(pc, gen, 2, rows_result(10, 200));
  CHECK(page_cache_cell(pc, 25, 0)->int_val == 205);
  /* Out of bounds */
  CHECK(page_cache_cell(pc, 25, 2) == NULL);
  page_cache_set_cell(pc, 25, 5, lace_value_text("lost"));
  page_cache_set_cell(pc, 95, 0, lace_value_text("lost"));
  /* request past a known total fetches nothing */
  page_cache_set_total(pc, 34, true, false);
  size_t nf = g_nfetched;
  page_cache_request(pc, 34);
  CHECK(g_nfetched == nf);
  /* A pending page below the removal is stale too: not stored, fetched again */
  page_cache_reset(pc);
  gen = page_cache_generation(pc);
  page_cache_set_total(pc, 34, true, false);
  page_cache_request(pc, 3);
  CHECK(page_cache_page_pending(pc, 0));
  page_cache_rows_removed(pc, 12, 1);
  CHECK(!page_cache_page_pending(pc, 0));
  page_cache_page_loaded(pc, gen, 0, rows_result(10, 0));
  CHECK(!page_cache_page_loaded_p(pc, 0) && !page_cache_page_pending(pc, 0));
  nf = g_nfetched;
  page_cache_request(pc, 3);
  CHECK(g_nfetched == nf + 1);
  /* A result claiming columns but holding none keeps the previous columns */
  gen = page_cache_generation(pc);
  LaceResult *bad = rows_result(2, 0);
  for (size_t i = 0; i < 2; i++) free(bad->columns[i].name);
  free(bad->columns);
  bad->columns = NULL;
  page_cache_page_loaded(pc, gen, 0, bad);
  CHECK(page_cache_num_columns(pc) == 2 && page_cache_column(pc, 0) && strcmp(page_cache_column(pc, 0)->name, "id") == 0);
  /* Real LRU: reading page 0 protects it, so page 1 is evicted, not 0 */
  page_cache_reset(pc);
  gen = page_cache_generation(pc);
  for (size_t p = 0; p < 8; p++) page_cache_page_loaded(pc, gen, p, rows_result(10, p * 10));
  CHECK(page_cache_cell(pc, 3, 0)->int_val == 3);
  page_cache_page_loaded(pc, gen, 8, rows_result(10, 80));
  CHECK(page_cache_page_loaded_p(pc, 0) && !page_cache_page_loaded_p(pc, 1) && page_cache_page_loaded_p(pc, 8));
  page_cache_free(pc);
}

static LaceSchema *schema_id_name(void) {
  LaceSchema *s = safe_calloc(1, sizeof *s);
  s->num_columns = 2; s->columns = safe_calloc(2, sizeof(LaceColumn));
  s->columns[0].name = str_dup("id"); s->columns[0].type = LACE_TYPE_INT; s->columns[0].primary_key = true; s->columns[0].auto_increment = true;
  s->columns[1].name = str_dup("v"); s->columns[1].type = LACE_TYPE_TEXT; s->columns[1].nullable = true;
  return s;
}

static void test_view_sql(void) {
  View *v = view_new_table("t");
  view_set_schema(v, schema_id_name());
  CHECK(view_where(v, LACE_DRIVER_SQLITE) == NULL);
  /* No sorts: the page order is still total (every pk column) */
  CHECK_STR_FREE(view_order_by(v, LACE_DRIVER_SQLITE), "\"id\" ASC");
  view_cycle_sort(v, "v");
  CHECK_STR_FREE(view_order_by(v, LACE_DRIVER_POSTGRES), "\"v\" ASC, \"id\" ASC");
  view_cycle_sort(v, "v");
  CHECK_STR_FREE(view_order_by(v, LACE_DRIVER_MYSQL), "`v` DESC, `id` ASC");
  view_cycle_sort(v, "v");
  CHECK_SIZE(v->num_sorts, 0);
  view_set_filter(v, "v", LACE_FILTER_CONTAINS, "a'b", NULL);
  CHECK_STR_FREE(view_where(v, LACE_DRIVER_SQLITE), "\"v\" LIKE '%a''b%'");
  view_set_filter(v, "v", LACE_FILTER_EQ, "x", NULL); /* replaces the filter on v */
  CHECK_SIZE(v->num_filters, 1);
  char *sql = view_page_sql(v, LACE_DRIVER_SQLITE, 2);
  CHECK(sql && strstr(sql, "WHERE \"v\" = 'x'") && strstr(sql, "ORDER BY \"id\" ASC") && strstr(sql, "LIMIT 1000 OFFSET 2000"));
  free(sql);
  CHECK_STR_FREE(view_count_sql(v, LACE_DRIVER_SQLITE), "SELECT COUNT(*) FROM \"t\" WHERE \"v\" = 'x'");
  view_clear_filters(v);
  CHECK(view_where(v, LACE_DRIVER_SQLITE) == NULL);
  view_free(v);

  View *q = view_new_query("SELECT * FROM t LIMIT 5");
  free(q->base_sql); q->base_sql = str_dup("SELECT * FROM t LIMIT 5"); q->paged = true; q->wrap = true;
  sql = view_page_sql(q, LACE_DRIVER_SQLITE, 1);
  CHECK(sql && strstr(sql, "lace_page") && strstr(sql, "LIMIT 1000 OFFSET 1000"));
  free(sql);
  CHECK_STR(view_title(q), "SELECT * FROM t LIMIT 5");
  view_free(q);
}

static void test_view_state(void) {
  View *v = view_new_table("t");
  view_set_schema(v, schema_id_name());
  uint64_t gen = page_cache_generation(v->cache);
  page_cache_set_total(v->cache, 25, true, false);
  page_cache_page_loaded(v->cache, gen, 0, rows_result(25, 0));
  CHECK(view_columns_changed(v) && !view_columns_changed(v));
  view_measure(v, 0, 25);
  CHECK(v->num_widths == 2 && v->widths[0] >= 2 && v->widths[1] >= 3); /* "id"/"24", "v"/"v24" */
  view_set_width(v, 1, 1);
  CHECK(v->widths[1] == MIN_COL_WIDTH);
  view_set_cursor(v, 99, 99);
  CHECK(v->cursor_row == 24 && v->cursor_col == 1);
  view_move(v, -30, 0);
  CHECK(v->cursor_row == 0);
  v->cursor_row = 20;
  view_scroll_into_view(v, 5, 80, 4);
  CHECK(v->scroll_row == 16);
  v->cursor_row = 2;
  view_scroll_into_view(v, 5, 80, 4);
  CHECK(v->scroll_row == 2);
  /* Horizontal: a narrow area shows one column at a time */
  view_set_width(v, 0, 40); view_set_width(v, 1, 40);
  v->cursor_col = 1;
  view_scroll_into_view(v, 5, 50, 4);
  CHECK(view_first_visible_col(v) == 1);
  view_toggle_select(v, 3); view_toggle_select(v, 1); view_toggle_select(v, 3);
  CHECK(v->num_selected == 1 && view_is_selected(v, 1) && !view_is_selected(v, 3));
  /* A reset changes nothing about the selection until the columns change */
  page_cache_reset(v->cache);
  gen = page_cache_generation(v->cache);
  LaceResult *r = rows_result(5, 0); free(r->columns[1].name); r->columns[1].name = str_dup("w");
  page_cache_page_loaded(v->cache, gen, 0, r);
  CHECK(view_columns_changed(v));
  CHECK(v->num_widths == 2 && v->cursor_col <= 1 && v->num_selected == 0);
  CHECK(view_row_numbers(v, NULL) == true); /* no config: shown */
  view_free(v);
}

static void test_page_cache_soft_reset(void) {
  PageCache *pc = page_cache_new(10);
  page_cache_set_fetch(pc, fetch_rec, NULL);
  uint64_t gen = page_cache_generation(pc);
  page_cache_request(pc, 0);
  page_cache_page_loaded(pc, gen, 0, rows_result(10, 0));
  page_cache_set_total(pc, 50, true, false);
  /* A soft reset keeps the rows (stale) and the total on show, but the page is asked for again */
  g_nfetched = 0;
  page_cache_reset_soft(pc);
  CHECK(page_cache_stale(pc) && page_cache_generation(pc) != gen);
  CHECK(page_cache_row_loaded(pc, 3) && !page_cache_page_loaded_p(pc, 0));
  CHECK(page_cache_total_known(pc) && page_cache_total(pc) == 50);
  page_cache_request(pc, 3);
  CHECK(g_nfetched == 1 && g_fetched[0] == 0);
  /* The old generation's page is ignored */
  page_cache_page_loaded(pc, gen, 0, rows_result(10, 100));
  CHECK(page_cache_cell(pc, 3, 0)->int_val == 3);
  /* Cancelled: the old pages and total are current again, the late page lands nowhere */
  uint64_t soft = page_cache_generation(pc);
  page_cache_unstale(pc);
  CHECK(!page_cache_stale(pc) && page_cache_page_loaded_p(pc, 0) && page_cache_total(pc) == 50);
  CHECK(page_cache_generation(pc) != soft && !page_cache_page_pending(pc, 0));
  page_cache_page_loaded(pc, soft, 0, rows_result(10, 200));
  CHECK(page_cache_cell(pc, 3, 0)->int_val == 3);
  /* The first page of the new state drops the stale rows and the old total */
  page_cache_reset_soft(pc);
  gen = page_cache_generation(pc);
  page_cache_request(pc, 0);
  page_cache_page_loaded(pc, gen, 0, rows_result(3, 500));
  CHECK(!page_cache_stale(pc) && page_cache_cell(pc, 1, 0)->int_val == 501);
  CHECK(page_cache_total_known(pc) && page_cache_total(pc) == 3); /* a short page: the total */
  page_cache_free(pc);
}

static void test_view_checkpoint(void) {
  View *v = view_new_table("t");
  view_set_schema(v, schema_id_name());
  view_cycle_sort(v, "id");
  view_set_filter(v, "v", LACE_FILTER_CONTAINS, "a", NULL);
  v->cursor_row = 7;
  view_checkpoint(v);
  view_cycle_sort(v, "id"); /* desc */
  view_cycle_sort(v, "v");
  view_set_filter(v, "id", LACE_FILTER_EQ, "3", NULL);
  view_add_raw_filter(v, "id > 1");
  v->cursor_row = 0;
  CHECK(v->num_sorts == 2 && v->num_filters == 3);
  CHECK(view_revert(v));
  CHECK(v->num_sorts == 1 && !v->sorts[0].desc && v->num_filters == 1 && v->cursor_row == 7);
  CHECK_STR(v->filters[0].value, "a");
  CHECK(!view_revert(v)); /* used up */
  view_checkpoint(v);
  view_checkpoint_drop(v);
  CHECK(!v->has_checkpoint && !view_revert(v));
  char *w = view_where(v, LACE_DRIVER_SQLITE);
  CHECK(w != NULL);
  free(w);
  view_add_raw_filter(v, "id > 1");
  CHECK(v->num_filters == 2 && v->filters[1].op == LACE_FILTER_RAW);
  view_free(v);
}

static void test_reload_twice_then_cancel(void) {
  /* o, o before the first reload lands, then both cancelled: back to the first state */
  View *v = view_new_table("t");
  view_set_schema(v, schema_id_name());
  PageCache *pc = v->cache;
  page_cache_page_loaded(pc, page_cache_generation(pc), 0, rows_result(10, 0));
  CHECK(view_checkpoint(v)); /* S0: unsorted */
  view_cycle_sort(v, "id");
  page_cache_reset_soft(pc);
  CHECK(!view_checkpoint(v)); /* S1 must not replace S0 */
  view_cycle_sort(v, "id"); /* desc */
  page_cache_reset_soft(pc);
  CHECK(view_revert(v) && v->num_sorts == 0);
  page_cache_unstale(pc);
  CHECK(page_cache_cell(pc, 2, 0)->int_val == 2 && page_cache_page_loaded_p(pc, 0));
  /* the fail throttle: one decision for every page until position or generation change */
  v->fail_set = true;
  v->fail_gen = page_cache_generation(pc);
  v->fail_cursor = v->cursor_row;
  v->fail_scroll = v->scroll_row;
  CHECK(view_fail_blocks(v));
  v->cursor_row = 1;
  CHECK(!view_fail_blocks(v));
  v->cursor_row = 0;
  page_cache_reset_soft(pc);
  CHECK(!view_fail_blocks(v));
  view_free(v);
}

static void test_header_table_state(void) {
  View *v = view_new_table("t");
  view_set_schema(v, schema_id_name());
  CHECK(header_position(v) == NULL);
  PageCache *pc = v->cache;
  uint64_t gen = page_cache_generation(pc);
  page_cache_page_loaded(pc, gen, 0, rows_result(TUI_PAGE_SIZE, 0));
  CHECK_STR_FREE(header_position(v), "1 of 1,001+"); /* a full page: more may follow */
  page_cache_set_total(pc, 1234567, true, false);
  v->cursor_row = 1233;
  CHECK_STR_FREE(header_position(v), "1,234 of 1,234,567");
  page_cache_set_total(pc, 200000, true, true);
  v->cursor_row = 0;
  CHECK_STR_FREE(header_position(v), "1 of \xe2\x89\x88" "200,000");
  CHECK_STR_FREE(header_sorts_filters(v), "");
  view_cycle_sort(v, "id");
  view_cycle_sort(v, "v");
  view_cycle_sort(v, "v"); /* desc */
  view_set_filter(v, "id", LACE_FILTER_EQ, "42", NULL);
  view_set_filter(v, "v", LACE_FILTER_CONTAINS, "x", NULL);
  CHECK_STR_FREE(header_sorts_filters(v), "\xe2\x86\x93v \xe2\x86\x91id  id = 42  v contains 'x'");
  view_free(v);
}

static void test_view_more(void) {
  /* sorts: unknown name skipped; no-schema fallback */
  View *v = view_new_table("t");
  view_set_schema(v, schema_id_name());
  view_cycle_sort(v, "nope");
  CHECK_STR_FREE(view_order_by(v, LACE_DRIVER_SQLITE), "\"id\" ASC");
  view_free(v);
  v = view_new_table("t");
  CHECK(view_order_by(v, LACE_DRIVER_SQLITE) == NULL);
  view_cycle_sort(v, "v"); view_cycle_sort(v, "v");
  CHECK_STR_FREE(view_order_by(v, LACE_DRIVER_SQLITE), "\"v\" DESC");
  view_set_filter(v, NULL, LACE_FILTER_EQ, "x", NULL);
  CHECK_SIZE(v->num_filters, 0);
  view_set_filter(v, "a", LACE_FILTER_EQ, "1", NULL);
  view_set_filter(v, "b", LACE_FILTER_EQ, "2", NULL);
  view_set_filter(v, "c", LACE_FILTER_EQ, "3", NULL);
  view_remove_filter(v, 1);
  CHECK(v->num_filters == 2 && !strcmp(v->filters[0].column, "a") && !strcmp(v->filters[1].column, "c"));
  view_free(v);

  /* widths: cap, never shrink, user override survives measure */
  v = view_new_table("t");
  uint64_t gen = page_cache_generation(v->cache);
  LaceResult *r = rows_result(2, 0);
  free(r->rows[0].cells[1].text.data);
  r->rows[0].cells[1] = lace_value_text("0123456789012345678901234567890123456789012345678901234567890");
  page_cache_set_total(v->cache, 2, true, false);
  page_cache_page_loaded(v->cache, gen, 0, r);
  CHECK(view_columns_changed(v));
  view_measure(v, 0, 2);
  CHECK(v->widths[1] == DEFAULT_COL_WIDTH);
  page_cache_reset(v->cache);
  gen = page_cache_generation(v->cache);
  page_cache_page_loaded(v->cache, gen, 0, rows_result(2, 0));
  CHECK(!view_columns_changed(v));
  view_measure(v, 0, 2);
  CHECK(v->widths[1] == DEFAULT_COL_WIDTH); /* never shrinks */
  view_set_width(v, 1, 4);
  view_measure(v, 0, 2);
  CHECK(v->widths[1] == 4); /* user width kept */
  /* scroll_col back left, rows_visible 0, last visible col */
  view_set_width(v, 0, 40); view_set_width(v, 1, 40);
  v->cursor_col = 1; v->cursor_row = 0;
  view_scroll_into_view(v, 0, 50, 4);
  CHECK(v->scroll_col == 1);
  v->cursor_row = 1; v->scroll_row = 0;
  view_scroll_into_view(v, 0, 50, 4); /* forced to one visible row */
  CHECK(v->scroll_row == 1);
  v->cursor_row = 0;
  view_scroll_into_view(v, 0, 50, 4);
  CHECK(v->scroll_row == 0);
  CHECK(view_last_visible_col(v, 50, 4) == 1);
  v->cursor_col = 0;
  view_scroll_into_view(v, 3, 50, 4);
  CHECK(v->scroll_col == 0);
  CHECK(view_last_visible_col(v, 50, 4) == 0);
  CHECK(view_last_visible_col(v, 200, 4) == 1);
  /* after a column change the width is measured again */
  page_cache_reset(v->cache);
  gen = page_cache_generation(v->cache);
  r = rows_result(2, 0); free(r->columns[1].name); r->columns[1].name = str_dup("w");
  free(r->rows[0].cells[1].text.data);
  r->rows[0].cells[1] = lace_value_text("0123456789");
  page_cache_page_loaded(v->cache, gen, 0, r);
  CHECK(view_columns_changed(v));
  view_measure(v, 0, 2);
  CHECK(v->widths[1] == 10); /* fresh measure, not the user 40 or a stuck 4 */
  view_free(v);

  /* titles */
  v = view_new_query("");
  CHECK_STR(view_title(v), "New query");
  free(v->text); v->text = str_dup("\n  \n  SELECT 1  \nFROM t");
  CHECK_STR(view_title(v), "SELECT 1");
  free(v->text); v->text = str_dup("SELECT aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(strlen(view_title(v)) == 40);
  free(v->text);
  v->text = str_dup("\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5");
  const char *t = view_title(v);
  CHECK(utf8_width(t, strlen(t)) <= 40 && strlen(t) == 60); /* 20 wide chars */
  view_free(v);

  /* zero-width combining marks add bytes, not columns: the title buffer holds */
  char *marks = safe_calloc(2 + 300 * 2, 1);
  strcpy(marks, "a");
  for (int i = 0; i < 300; i++) strcat(marks, "\xcc\x81");
  v = view_new_query(marks);
  free(marks);
  t = view_title(v);
  CHECK(strlen(t) < 256 && strlen(t) > 0);
  view_free(v);
}

/* The preview view: found, replaced in place, pinned by its flag */
static void test_sidebar_marquee(void) {
  CHECK(sidebar_marquee_offset(0, 12345) == 0 && sidebar_marquee_offset(-3, 99) == 0);
  /* overflow 5: 1500 ms rest, 5 steps of 200 ms, 1500 ms rest, then over again */
  CHECK(sidebar_marquee_offset(5, 0) == 0 && sidebar_marquee_offset(5, 1499) == 0);
  CHECK(sidebar_marquee_offset(5, 1500) == 1 && sidebar_marquee_offset(5, 1699) == 1);
  CHECK(sidebar_marquee_offset(5, 1700) == 2 && sidebar_marquee_offset(5, 2300) == 5);
  CHECK(sidebar_marquee_offset(5, 2500) == 5 && sidebar_marquee_offset(5, 3999) == 5);
  CHECK(sidebar_marquee_offset(5, 4000) == 0); /* the period is 4000 ms */
  int prev = 0;
  for (int64_t ms = 0; ms < 4000; ms += 50) { /* monotonic within a period, never past the overflow */
    int o = sidebar_marquee_offset(5, ms);
    CHECK(o >= prev && o <= 5);
    prev = o;
  }
}

static void test_conntab_preview(void) {
  NcWorkspace *w = workspace_new("W");
  ConnTab *t = workspace_add_tab(w, NULL);
  CHECK(conntab_find_preview(t) == SIZE_MAX);
  conntab_add_view(t, view_new_table("a"));
  View *p = view_new_table("b");
  p->preview = true;
  conntab_add_view(t, p);
  conntab_add_view(t, view_new_query("SELECT 1"));
  t->current = 0;
  CHECK(conntab_find_preview(t) == 1);
  View *c = view_new_table("c");
  c->preview = true;
  CHECK(conntab_replace_view(t, 1, c) == c);
  CHECK(t->num_views == 3 && t->views[1] == c && t->current == 1);
  CHECK(conntab_find_preview(t) == 1 && conntab_find_table_view(t, "b") == SIZE_MAX);
  c->preview = false; /* pinned */
  CHECK(conntab_find_preview(t) == SIZE_MAX);
  View *d = view_new_table("d");
  CHECK(conntab_replace_view(t, 9, d) == d && t->num_views == 4 && t->current == 3); /* out of range: appended */
  workspace_free(w);
}

static void test_subviews_model(void) {
  View *tv = view_new_table("orders");
  CHECK(tv->subview == SV_DATA && tv->object == LACE_OBJ_TABLE);
  for (int i = 0; i < SV_COUNT; i++) CHECK(view_has_subview(tv, (SubView)i));
  /* a foreign key's first column is marked and followed */
  LaceSchema *s = schema_id_name();
  s->num_foreign_keys = 1;
  s->foreign_keys = safe_calloc(1, sizeof(LaceForeignKey));
  s->foreign_keys[0].num_columns = s->foreign_keys[0].num_ref_columns = 1;
  s->foreign_keys[0].columns = safe_calloc(1, sizeof(char *));
  s->foreign_keys[0].columns[0] = str_dup("v");
  s->foreign_keys[0].ref_columns = safe_calloc(1, sizeof(char *));
  s->foreign_keys[0].ref_columns[0] = str_dup("id");
  s->foreign_keys[0].ref_table = str_dup("t");
  view_set_schema(tv, s);
  CHECK(view_fk_first_column(tv, "v") && !view_fk_first_column(tv, "id") && !view_fk_first_column(tv, NULL));
  /* the session's names */
  CHECK_STR(view_subview_key(tv, SV_FOREIGN_KEYS), "foreign_keys");
  SubView sv;
  CHECK(view_subview_from_key("indexes", &sv) && sv == SV_INDEXES);
  CHECK(view_subview_from_key("definition", &sv) && sv == SV_STRUCTURE);
  CHECK(!view_subview_from_key("rows", &sv));
  LaceObjectKind k;
  CHECK(view_object_from_key("trigger", &k) && k == LACE_OBJ_TRIGGER && !view_object_from_key("x", &k));
  tv->subview = SV_INDEXES;
  cJSON *j = view_to_json(tv, false);
  CHECK_STR(json_get_string(j, "subview", ""), "indexes");
  CHECK(!cJSON_GetObjectItem(j, "object"));
  cJSON_Delete(j);
  view_free(tv);
  /* a database view has Data, Definition and Information; a trigger starts on Definition */
  View *vv = view_new_object(LACE_OBJ_VIEW, "big", NULL, NULL);
  CHECK(vv->subview == SV_DATA && view_has_subview(vv, SV_STRUCTURE) && !view_has_subview(vv, SV_INDEXES));
  CHECK_STR(view_subview_key(vv, SV_STRUCTURE), "definition");
  view_free(vv);
  View *tr = view_new_object(LACE_OBJ_TRIGGER, "trg", "orders", NULL);
  CHECK(tr->subview == SV_STRUCTURE && !view_has_subview(tr, SV_DATA) && view_has_subview(tr, SV_INFORMATION));
  j = view_to_json(tr, false);
  CHECK_STR(json_get_string(j, "object", ""), "trigger");
  CHECK_STR(json_get_string(j, "object_table", ""), "orders");
  CHECK_STR(json_get_string(j, "subview", ""), "definition");
  cJSON_Delete(j);
  view_free(tr);
  /* tables and views share a namespace in a tab, routines and triggers do not */
  NcWorkspace *w = workspace_new("W");
  ConnTab *t = workspace_add_tab(w, NULL);
  conntab_add_view(t, view_new_object(LACE_OBJ_VIEW, "big", NULL, NULL));
  conntab_add_view(t, view_new_object(LACE_OBJ_TRIGGER, "big", "orders", NULL));
  CHECK(conntab_find_table_view(t, "big") == 0);
  CHECK(conntab_find_object_view(t, LACE_OBJ_TRIGGER, "big") == 1);
  CHECK(conntab_find_object_view(t, LACE_OBJ_ROUTINE, "big") == SIZE_MAX);
  workspace_free(w);
}

static void test_workspace_json(void) {
  NcWorkspace *w = workspace_new("W2");
  ConnTab *t = workspace_add_tab(w, NULL);
  t->sidebar = false;
  View *tv = view_new_table("orders");
  view_set_schema(tv, schema_id_name());
  view_cycle_sort(tv, "id"); view_cycle_sort(tv, "id"); /* desc */
  view_set_filter(tv, "v", LACE_FILTER_BETWEEN, "1", "9");
  tv->cursor_row = 120; tv->cursor_col = 1; tv->scroll_row = 100;
  tv->filters_visible = true;
  conntab_add_view(t, tv);
  View *qv = view_new_query("SELECT 1");
  qv->text_cursor = 3;
  conntab_add_view(t, qv);
  tv->filters = safe_realloc(tv->filters, (tv->num_filters + 1) * sizeof(LaceFilter));
  tv->filters[tv->num_filters] = (LaceFilter){.op = LACE_FILTER_RAW, .value = str_dup("a > 1")};
  tv->num_filters++;
  t->current = 0;
  cJSON *j = workspace_to_json(w, true);
  CHECK(j && workspace_json_shape(j) == WORKSPACE_JSON_NESTED);
  CHECK_STR(json_get_string(j, "name", ""), "W2");
  cJSON *c = json_get_array_item(json_get_array(j, "connections"), 0);
  CHECK(c && !json_get_bool(c, "sidebar", true) && json_get_int(c, "current", -1) == 0);
  CHECK(!cJSON_GetObjectItem(c, "saved_id") && !cJSON_GetObjectItem(c, "connstr"));
  cJSON *tab = json_get_array_item(json_get_array(c, "tabs"), 0);
  CHECK_STR(json_get_string(tab, "table", ""), "orders");
  CHECK(json_get_bool(json_get_array_item(json_get_array(tab, "sorts"), 0), "desc", false));
  cJSON *f = json_get_array_item(json_get_array(tab, "filters"), 0);
  CHECK_STR(json_get_string(f, "op", ""), "between");
  CHECK_STR(json_get_string(f, "value2", ""), "9");
  cJSON *rf = json_get_array_item(json_get_array(tab, "filters"), 1);
  CHECK(rf && !cJSON_GetObjectItem(rf, "column"));
  CHECK_STR(json_get_string(rf, "op", ""), "raw");
  CHECK_STR(json_get_string(rf, "value", ""), "a > 1");
  CHECK(json_get_bool(tab, "filters_visible", false));
  cJSON *cur = json_get_array(tab, "cursor");
  CHECK(cur && json_get_array_item(cur, 0)->valuedouble == 120);
  cJSON *q = json_get_array_item(json_get_array(c, "tabs"), 1);
  CHECK_STR(json_get_string(q, "type", ""), "query");
  CHECK(json_get_int(q, "cursor", -1) == 3);
  cJSON_Delete(j);
  j = workspace_to_json(w, false);
  CHECK(cJSON_GetObjectItem(json_get_array_item(json_get_array(json_get_array_item(json_get_array(j, "connections"), 0), "tabs"), 0), "cursor") == NULL);
  cJSON_Delete(j);
  CHECK(conntab_find_table_view(t, "orders") == 0 && conntab_find_table_view(t, "x") == SIZE_MAX);
  /* A connection form: its place and nothing else */
  ConnTab *ft = workspace_add_form(w, SIZE_MAX, false);
  CHECK(ft && ft->form && !ft->conn && w->current == 0 && w->num_tabs == 2);
  CHECK_STR(form_tab_label(ft->form), "New connection");
  j = workspace_to_json(w, false);
  cJSON *fj = json_get_array_item(json_get_array(j, "connections"), 1);
  CHECK(json_get_bool(fj, "form", false) && json_get_int(fj, "order", -1) == 2);
  CHECK(cJSON_GetArraySize(fj) == 2); /* no saved_id, connstr, tabs, name */
  cJSON_Delete(j);
  /* a form turned connected and back */
  conntab_make_connected(ft, NULL);
  CHECK(!ft->form && ft->sidebar);
  conntab_add_view(ft, view_new_table("x"));
  conntab_make_form(ft);
  CHECK(ft->form && ft->num_views == 0);
  workspace_remove_tab(w, 1);
  CHECK(workspace_add_form(w, 0, true) == w->tabs[0] && w->current == 0 && w->tabs[1] == t);
  workspace_remove_tab(w, 0);
  workspace_add_tab(w, NULL);
  workspace_remove_tab(w, 1); /* releases its owner (ASan checks) */
  CHECK(w->num_tabs == 1);
  workspace_free(w);
}

static void test_session_file(void) {
  char home[64];
  snprintf(home, sizeof home, "/tmp/lace-nc-s-XXXXXX");
  if (!mkdtemp(home)) {
    CHECK(!"mkdtemp");
    return;
  }
  /* platform_get_config_dir() caches its first answer: this test runs first */
  setenv("HOME", home, 1);
  unsetenv("XDG_CONFIG_HOME");
  char *path = nc_session_path();
  /* Never go on outside the temporary HOME: it would write the user's file */
  bool private_path = path && !strncmp(path, home, strlen(home)) && path[strlen(home)] == '/' &&
                      strstr(path, "session.json");
  CHECK(private_path);
  if (!private_path) {
    free(path);
    return;
  }
  char *err = NULL;
  CHECK(nc_session_load(&err) == NULL && !err); /* absent */
  const char *v1 = "{\"settings\":{},\"workspaces\":[{\"name\":\"A\",\"current_tab\":0,\"tabs\":[{\"type\":\"TABLE\",\"connection_id\":\"X\",\"table_name\":\"t\",\"ui\":{}}]}],\"current_workspace\":0}";
  char *d1 = str_printf("%s/.config", home);
  char *dir = str_printf("%s/.config/lace", home);
  platform_mkdir(d1); platform_mkdir(dir);
  free(d1);
  CHECK(platform_write_file_private(path, v1, strlen(v1)));
  cJSON *root = nc_session_load(&err);
  CHECK(root && !err && json_get_int(root, "version", 0) == 2);
  CHECK(workspace_json_shape(json_get_array_item(json_get_array(root, "workspaces"), 0)) == WORKSPACE_JSON_NESTED);
  cJSON_Delete(root);
  const char *v2 = "{\"version\":2,\"workspaces\":[],\"current\":0,\"ncurses\":{\"sidebar_visible\":false},\"gtk\":{\"x\":1}}";
  CHECK(platform_write_file_private(path, v2, strlen(v2)));
  NcApp app = {0};
  app.workspaces = safe_calloc(1, sizeof(NcWorkspace *));
  app.workspaces[0] = workspace_new("Only"); app.num_workspaces = 1;
  CHECK(nc_session_save(&app, &err));
  root = nc_session_load(&err);
  CHECK(root && cJSON_GetObjectItem(root, "gtk") && cJSON_GetObjectItem(root, "ncurses"));
  CHECK_STR(json_get_string(json_get_array_item(json_get_array(root, "workspaces"), 0), "name", ""), "Only");
  CHECK(cJSON_IsObject(cJSON_GetObjectItem(root, "notcurses")));
  cJSON_Delete(root);
  /* Saving over a v1 file keeps the ncurses settings as "ncurses" */
  const char *v1s = "{\"settings\":{\"sidebar_visible\":false},\"workspaces\":[{\"name\":\"A\",\"current_tab\":0,\"tabs\":[{\"type\":\"TABLE\",\"connection_id\":\"X\",\"table_name\":\"t\",\"ui\":{}}]}],\"current_workspace\":0}";
  CHECK(platform_write_file_private(path, v1s, strlen(v1s)));
  CHECK(nc_session_save(&app, &err));
  { char *txt = platform_read_file(path, 100000);
    cJSON *r2 = cJSON_Parse(txt);
    free(txt);
    CHECK(r2 && !cJSON_GetObjectItem(r2, "settings") && !cJSON_GetObjectItem(r2, "current_workspace"));
    CHECK(!json_get_bool(cJSON_GetObjectItem(r2, "ncurses"), "sidebar_visible", true));
    cJSON_Delete(r2); }
  /* A non-object file is not overwritten either */
  CHECK(platform_write_file_private(path, "[1]", 3));
  CHECK(!nc_session_save(&app, &err) && err && strstr(err, "object"));
  free(err); err = NULL;
  /* A broken file: error with the byte offset, nothing written */
  CHECK(platform_write_file_private(path, "{\"version\": 2, ", 15));
  root = nc_session_load(&err);
  CHECK(!root && err && strstr(err, "byte"));
  free(err); err = NULL;
  CHECK(!nc_session_save(&app, &err) && err && strstr(err, "byte"));
  free(err);
  char *left = platform_read_file(path, 1000);
  CHECK(left && !strcmp(left, "{\"version\": 2, "));
  free(left);
  workspace_free(app.workspaces[0]); free(app.workspaces);
  { char *cmd = str_printf("rm -rf %s", home); if (system(cmd)) {} free(cmd); }
  free(path); free(dir);
}

static UiEvent key_ev(int key, bool special, UiKeyMod mods) {
  UiEvent e = {0};
  e.type = UI_EVENT_KEY;
  e.key.key = key;
  e.key.is_special = special;
  e.key.mods = mods;
  return e;
}

static void test_palette_score(void) {
  CHECK(palette_score("", "anything") >= 0);
  CHECK(palette_score("ord", "orders") > palette_score("ord", "customer_orders"));
  CHECK(palette_score("ord", "customer_orders") > palette_score("ord", "o-r-d"));
  CHECK(palette_score("xyz", "orders") < 0);
  CHECK(palette_score("OPEN", "Open table orders") >= 300); /* case-insensitive prefix */
  CHECK(palette_score("tab ord", "Open table orders") > 0); /* every word must match */
  CHECK(palette_score("tab zzz", "Open table orders") < 0);
  /* Rows run real code: editor and dialog keys, the ncurses tab keys and
   * missing connection tabs are not listed */
  NcApp app = {0};
  app.workspaces = safe_calloc(1, sizeof *app.workspaces);
  app.workspaces[0] = workspace_new("Workspace");
  app.num_workspaces = 1;
  CHECK(palette_lists_action(&app, HOTKEY_QUIT));
  CHECK(palette_lists_action(&app, HOTKEY_CONFIG));
  CHECK(!palette_lists_action(&app, HOTKEY_CUT_LINE));
  CHECK(!palette_lists_action(&app, HOTKEY_PASTE));
  CHECK(!palette_lists_action(&app, HOTKEY_EDITOR_SAVE));
  CHECK(!palette_lists_action(&app, HOTKEY_NEW_TAB));
  CHECK(!palette_lists_action(&app, HOTKEY_CLOSE_TAB));
  CHECK(!palette_lists_action(&app, HOTKEY_CONN_TAB_1));
  CHECK(!palette_lists_action(&app, HOTKEY_TOGGLE_HEADER)); /* ncurses only */
  workspace_free(app.workspaces[0]);
  free(app.workspaces);
}

static void test_text_input_key(void) {
  TextInput in;
  text_input_init(&in, NULL, false);
  UiEvent e = key_ev('a', false, UI_MOD_NONE);
  CHECK(nc_text_input_key(&in, &e) && in.len == 1);
  e = key_ev(0xC3, false, UI_MOD_NONE);
  memcpy(e.key.text, "\xc3\xa9", 3);
  CHECK(nc_text_input_key(&in, &e));
  CHECK(in.len == 3 && in.cursor == 3 && memcmp(in.buf, "a\xc3\xa9", 3) == 0);
  text_input_free(&in);
}

static void test_list_widget(void) {
  ListWidget l;
  list_init(&l, true);
  ListRow *rows = safe_calloc(4, sizeof(ListRow));
  rows[0] = (ListRow){str_dup("TABLES"), NULL, NULL, 0, NULL, true};
  rows[1] = (ListRow){str_dup("orders"), NULL, NULL, 1, NULL, false};
  rows[2] = (ListRow){str_dup("customers"), NULL, NULL, 1, NULL, false};
  rows[3] = (ListRow){str_dup("order_items"), NULL, NULL, 1, NULL, false};
  list_set_rows(&l, rows, 4);
  CHECK(l.num_shown == 4 && l.cursor == 1); /* a header is never selected */
  UiEvent e = key_ev(UI_KEY_DOWN, true, UI_MOD_NONE);
  CHECK(list_key(&l, &e) == LIST_CHANGED && l.cursor == 2);
  e = key_ev('o', false, UI_MOD_NONE); list_key(&l, &e);
  e = key_ev('r', false, UI_MOD_NONE); list_key(&l, &e);
  e = key_ev('d', false, UI_MOD_NONE); list_key(&l, &e);
  CHECK(l.num_shown == 3); /* header, orders, order_items */
  CHECK_STR(list_selected(&l)->text, "orders");
  e = key_ev(UI_KEY_ENTER, true, UI_MOD_NONE);
  CHECK(list_key(&l, &e) == LIST_ACTIVATE);
  e = key_ev(UI_KEY_ESCAPE, true, UI_MOD_NONE);
  CHECK(list_key(&l, &e) == LIST_CANCEL);
  /* the cursor skips the header at both ends */
  e = key_ev(UI_KEY_UP, true, UI_MOD_NONE);
  CHECK(list_key(&l, &e) == LIST_NONE && l.cursor == 1);
  e = key_ev(UI_KEY_TAB, true, UI_MOD_NONE); /* End moves the list only off the input */
  CHECK(list_key(&l, &e) == LIST_NONE && !l.input_focused);
  e = key_ev(UI_KEY_END, true, UI_MOD_NONE);
  CHECK(list_key(&l, &e) == LIST_CHANGED && l.cursor == 2);
  /* detail column width: widest detail, capped at a third */
  CHECK(list_detail_width(&l, 90) == 0);
  l.rows[1].detail = str_dup("folder/sub");
  CHECK(list_detail_width(&l, 90) == 10);
  CHECK(list_detail_width(&l, 21) == 7);
  l.detail_cols = 5;
  CHECK(list_detail_width(&l, 90) == 5);
  list_free(&l);

  /* palette scoring orders sections and rows; no match hides the header too */
  list_init(&l, true);
  l.score = NULL;
  rows = safe_calloc(5, sizeof(ListRow));
  rows[0] = (ListRow){str_dup("A"), NULL, NULL, 0, NULL, true};
  rows[1] = (ListRow){str_dup("alpha"), NULL, NULL, 1, NULL, false};
  rows[2] = (ListRow){str_dup("B"), NULL, NULL, 0, NULL, true};
  rows[3] = (ListRow){str_dup("beta"), NULL, NULL, 1, NULL, false};
  rows[4] = (ListRow){str_dup("beta2"), NULL, NULL, 1, NULL, false};
  list_set_rows(&l, rows, 5);
  e = key_ev('t', false, UI_MOD_NONE); list_key(&l, &e);
  CHECK(l.num_shown == 3 && l.rows[l.shown[0]].header);
  e = key_ev('x', false, UI_MOD_NONE); list_key(&l, &e);
  CHECK(l.num_shown == 0 && list_selected(&l) == NULL);
  list_free(&l);
}


static void test_header_format(void) {
  const char *two[] = {"a.db", "b.db"};
  size_t off = 99, len = 99, dboff = 99, dblen = 99;
  char *h = header_format("Workspace", two, 2, 0, NULL, "big", 0, 3, 80, &off, &len, &dboff, &dblen);
  CHECK_STR(h, "Workspace \xe2\x96\xb8 a.db \xe2\x96\xb8 big (1 of 3)"); /* the other tab is not listed */
  CHECK(off == strlen("Workspace \xe2\x96\xb8 ") && len == 4 && !strncmp(h + off, "a.db", len));
  CHECK(dboff == 0 && dblen == 0);
  free(h);
  CHECK_STR_FREE(header_format("Workspace", two, 2, 1, NULL, "big", 0, 0, 80, NULL, NULL, NULL, NULL),
                 "Workspace \xe2\x96\xb8 b.db \xe2\x96\xb8 big"); /* no count with the tabs strip */
  /* a server connection's database sits between the connection and the view */
  h = header_format("W", two, 2, 0, "shop", "big", 0, 0, 80, &off, &len, &dboff, &dblen);
  CHECK_STR(h, "W \xe2\x96\xb8 a.db \xe2\x96\xb8 shop \xe2\x96\xb8 big");
  CHECK(dblen == 4 && !strncmp(h + dboff, "shop", dblen));
  free(h);
  CHECK_STR_FREE(header_format("W", two, 2, 0, "shop", NULL, 0, 0, 80, NULL, NULL, NULL, NULL),
                 "W \xe2\x96\xb8 a.db \xe2\x96\xb8 shop");
  CHECK_STR_FREE(header_format("W", two, 2, 0, "", "big", 0, 0, 80, NULL, NULL, NULL, NULL),
                 "W \xe2\x96\xb8 a.db \xe2\x96\xb8 big"); /* empty: none */
  /* a repeated name gets its position among the equal ones */
  const char *same[] = {"t.sqlite", "t.sqlite", "x"};
  CHECK_STR_FREE(header_format("W", same, 3, 1, NULL, NULL, 0, 0, 80, NULL, NULL, NULL, NULL), "W \xe2\x96\xb8 t.sqlite 2");
  CHECK_STR_FREE(header_format("W", same, 3, 0, NULL, NULL, 0, 0, 80, NULL, NULL, NULL, NULL), "W \xe2\x96\xb8 t.sqlite");
  /* too narrow: the workspace name goes */
  const char *three[] = {"alpha", "bravo", "charlie"};
  CHECK_STR_FREE(header_format("Workspace", three, 3, 1, NULL, "big", 0, 0, 16, NULL, NULL, NULL, NULL),
                 "bravo \xe2\x96\xb8 big");
  /* then the line is clipped on a column boundary, and the current name with it */
  h = header_format("WS", three, 3, 0, NULL, NULL, 0, 0, 3, &off, &len, NULL, NULL);
  CHECK_STR(h, "alp");
  CHECK(off == 0 && len == 3);
  free(h);
  h = header_format("WS", three, 3, 2, NULL, NULL, 0, 0, 3, &off, &len, NULL, NULL);
  CHECK_STR(h, "cha"); /* the current tab leads once the workspace name is gone */
  CHECK(off == 0 && len == 3);
  free(h);
  h = header_format("WS", three, 3, 2, "db", NULL, 0, 0, 9, &off, &len, &dboff, &dblen);
  CHECK_STR(h, "charlie \xe2\x96\xb8"); /* the database clipped away: no range */
  CHECK(dblen == 0);
  free(h);
  h = header_format("WS", three, 3, 2, NULL, NULL, 0, 0, 0, &off, &len, NULL, NULL);
  CHECK_STR(h, "");
  CHECK(len == 0);
  free(h);
  /* no tabs: just the workspace */
  CHECK_STR_FREE(header_format("Workspace", NULL, 0, 0, NULL, NULL, 0, 0, 40, NULL, NULL, NULL, NULL), "Workspace \xe2\x96\xb8 ");
  /* wide characters count by display width */
  const char *wide[] = {"\xe6\x97\xa5\xe6\x9c\xac"};
  h = header_format(NULL, wide, 1, 0, NULL, NULL, 0, 0, 3, &off, &len, NULL, NULL);
  CHECK_STR(h, "\xe6\x97\xa5");
  free(h);
}

static bool has_error(char *e) {
  bool yes = e != NULL;
  free(e);
  return yes;
}

static void test_connect_helpers(void) {
  char *err = NULL;
  CHECK_STR_FREE(connect_check_port("", "Port"), NULL);
  CHECK_STR_FREE(connect_check_port("5432", "Port"), NULL);
  CHECK_STR_FREE(connect_check_port("65535", "Port"), NULL);
  CHECK(has_error(connect_check_port("0", "Port")));
  CHECK(has_error(connect_check_port("65536", "Port")));
  CHECK(has_error(connect_check_port("12ab", "Port")));
  CHECK(has_error(connect_check_port("-1", "Port")));
  CHECK_STR_FREE(connect_check_host("db.example.com", "Host"), NULL);
  CHECK(has_error(connect_check_host("a b", "Host")));
  CHECK(has_error(connect_check_host("u@h", "Host")));
  CHECK(has_error(connect_check_host("h/x", "Host")));
  CHECK(has_error(connect_check_host("h?x", "Host")));
  CHECK(has_error(connect_check_host("h#x", "Host")));
  /* targets: URLs as they are, a missing file is an error, not a SQLite URL */
  CHECK_STR_FREE(conn_target_to_connstr("  postgres://u@h/d ", &err), "postgres://u@h/d");
  CHECK_STR_FREE(conn_target_to_connstr("sqlite:///x.db", &err), "sqlite:///x.db");
  CHECK(conn_target_to_connstr("", &err) == NULL);
  free(err);
  err = NULL;
  CHECK(conn_target_to_connstr("/nonexistent/lace-test.db", &err) == NULL && err != NULL);
  free(err);
}


/* The connection form's fields: filled from a saved connection, synced with
 * the URI line both ways, dirty when they differ from what was filled */
#define MASK "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"
static void test_conn_form_fill(void) {
  ConnFields f;
  conn_fields_init(&f);
  text_input_set(&f.in[CF_PASS], "typed");
  LaceSavedConnection *c = lace_saved_conn_from_connstr("postgres://app@db1:5432/shop", NULL);
  CHECK(c != NULL);
  if (!c) return;
  free(c->name);
  c->name = str_dup("shop on db1");
  conn_fields_fill(&f, c);
  CHECK_STR(f.in[CF_HOST].buf, "db1");
  CHECK_STR(f.in[CF_NAME].buf, "shop on db1");
  CHECK_STR(f.in[CF_PORT].buf, "5432");
  CHECK(f.in[CF_PASS].len == 0); /* never kept from before */
  CHECK(!conn_fields_dirty(&f));
  CHECK(conn_fields_visible(&f, CF_HOST) && !conn_fields_visible(&f, CF_FILE));
  CHECK(!conn_fields_visible(&f, CF_SSHHOST));
  /* a saved password comes along only when it is saved */
  c->save_password = true;
  free(c->password);
  c->password = str_dup("pw");
  conn_fields_fill(&f, c);
  CHECK_STR(f.in[CF_PASS].buf, "pw");
  conn_fields_clear(&f);
  CHECK(f.in[CF_HOST].len == 0 && f.in[CF_PASS].len == 0 && !conn_fields_dirty(&f));
  lace_saved_conn_free(c);
  conn_fields_free(&f);
}

static void test_conn_form_uri(void) {
  ConnFields f;
  conn_fields_init(&f);
  CHECK(conn_fields_set_uri(&f, "postgres://app:s3cret@db1:5432/shop"));
  CHECK_STR(f.in[CF_PASS].buf, "s3cret");
  CHECK_STR(f.in[CF_DB].buf, "shop");
  CHECK_STR_FREE(conn_fields_uri(&f), "postgres://app:" MASK "@db1:5432/shop");
  char *cs = conn_fields_connstr(&f);
  CHECK(cs && strstr(cs, "s3cret") != NULL);
  str_secure_free(cs);
  /* the masked line typed back keeps the password */
  CHECK(conn_fields_set_uri(&f, "postgres://app:" MASK "@db2:5432/shop"));
  CHECK_STR(f.in[CF_PASS].buf, "s3cret");
  CHECK_STR(f.in[CF_HOST].buf, "db2");
  /* a password typed into the line sets the field */
  CHECK(conn_fields_set_uri(&f, "postgres://app:other@db2:5432/shop"));
  CHECK_STR(f.in[CF_PASS].buf, "other");
  /* a URI that parses while still being typed fills what it names (the form applies the line only when it is left) */
  ConnFields g;
  conn_fields_init(&g);
  CHECK(conn_fields_set_uri(&g, "postgres://ap"));
  CHECK_STR(g.in[CF_HOST].buf, "ap");
  conn_fields_free(&g);
  /* a half URI leaves the fields and says why */
  CHECK(!conn_fields_set_uri(&f, "postgres:/"));
  CHECK_STR(f.in[CF_HOST].buf, "db2");
  CHECK(f.message != NULL && f.message_error);
  /* ssh survives the round trip */
  CHECK(conn_fields_set_uri(&f, "postgres+ssh://su@jump:22/app@db1:5432/shop"));
  CHECK(f.chk[CF_SSH]);
  CHECK_STR(f.in[CF_SSHHOST].buf, "jump");
  CHECK_STR(f.in[CF_HOST].buf, "db1");
  char *u = conn_fields_uri(&f);
  CHECK(u && strncmp(u, "postgres+ssh://", 15) == 0);
  free(u);
  /* the name is the user's, never taken from a URI */
  text_input_set(&f.in[CF_NAME], "mine");
  CHECK(conn_fields_set_uri(&f, "mysql://root@localhost/x"));
  CHECK_STR(f.in[CF_NAME].buf, "mine");
  CHECK(!f.chk[CF_SSH]);
  /* a bare file path is SQLite */
  char path[] = "/tmp/lace-cf-XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  if (fd >= 0) {
    CHECK(write(fd, "SQLite format 3", 16) == 16);
    close(fd);
  }
  CHECK(conn_fields_set_uri(&f, path));
  CHECK(f.driver == 0);
  /* the path comes back resolved (macOS: /tmp is /private/tmp) */
  char *real = realpath(path, NULL);
  CHECK_STR(f.in[CF_FILE].buf, real ? real : path);
  free(real);
  unlink(path);
  conn_fields_free(&f);
}

static void test_conn_form_dirty(void) {
  ConnFields f;
  conn_fields_init(&f);
  CHECK(!conn_fields_dirty(&f));
  text_input_set(&f.in[CF_PASS], "x"); /* a typed password alone is an edit */
  CHECK(conn_fields_dirty(&f));
  conn_fields_mark_clean(&f);
  CHECK(!conn_fields_dirty(&f));
  f.chk[CF_SSH] = true;
  CHECK(conn_fields_dirty(&f));
  conn_fields_mark_clean(&f);
  f.driver = 2;
  CHECK(conn_fields_dirty(&f));
  conn_fields_free(&f);
}

static void test_edit_decide(void) {
  LaceValue orig = lace_value_text("abc"); LaceValue out; char *err = NULL;
  CHECK(edit_decide(&orig, LACE_TYPE_TEXT, "abc", false, false, &out, &err) == EDIT_NOTHING);
  CHECK(edit_decide(&orig, LACE_TYPE_TEXT, "", false, false, &out, &err) == EDIT_WRITE && out.type == LACE_TYPE_TEXT && out.text.len == 0);
  lace_value_free(&out);
  CHECK(edit_decide(&orig, LACE_TYPE_TEXT, NULL, true, false, &out, &err) == EDIT_WRITE && out.is_null);
  lace_value_free(&out);
  /* a number column takes text too: the database decides (liblace's rule) */
  CHECK(edit_decide(&orig, LACE_TYPE_INT, "12x", false, false, &out, &err) == EDIT_WRITE && out.type == LACE_TYPE_TEXT);
  lace_value_free(&out);
  LaceValue omitted = {0}; omitted.type = LACE_TYPE_TEXT; omitted.omitted = 5000; omitted.text.data = str_dup("[TEXT: 5000 bytes]"); omitted.text.len = strlen(omitted.text.data);
  CHECK(edit_decide(&omitted, LACE_TYPE_TEXT, "x", false, false, &out, &err) == EDIT_REFUSED && err);
  free(err); err = NULL;
  CHECK(edit_decide(&omitted, LACE_TYPE_TEXT, NULL, true, false, &out, &err) == EDIT_REFUSED);
  free(err); err = NULL;
  lace_value_free(&omitted);
  LaceValue nul = lace_value_null();
  CHECK(edit_decide(&nul, LACE_TYPE_TEXT, NULL, true, false, &out, &err) == EDIT_NOTHING);
  /* empty text over NULL is no change, unless it is the explicit action */
  CHECK(edit_decide(&nul, LACE_TYPE_TEXT, "", false, false, &out, &err) == EDIT_NOTHING);
  CHECK(edit_decide(&nul, LACE_TYPE_TEXT, "", false, true, &out, &err) == EDIT_WRITE && !out.is_null && out.text.len == 0);
  lace_value_free(&out);
  lace_value_free(&nul);
  lace_value_free(&orig);
}

static void test_base64(void) {
  CHECK_STR_FREE(edit_base64((const unsigned char *)"", 0), "");
  CHECK_STR_FREE(edit_base64((const unsigned char *)"f", 1), "Zg==");
  CHECK_STR_FREE(edit_base64((const unsigned char *)"fo", 2), "Zm8=");
  CHECK_STR_FREE(edit_base64((const unsigned char *)"foo", 3), "Zm9v");
  CHECK_STR_FREE(edit_base64((const unsigned char *)"foobar", 6), "Zm9vYmFy");
}

static void test_textbuf(void) {
  TextBuf tb;
  textbuf_init(&tb, "ab\ncde\n\nf");
  CHECK(textbuf_num_lines(&tb) == 4);
  tb.cursor = 0;
  tb.want_col = 0;
  textbuf_move_line(&tb, 1);
  size_t line, col;
  textbuf_line_col(&tb, &line, &col);
  CHECK(line == 1 && col == 0);
  /* the wanted column survives a short line */
  tb.cursor = 5; /* "cd|e" */
  tb.want_col = 2;
  textbuf_move_line(&tb, 1);   /* the empty line */
  textbuf_line_col(&tb, &line, &col);
  CHECK(line == 2 && col == 0);
  textbuf_move_line(&tb, 1);   /* "f": col clamps to 1 */
  textbuf_line_col(&tb, &line, &col);
  CHECK(line == 3 && col == 1);
  textbuf_move_line(&tb, -1);
  textbuf_move_line(&tb, -1);
  textbuf_line_col(&tb, &line, &col);
  CHECK(line == 1 && col == 2);
  CHECK(textbuf_cursor_at(&tb, 1, 99) == 6);
  /* insert and delete, with a multi-byte character */
  textbuf_set(&tb, "");
  textbuf_insert(&tb, "h\xc3\xa9", 3);
  textbuf_insert(&tb, "\n", 1);
  textbuf_insert(&tb, "x", 1);
  CHECK(strcmp(tb.text, "h\xc3\xa9\nx") == 0 && tb.cursor == tb.len);
  textbuf_delete(&tb, true);
  textbuf_delete(&tb, true);   /* removes the newline */
  CHECK(strcmp(tb.text, "h\xc3\xa9") == 0);
  textbuf_delete(&tb, true);   /* removes the 2-byte e-acute whole */
  CHECK(strcmp(tb.text, "h") == 0);
  tb.cursor = 0;
  textbuf_delete(&tb, false);
  CHECK(tb.len == 0);
  textbuf_free(&tb);
}

static void test_sqlhl(void) {
  const char *s = "SELECT 'a;b' -- c\nFROM t WHERE x = 42";
  HlClass c;
  size_t p = 0, n;
  n = sqlhl_next(s, p, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_KEYWORD && n == 6); p += n;
  n = sqlhl_next(s, p, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_PLAIN && n == 1); p += n;   /* space */
  n = sqlhl_next(s, p, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_STRING && n == 5); p += n + 1;
  n = sqlhl_next(s, p, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_COMMENT && n == 4);
  p = strlen(s) - 2;
  n = sqlhl_next(s, p, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_NUMBER && n == 2);
  /* lower-case keywords, identifiers that merely start like one, an unterminated string */
  n = sqlhl_next("select x", 0, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_KEYWORD && n == 6);
  n = sqlhl_next("selection", 0, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_PLAIN && n == 9);
  n = sqlhl_next("t1", 0, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_PLAIN && n == 2);
  n = sqlhl_next("'abc", 0, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_STRING && n == 4);
  n = sqlhl_next("(", 0, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_PLAIN && n == 1);
  n = sqlhl_next("/* x */ y", 0, LACE_DRIVER_SQLITE, &c); CHECK(c == HL_COMMENT && n == 7);
  /* walking a whole text covers every byte exactly once */
  size_t total = 0, len = strlen(s);
  while (total < len) { n = sqlhl_next(s, total, LACE_DRIVER_SQLITE, &c); CHECK(n > 0); total += n; }
  CHECK(total == len);
}

static void test_textbuf_statement(void) {
  TextBuf tb;
  textbuf_init(&tb, "select 1;\nselect 2");
  size_t a, b;
  tb.cursor = 3;
  CHECK(textbuf_statement_at(&tb, LACE_DRIVER_SQLITE, &a, &b) && a == 0 && b == 8);
  tb.cursor = tb.len;
  CHECK(textbuf_statement_at(&tb, LACE_DRIVER_SQLITE, &a, &b) && a == 10 && b == tb.len);
  textbuf_set(&tb, "  ");
  CHECK(!textbuf_statement_at(&tb, LACE_DRIVER_SQLITE, &a, &b));
  textbuf_free(&tb);
}

static void test_log_ring(void) {
  NcApp app;
  memset(&app, 0, sizeof app);
  for (int i = 0; i < 505; i++) app_log(&app, false, "line %d", i);
  CHECK(app.num_log == 500);
  CHECK(app.log_seq == 505);
  CHECK(strstr(app.log[0].line, "line 5") == app.log[0].line + 10); /* after "HH:MM:SS  " */
  CHECK(strstr(app.log[499].line, "line 504") != NULL);
  for (size_t i = 0; i < app.num_log; i++) str_secure_free(app.log[i].line);
  free(app.log);
}

static void test_tabstrip_entries(void) {
  NcWorkspace *w = workspace_new("W");
  ConnTab *a = workspace_add_tab(w, NULL);
  conntab_add_view(a, view_new_table("x"));
  conntab_add_view(a, view_new_table("y"));
  StripEntry *e;
  size_t cur;
  CHECK(tabstrip_entries(w, &e, &cur) == 2 && cur == 1 && e[0].tab == 0 && e[1].view == 1);
  free(e);
  ConnTab *b = workspace_add_tab(w, NULL); /* becomes current, no views: its own cell */
  CHECK(tabstrip_entries(w, &e, &cur) == 3 && cur == 2 && e[2].tab == 1 && e[2].view == SIZE_MAX);
  free(e);
  conntab_add_view(b, view_new_query("SELECT 1"));
  workspace_add_tab(w, NULL);              /* an empty tab is one cell */
  w->current = 1;
  /* one flat row: tab by tab, no group markers */
  size_t n = tabstrip_entries(w, &e, &cur);
  CHECK(n == 4 && cur == 2);
  CHECK(e[0].tab == 0 && e[0].view == 0 && e[1].tab == 0 && e[1].view == 1 && e[2].tab == 1 && e[2].view == 0);
  CHECK(e[3].tab == 2 && e[3].view == SIZE_MAX);
  free(e);
  w->current = 2; /* the current tab has no view: its cell is current */
  CHECK(tabstrip_entries(w, &e, &cur) == 4 && cur == 3);
  free(e);
  workspace_free(w);
}

/* The undo stack: newest last, capped, cleared with the view */
static void test_undo_stack(void) {
  View *v = view_new_table("t");
  LaceRowKey key = {0};
  key.names = safe_calloc(1, sizeof *key.names);
  key.values = safe_calloc(1, sizeof *key.values);
  key.names[0] = str_dup("id");
  key.values[0] = lace_value_int(1);
  key.count = 1;
  LaceValue before = lace_value_text("old"), after = lace_value_text("new");
  for (int i = 0; i < UNDO_MAX + 5; i++) {
    lace_value_free(&before);
    char buf[16];
    snprintf(buf, sizeof buf, "old%d", i);
    before = lace_value_text(buf);
    undo_stack_push_cell(&v->undo, "t", &key, "name", &before, &after);
  }
  UndoEntry *it = v->undo.items;
  CHECK(v->undo.n == UNDO_MAX); /* the oldest five went */
  CHECK(it[0].kind == UNDO_CELL && strcmp(it[0].before.text.data, "old5") == 0);
  CHECK(strcmp(it[UNDO_MAX - 1].before.text.data, "old204") == 0);
  CHECK(strcmp(it[0].column, "name") == 0 && it[0].key.count == 1 && it[0].key.values[0].int_val == 1);
  /* a delete step takes its rows */
  UndoRow *rows = safe_calloc(2, sizeof *rows);
  rows[0].names = safe_calloc(2, sizeof *rows[0].names);
  rows[0].values = safe_calloc(2, sizeof *rows[0].values);
  rows[0].names[0] = str_dup("id");
  rows[0].values[0] = lace_value_int(7);
  rows[0].count = 1;
  undo_stack_push_delete(&v->undo, "t", rows, 1, NULL);
  it = v->undo.items;
  CHECK(v->undo.n == UNDO_MAX && it[UNDO_MAX - 1].kind == UNDO_DELETE &&
        it[UNDO_MAX - 1].num_rows == 1 && it[UNDO_MAX - 1].rows[0].values[0].int_val == 7);
  /* a bulk delete recorded row by row becomes one step */
  for (int i = 0; i < 2; i++) {
    UndoRow *r = safe_calloc(2, sizeof *r);
    r[0].names = safe_calloc(2, sizeof *r[0].names);
    r[0].values = safe_calloc(2, sizeof *r[0].values);
    r[0].names[0] = str_dup("id");
    r[0].values[0] = lace_value_int(10 + i);
    r[0].count = 1;
    undo_stack_push_delete(&v->undo, "t", r, 1, i == 1 ? str_dup("too large") : NULL);
  }
  undo_stack_merge_last_deletes(&v->undo, 3);
  it = v->undo.items;
  CHECK(v->undo.n == UNDO_MAX - 2 && undo_stack_top(&v->undo)->num_rows == 3 &&
        undo_stack_top(&v->undo)->rows[2].values[0].int_val == 11 && undo_stack_top(&v->undo)->not_restorable);
  undo_stack_pop(&v->undo);
  CHECK(v->undo.n == UNDO_MAX - 3 && undo_stack_top(&v->undo)->kind == UNDO_CELL);
  undo_stack_clear(&v->undo);
  CHECK(v->undo.n == 0 && v->undo.items == NULL);
  lace_row_key_free(&key);
  lace_value_free(&before);
  lace_value_free(&after);
  view_free(v); /* frees a stack too */
}

static void test_tabstrip_layout(void) {
  int w[] = {6, 9, 4, 12};
  int x0[4], x1[4];
  /* everything fits: cells of width + 2, one column between */
  CHECK(tabstrip_layout(w, 4, 0, 80, x0, x1) == 0);
  CHECK(x0[0] == 0 && x1[0] == 8 && x0[1] == 9 && x1[1] == 20 && x0[2] == 21 && x1[2] == 27 && x0[3] == 28 && x1[3] == 42);
  /* too narrow for the last: it is off the strip, a mark at the right */
  CHECK(tabstrip_layout(w, 4, 0, 30, x0, x1) == 0);
  CHECK(x1[2] == 27 && x0[3] == 0 && x1[3] == 0);
  /* the current one is the last: scrolled so that it shows, a mark at the left */
  size_t first = tabstrip_layout(w, 4, 3, 30, x0, x1);
  CHECK(first > 0 && x1[3] > x0[3] && x0[first] == 2);
  for (size_t i = 0; i < first; i++) CHECK(x0[i] == 0 && x1[i] == 0);
  /* a label wider than the strip: still placed, clipped by the draw */
  int big[] = {60};
  CHECK(tabstrip_layout(big, 1, 0, 30, x0, x1) == 0 && x0[0] == 0 && x1[0] == 0);
  CHECK(tabstrip_layout(w, 0, 0, 30, x0, x1) == 0);
}

/* The structure editors' pure parts: the form's name lists, the lock
 * banner's text, the header's view segment */
static void test_structure_helpers(void) {
  char **names = NULL;
  CHECK_SIZE(form_split_names(" a, b ,,c ", &names), 3);
  CHECK_STR(names[0], "a");
  CHECK_STR(names[1], "b");
  CHECK_STR(names[2], "c");
  lace_tables_free(names, 3);
  CHECK_SIZE(form_split_names("", &names), 0);
  CHECK(names == NULL);
  CHECK_SIZE(form_split_names(NULL, &names), 0);
  CHECK_SIZE(form_split_names("solo", &names), 1);
  CHECK_STR(names[0], "solo");
  lace_tables_free(names, 1);

  /* the first line, cut to fit before the time and "Esc cancels" */
  CHECK_STR_FREE(content_banner_text("SELECT 1\nFROM t", 1500, 80), "SELECT 1  1.5 s  Esc cancels");
  CHECK_STR_FREE(content_banner_text("UPDATE", 61000, 80), "UPDATE  1:01  Esc cancels");
  char *cut = content_banner_text("SELECT a_very_long_column_name_indeed FROM somewhere", 100, 40);
  CHECK(nc_text_width(cut) <= 40);
  CHECK(strstr(cut, "\xe2\x80\xa6  0.1 s  Esc cancels") != NULL);
  free(cut);
  CHECK_STR_FREE(content_banner_text("a\tb", 0, 80), "a b  0.0 s  Esc cancels");

  /* the view's name is a byte range of the line, after the database's */
  const char *one[] = {"db"};
  size_t voff = 0, vlen = 0, dboff = 0, dblen = 0;
  char *h = header_format_ex("W", one, 1, 0, "shop", "orders", 0, 0, 80, NULL, NULL, &dboff, &dblen, &voff, &vlen);
  CHECK_STR(h, "W \xe2\x96\xb8 db \xe2\x96\xb8 shop \xe2\x96\xb8 orders");
  CHECK(vlen == 6 && strncmp(h + voff, "orders", 6) == 0);
  CHECK(dblen == 4 && strncmp(h + dboff, "shop", 4) == 0);
  free(h);
  h = header_format_ex("W", one, 1, 0, NULL, "orders", 2, 5, 80, NULL, NULL, NULL, NULL, &voff, &vlen);
  CHECK_STR(h, "W \xe2\x96\xb8 db \xe2\x96\xb8 orders (3 of 5)");
  CHECK(vlen == 6 && strncmp(h + voff, "orders", 6) == 0);
  free(h);
  h = header_format_ex("W", one, 1, 0, NULL, "orders", 0, 0, 8, NULL, NULL, NULL, NULL, &voff, &vlen); /* cut: "db \xe2\x96\xb8 ord" */
  CHECK(vlen == 3 && strncmp(h + voff, "ord", 3) == 0);
  free(h);
  h = header_format_ex("W", one, 1, 0, NULL, "orders", 0, 0, 4, NULL, NULL, NULL, NULL, &voff, &vlen); /* cut away */
  CHECK(vlen == 0);
  free(h);
}

static void test_filter_panel_helpers(void) {
  /* height: title + rows, at most FP_MAX_ROWS rows, and FP_GRID_KEEP grid rows kept */
  CHECK(filter_panel_height(0, 30) == 2);
  CHECK(filter_panel_height(3, 30) == 4);
  CHECK(filter_panel_height(9, 30) == 1 + FP_MAX_ROWS);
  CHECK(filter_panel_height(9, 8) == 4);  /* 8 - 4 */
  CHECK(filter_panel_height(1, 5) == 0);  /* no room */
  FpLayout L;
  filter_panel_layout(100, false, &L);
  CHECK(L.col_x == 1 && L.col_w == 14 && L.op_x == 17 && L.op_w == 12 && L.val_x == 31 && L.del_x == 98);
  CHECK(L.val_w == 98 - 31 - 1 && L.val2_w == 0);
  filter_panel_layout(100, true, &L);
  CHECK(L.val_w > 3 && L.val2_x == L.val_x + L.val_w + 5 && L.val2_x + L.val2_w <= L.del_x);
  filter_panel_layout(50, false, &L);
  CHECK(L.col_w == 10 && L.op_w == 9 && L.val_x < L.del_x);
  /* the view's rows: a blank row is inactive, invisible to WHERE, header and session */
  View *v = view_new_table("t");
  view_set_schema(v, schema_id_name());
  size_t i = view_add_filter(v, "id");
  CHECK(i == 0 && v->num_filters == 1 && !view_filter_active(&v->filters[0]));
  CHECK(view_where(v, LACE_DRIVER_SQLITE) == NULL);
  CHECK_STR_FREE(header_sorts_filters(v), "");
  view_filter_set(v, 0, v->filters[0].column, LACE_FILTER_EQ, "7", NULL); /* the column points into the filter */
  CHECK(view_filter_active(&v->filters[0]));
  CHECK_STR_FREE(view_where(v, LACE_DRIVER_SQLITE), "\"id\" = 7");
  view_filter_set(v, 0, "id", LACE_FILTER_NULL, "", NULL);
  CHECK(view_filter_active(&v->filters[0]));
  view_filter_set(v, 0, "id", LACE_FILTER_BETWEEN, "1", NULL);
  CHECK(!view_filter_active(&v->filters[0])); /* the upper bound is missing */
  view_filter_set(v, 0, NULL, LACE_FILTER_RAW, "  ", NULL);
  CHECK(!view_filter_active(&v->filters[0]) && v->filters[0].column == NULL);
  view_filter_set(v, 0, NULL, LACE_FILTER_RAW, "id > 3", NULL);
  CHECK(view_filter_active(&v->filters[0]));
  view_add_filter(v, "name");
  view_add_filter(v, NULL);
  CHECK(v->num_filters == 3 && v->filters[2].op == LACE_FILTER_RAW);
  cJSON *j = view_to_json(v, false);
  CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(j, "filters")) == 1);
  cJSON_Delete(j);
  view_prune_filters(v);
  CHECK(v->num_filters == 1 && view_filter_active(&v->filters[0]));
  view_free(v);
}

int main(void) {
  if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8")) {
    fprintf(stderr, "no UTF-8 locale available\n");
    return 1;
  }
  test_session_file(); /* first: the config directory is cached on first use */
  test_sidebar_marquee();
  test_conntab_preview();
  test_workspace_json();
  test_subviews_model();
  test_input_translate();
  test_page_cache();
  test_view_sql();
  test_view_state();
  test_view_more();
  test_page_cache_soft_reset();
  test_view_checkpoint();
  test_reload_twice_then_cancel();
  test_header_table_state();
  test_mainq();
  test_connection_jobs();
  test_connection_burst();
  test_connection_free_running();
  test_connection_free_disconnects();
  test_detach_cancels_running();
  test_conn_key_matches();
  test_conn_registry();
  test_escape_spares_commit();
  test_palette_score();
  test_list_widget();
  test_text_input_key();
  test_header_format();
  test_connect_helpers();
  test_conn_form_fill();
  test_conn_form_uri();
  test_conn_form_dirty();
  test_edit_decide();
  test_base64();
  test_textbuf();
  test_sqlhl();
  test_textbuf_statement();
  test_log_ring();
  test_tabstrip_layout();
  test_tabstrip_entries();
  test_undo_stack();
  test_filter_panel_helpers();
  test_structure_helpers();
  return test_report("nc");
}
