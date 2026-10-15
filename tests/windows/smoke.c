/*
 * Lace
 * Windows smoke test: a Windows build of liblace (os_win32.c) talking to a
 * real daemon over TCP. Run by tests/windows/run.sh under Wine.
 *
 *   smoke.exe <port> <token> <sqlite url>
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../test.h"
#include "lace.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: smoke <port> <token> <sqlite url>\n");
    return 2;
  }
  LaceClientConfig cfg = {0};
  cfg.mode = LACE_CONN_TCP;
  cfg.host = "127.0.0.1";
  cfg.port = atoi(argv[1]);
  lace_client_t *c = lace_client_create_with_config(&cfg);
  lace_client_set_token(c, argv[2]);
  CHECK(lace_client_connected(c));
  CHECK(lace_ping(c) == LACE_OK);

  int conn = 0;
  CHECK(lace_connect(c, argv[3], NULL, &conn) == LACE_OK);
  CHECK(lace_exec(c, conn, "CREATE TABLE w (id INTEGER PRIMARY KEY, s TEXT)", NULL) == LACE_OK);

  /* A value with quotes and non-ASCII text, written through liblace */
  const char *cols[] = {"id", "s"};
  LaceValue vals[] = {lace_value_int(1), lace_value_text("it's \xc3\xa9t\xc3\xa9")};
  CHECK(lace_insert(c, conn, "w", cols, vals, 2) == LACE_OK);
  lace_value_free(&vals[1]);

  /* A large result: many reads on the socket */
  LaceResult *r = NULL;
  CHECK(lace_exec(c, conn,
                  "WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s "
                  "WHERE n < 50000) SELECT n, printf('row %d', n) FROM s",
                  &r) == LACE_OK);
  CHECK(r && r->num_rows == 50000);
  lace_result_free(r);

  r = NULL;
  CHECK(lace_exec(c, conn, "SELECT s FROM w WHERE id = 1", &r) == LACE_OK);
  CHECK(r && r->num_rows == 1 && r->rows[0].cells[0].text.data &&
        strcmp(r->rows[0].cells[0].text.data, "it's \xc3\xa9t\xc3\xa9") == 0);
  lace_result_free(r);

  /* Errors come back as codes */
  CHECK(lace_exec(c, conn, "SELECT * FROM missing", NULL) == LACE_ERR_QUERY_FAILED);

  /* A wrong token is refused */
  lace_client_t *bad = lace_client_create_with_config(&cfg);
  lace_client_set_token(bad, "wrong");
  CHECK(lace_ping(bad) != LACE_OK);
  lace_client_destroy(bad);

  /* The daemon goes away: calls fail cleanly instead of hanging */
  CHECK(lace_shutdown(c) == LACE_OK);
  CHECK(lace_exec(c, conn, "SELECT 1", NULL) != LACE_OK);
  lace_client_destroy(c);
  return test_report("windows smoke");
}
