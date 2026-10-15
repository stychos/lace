/*
 * Lace
 * Fuzz helper: encode a value the way the daemon puts it on the wire
 * (laced_json_from_value). A separate file because the daemon's types
 * (DbValue) and liblace's (LaceValue) do not share a translation unit.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "json.h"
#include "daemon_encode.h"

cJSON *fuzz_daemon_encode(int type, bool is_null, size_t omitted, int64_t i,
                          double f, const void *bytes, size_t len, bool b) {
  DbValue v;
  memset(&v, 0, sizeof(v));
  v.type = (DbValueType)type; /* Same codes as LaceValueType */
  v.is_null = is_null;
  v.omitted = omitted;
  switch (v.type) {
  case DB_TYPE_INT:
    v.int_val = i;
    break;
  case DB_TYPE_FLOAT:
    v.float_val = f;
    break;
  case DB_TYPE_BOOL:
    v.bool_val = b;
    break;
  case DB_TYPE_BLOB:
    v.blob.data = (uint8_t *)bytes;
    v.blob.len = len;
    break;
  default:
    v.text.data = (char *)bytes;
    v.text.len = len;
    break;
  }
  return laced_json_from_value(&v);
}
