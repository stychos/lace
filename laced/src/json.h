/*
 * laced - Lace Database Daemon
 * JSON serialization helpers
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_JSON_H
#define LACED_JSON_H

#include "db/db.h"
#include "db/db_types.h"
#include <cjson/cJSON.h>
#include <stdbool.h>

/* ==========================================================================
 * Result Set Serialization
 * ========================================================================== */

/*
 * Serialize a ResultSet to JSON.
 *
 * @param rs  Result set to serialize
 * @return    JSON object (caller must cJSON_Delete), or NULL on failure
 */
cJSON *laced_json_from_result(const ResultSet *rs);

/*
 * Serialize a TableSchema to JSON.
 *
 * @param schema  Schema to serialize
 * @return        JSON object (caller must cJSON_Delete), or NULL on failure
 */
cJSON *laced_json_from_schema(const TableSchema *schema);

/* ==========================================================================
 * Value Serialization
 * ========================================================================== */

/*
 * Serialize a DbValue to JSON.
 *
 * @param val  Value to serialize
 * @return     JSON value (caller must cJSON_Delete), or NULL on failure
 */
cJSON *laced_json_from_value(const DbValue *val);

/*
 * Deserialize a DbValue from JSON.
 *
 * @param json  JSON value
 * @param val   Output: deserialized value
 * @return      true on success, false on failure
 */
bool laced_json_to_value(cJSON *json, DbValue *val);

/* ==========================================================================
 * Parameter Extraction
 * ========================================================================== */

/*
 * Get a string parameter from JSON object.
 *
 * @param params   JSON object
 * @param name     Parameter name
 * @param out      Output: string value (NOT duplicated, points into JSON)
 * @return         true if found and is string, false otherwise
 */
bool laced_json_get_string(cJSON *params, const char *name, const char **out);

/*
 * Get an integer parameter from JSON object.
 *
 * @param params   JSON object
 * @param name     Parameter name
 * @param out      Output: integer value
 * @return         true if found and is number, false otherwise
 */
bool laced_json_get_int(cJSON *params, const char *name, int *out);

/*
 * Get a size_t parameter from JSON object.
 *
 * @param params   JSON object
 * @param name     Parameter name
 * @param out      Output: size_t value
 * @return         true if found and is non-negative number, false otherwise
 */
bool laced_json_get_size(cJSON *params, const char *name, size_t *out);

/*
 * Get a boolean parameter from JSON object.
 *
 * @param params   JSON object
 * @param name     Parameter name
 * @param out      Output: boolean value
 * @return         true if found and is boolean, false otherwise
 */
bool laced_json_get_bool(cJSON *params, const char *name, bool *out);

#endif /* LACED_JSON_H */
