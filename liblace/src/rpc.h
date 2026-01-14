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

/* ==========================================================================
 * Types to JSON Conversion
 * ========================================================================== */

/*
 * Convert a LaceValue to JSON.
 *
 * @param val  Value to convert
 * @return     JSON value (caller must cJSON_Delete), or NULL on error
 */
cJSON *lace_rpc_value_to_json(const LaceValue *val);

/*
 * Convert a LaceFilter to JSON.
 *
 * @param filter  Filter to convert
 * @return        JSON object (caller must cJSON_Delete), or NULL on error
 */
cJSON *lace_rpc_filter_to_json(const LaceFilter *filter);

/*
 * Convert a LaceSort to JSON.
 *
 * @param sort  Sort to convert
 * @return      JSON object (caller must cJSON_Delete), or NULL on error
 */
cJSON *lace_rpc_sort_to_json(const LaceSort *sort);

#endif /* LIBLACE_RPC_H */
