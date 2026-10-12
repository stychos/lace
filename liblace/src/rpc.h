/*
 * liblace - Lace Client Library
 * JSON-RPC marshaling interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_RPC_H
#define LIBLACE_RPC_H

#include "../include/lace.h"
#include <cjson/cJSON.h>

/* ==========================================================================
 * RPC Call
 * ========================================================================== */

/*
 * Make a JSON-RPC call to the daemon.
 *
 * @param client  Client handle
 * @param method  Method name
 * @param params  Parameters (NULL for none, ownership NOT transferred)
 * @param result  Output: result JSON (caller must cJSON_Delete)
 * @return        LACE_OK on success, error code on failure
 */
int lace_rpc_call(lace_client_t *client, const char *method,
                  cJSON *params, cJSON **result);

/* Like lace_rpc_call, but returns LACE_ERR_TIMEOUT immediately instead of
 * waiting when another call holds the client. */
int lace_rpc_try_call(lace_client_t *client, const char *method,
                      cJSON *params, cJSON **result);

/* stdio only: send method as a notification (no id, no reply) even while
 * another thread holds the single link and waits for its reply. The daemon
 * runs it on a worker and answers nothing, so the waiting reader sees no
 * extra line. LACE_ERR_INVALID_REQUEST if the client is not stdio. */
int lace_rpc_notify(lace_client_t *client, const char *method, cJSON *params);

/* ==========================================================================
 * JSON to Types Conversion
 * ========================================================================== */

/*
 * Parse a LaceResult from JSON response.
 *
 * @param json  JSON object with columns, rows, etc.
 * @return      Allocated result (caller must lace_result_free), or NULL on error
 */
LaceResult *lace_rpc_parse_result(cJSON *json);

/*
 * Parse a LaceSchema from JSON response.
 *
 * @param json  JSON object with schema info
 * @return      Allocated schema (caller must lace_schema_free), or NULL on error
 */
LaceSchema *lace_rpc_parse_schema(cJSON *json);

/*
 * Parse a LaceValue from JSON.
 *
 * @param json  JSON value
 * @param val   Output: parsed value
 * @return      true on success, false on error
 */
bool lace_rpc_parse_value(cJSON *json, LaceValue *val);

#endif /* LIBLACE_RPC_H */
