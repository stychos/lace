/*
 * Lace
 * Fuzz: wire values and results, edit text, SQL literals. A value liblace
 * decodes, encoded again by the daemon's encoder, decodes to the same value.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "fuzz.h"
#include "../../liblace/src/client_internal.h"
#include "../../liblace/src/rpc.h"
#include "daemon_encode.h"
#include <cjson/cJSON.h>

static bool same_value(const LaceValue *a, const LaceValue *b) {
  if (a->is_null || b->is_null)
    return a->is_null == b->is_null;
  if (a->type != b->type || a->omitted != b->omitted)
    return false;
  switch (a->type) {
  case LACE_TYPE_BLOB:
    return a->blob.len == b->blob.len &&
           (a->blob.len == 0 || memcmp(a->blob.data, b->blob.data, a->blob.len) == 0);
  case LACE_TYPE_INT:
    return a->int_val == b->int_val;
  case LACE_TYPE_FLOAT:
    return a->float_val == b->float_val ||
           (a->float_val != a->float_val && b->float_val != b->float_val);
  case LACE_TYPE_BOOL:
    return a->bool_val == b->bool_val;
  default:
    return a->text.len == b->text.len &&
           (a->text.len == 0 || memcmp(a->text.data, b->text.data, a->text.len) == 0);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  cJSON *json = cJSON_ParseWithLength((const char *)data, size);
  if (!json)
    return 0;

  LaceValue v;
  memset(&v, 0, sizeof(v));
  if (lace_rpc_parse_value(json, &v)) {
    /* Through the daemon's encoder and back: the same value */
    const void *bytes = v.type == LACE_TYPE_BLOB ? (const void *)v.blob.data
                                                 : (const void *)v.text.data;
    size_t blen = v.type == LACE_TYPE_BLOB ? v.blob.len : v.text.len;
    bool has_bytes = v.type == LACE_TYPE_BLOB || v.type == LACE_TYPE_TEXT ||
                     v.type == LACE_TYPE_DATE || v.type == LACE_TYPE_TIMESTAMP ||
                     v.type == LACE_TYPE_DECIMAL;
    cJSON *j2 = fuzz_daemon_encode(
        (int)v.type, v.is_null, v.omitted, v.type == LACE_TYPE_INT ? v.int_val : 0,
        v.type == LACE_TYPE_FLOAT ? v.float_val : 0, has_bytes ? bytes : NULL,
        has_bytes ? blen : 0, v.type == LACE_TYPE_BOOL && v.bool_val);
    LaceValue v2;
    memset(&v2, 0, sizeof(v2));
    if (!j2 || !lace_rpc_parse_value(j2, &v2) || !same_value(&v, &v2))
      FUZZ_FAIL("value does not survive the wire round trip");
    lace_value_free(&v2);
    cJSON_Delete(j2);

    /* Edit text and back, for the types that convert exactly */
    if (!v.is_null && !v.omitted &&
        (v.type == LACE_TYPE_BLOB || v.type == LACE_TYPE_INT ||
         v.type == LACE_TYPE_DECIMAL || v.type == LACE_TYPE_TEXT)) {
      char *text = lace_value_to_edit_text(&v);
      LaceValue v3;
      char *err = NULL;
      bool ok = text && lace_value_from_edit_text(text, v.type, &v3, &err);
      if (v.type != LACE_TYPE_DECIMAL || (ok && v3.type == LACE_TYPE_DECIMAL)) {
        if (!ok || !same_value(&v, &v3))
          FUZZ_FAIL("edit text [%s] does not give the value back", text ? text : "");
      }
      if (ok)
        lace_value_free(&v3);
      free(err);
      free(text);
    }

    /* Its SQL literal stays one literal on every driver */
    for (size_t d = 0; d < 4 && !v.omitted; d++) {
      char *lit = lace_value_to_sql_literal(&v, fuzz_drivers[d]);
      if (lit)
        fuzz_check_contained(lit, fuzz_drivers[d], ".-+:()", "value literal", "(value)");
      free(lit);
    }
    lace_value_free(&v);
  }

  lace_result_free(lace_rpc_parse_result(json));
  lace_schema_free(lace_rpc_parse_schema(json));
  cJSON_Delete(json);
  return 0;
}
