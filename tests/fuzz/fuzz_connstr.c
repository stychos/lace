/*
 * Lace
 * Fuzz: connection strings and URL coding (liblace)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "fuzz.h"
#include "util/connstr.h"
#include "util/str.h"

static bool same(const char *a, const char *b) {
  return strcmp(a ? a : "", b ? b : "") == 0;
}

/* Characters a host can hold in a URL as connstr_build writes it */
static bool plain_host(const char *h) {
  return h && h[0] && !strpbrk(h, ":/@?#[]%&= ");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *s = fuzz_cstr(data, size);

  char *err = NULL;
  ConnString *cs = connstr_parse(s, &err);
  free(err);
  if (cs) {
    err = NULL;
    (void)connstr_validate(cs, &err);
    free(err);
    (void)connstr_get_port(cs);
    free(connstr_mask_password(s));

    /* What parsed builds back into a string that parses the same */
    bool network = !cs->ssh && cs->driver &&
                   (strcmp(cs->driver, "postgres") == 0 ||
                    strcmp(cs->driver, "mysql") == 0);
    if (network && plain_host(cs->host) && cs->num_options == 0) {
      char *built = connstr_build(cs->driver, cs->user, cs->password, cs->host,
                                  cs->port, cs->database, NULL, NULL, 0);
      ConnString *again = built ? connstr_parse(built, NULL) : NULL;
      if (!again)
        FUZZ_FAIL("rebuilt [%s] from [%s] does not parse", built ? built : "", s);
      if (!same(cs->user, again->user) || !same(cs->password, again->password) ||
          !same(cs->host, again->host) || !same(cs->database, again->database) ||
          cs->port != again->port)
        FUZZ_FAIL("[%s] rebuilt as [%s] parses differently", s, built);
      connstr_free(again);
      free(built);
    }
    connstr_free(cs);
  }

  /* URL coding round trips */
  char *enc = str_url_encode(s);
  char *dec = enc ? str_url_decode(enc) : NULL;
  if (!dec || strcmp(dec, s) != 0)
    FUZZ_FAIL("url decode(encode([%s])) = [%s]", s, dec ? dec : "(null)");
  free(enc);
  free(dec);
  free(str_url_decode(s));

  /* A SQLite path survives connstr_encode_path and parsing */
  if (s[0] == '/') {
    char *p = connstr_encode_path(s);
    char *url = str_printf("sqlite://%s", p);
    ConnString *sq = connstr_parse(url, NULL);
    if (!sq || !same(sq->database, s))
      FUZZ_FAIL("path [%s] as [%s] parses as [%s]", s, url,
                sq && sq->database ? sq->database : "(null)");
    connstr_free(sq);
    free(url);
    free(p);
  }
  free(s);
  return 0;
}
