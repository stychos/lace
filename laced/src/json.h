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

#endif /* LACED_JSON_H */
