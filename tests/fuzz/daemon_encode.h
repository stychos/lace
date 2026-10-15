/*
 * Lace
 * Fuzz helper: the daemon's value encoder, callable without its types
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FUZZ_DAEMON_ENCODE_H
#define LACE_FUZZ_DAEMON_ENCODE_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

cJSON *fuzz_daemon_encode(int type, bool is_null, size_t omitted, int64_t i,
                          double f, const void *bytes, size_t len, bool b);

#endif
