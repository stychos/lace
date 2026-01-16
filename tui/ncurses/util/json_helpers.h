/*
 * Lace
 * JSON parsing helpers
 *
 * Provides safe accessor functions for cJSON objects that return sensible
 * defaults when keys are missing or have wrong types.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_JSON_HELPERS_H
#define LACE_JSON_HELPERS_H

#include "str.h"
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Safe JSON value getters
 * ============================================================================
 * These functions safely extract values from cJSON objects, returning a
 * default value if the key is missing or has the wrong type.
 */

/* Get a string value, returns def if missing/wrong type (does NOT duplicate) */
static inline const char *json_get_string(cJSON *obj, const char *key,
                                          const char *def) {
  if (!obj || !key)
    return def;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  return (item && cJSON_IsString(item)) ? item->valuestring : def;
}

/* Get an integer value, returns def if missing/wrong type */
static inline int json_get_int(cJSON *obj, const char *key, int def) {
  if (!obj || !key)
    return def;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  return (item && cJSON_IsNumber(item)) ? item->valueint : def;
}

/* Get an int64 value, returns def if missing/wrong type */
static inline int64_t json_get_int64(cJSON *obj, const char *key, int64_t def) {
  if (!obj || !key)
    return def;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  if (!item || !cJSON_IsNumber(item))
    return def;
  return (int64_t)item->valuedouble;
}

/* Get a double value, returns def if missing/wrong type */
static inline double json_get_double(cJSON *obj, const char *key, double def) {
  if (!obj || !key)
    return def;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  return (item && cJSON_IsNumber(item)) ? item->valuedouble : def;
}

/* Get a boolean value, returns def if missing/wrong type */
static inline bool json_get_bool(cJSON *obj, const char *key, bool def) {
  if (!obj || !key)
    return def;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  if (!item)
    return def;
  if (cJSON_IsBool(item))
    return cJSON_IsTrue(item);
  return def;
}

/* Get a size_t value with range validation, returns def if invalid */
static inline size_t json_get_size(cJSON *obj, const char *key, size_t def) {
  if (!obj || !key)
    return def;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  if (!item || !cJSON_IsNumber(item))
    return def;
  double val = item->valuedouble;
  if (val < 0 || val > (double)SIZE_MAX)
    return def;
  return (size_t)val;
}

/* ============================================================================
 * String duplication helpers
 * ============================================================================
 * These duplicate the string value - caller owns the result.
 */

/* Get and duplicate a string value, returns NULL if missing/wrong type */
static inline char *json_dup_string(cJSON *obj, const char *key) {
  const char *s = json_get_string(obj, key, NULL);
  return s ? str_dup(s) : NULL;
}

/* Get and duplicate a string value, returns dup of def if missing/wrong type */
static inline char *json_dup_string_or(cJSON *obj, const char *key,
                                       const char *def) {
  const char *s = json_get_string(obj, key, def);
  return s ? str_dup(s) : NULL;
}

/* ============================================================================
 * Array helpers
 * ============================================================================
 */

/* Get array item at index, returns NULL if not an array or out of bounds */
static inline cJSON *json_get_array_item(cJSON *arr, int index) {
  if (!arr || !cJSON_IsArray(arr))
    return NULL;
  return cJSON_GetArrayItem(arr, index);
}

/* Get array size safely, returns 0 if not an array */
static inline int json_array_size(cJSON *arr) {
  if (!arr || !cJSON_IsArray(arr))
    return 0;
  return cJSON_GetArraySize(arr);
}

/* Get object item, returns NULL if not an object */
static inline cJSON *json_get_object(cJSON *obj, const char *key) {
  if (!obj || !key)
    return NULL;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  return (item && cJSON_IsObject(item)) ? item : NULL;
}

/* Get array item from object, returns NULL if not an array */
static inline cJSON *json_get_array(cJSON *obj, const char *key) {
  if (!obj || !key)
    return NULL;
  cJSON *item = cJSON_GetObjectItem(obj, key);
  return (item && cJSON_IsArray(item)) ? item : NULL;
}

/* ============================================================================
 * JSON serialization macros
 * ============================================================================
 * Shorthand for adding values to cJSON objects during serialization.
 */

/* Add a string to object, using empty string if val is NULL */
#define JSON_ADD_STR(obj, key, val)                                            \
  cJSON_AddStringToObject((obj), (key), (val) ? (val) : "")

/* Add a number to object */
#define JSON_ADD_NUM(obj, key, val)                                            \
  cJSON_AddNumberToObject((obj), (key), (double)(val))

/* Add a boolean to object */
#define JSON_ADD_BOOL(obj, key, val) cJSON_AddBoolToObject((obj), (key), (val))

/* Add an integer to object (alias for clarity) */
#define JSON_ADD_INT(obj, key, val)                                            \
  cJSON_AddNumberToObject((obj), (key), (double)(val))

/* ============================================================================
 * JSON deserialization helpers
 * ============================================================================
 */

/* Get integer from JSON and assign to field if within range.
 * Usage: JSON_GET_INT_RANGE(json, "page_size", config->page_size, 10, 1000); */
#define JSON_GET_INT_RANGE(obj, key, field, min_val, max_val)                  \
  do {                                                                         \
    int _v = json_get_int((obj), (key), (field));                              \
    if (_v >= (min_val) && _v <= (max_val))                                    \
      (field) = _v;                                                            \
  } while (0)

/* Get size_t from JSON and assign to field if within range */
#define JSON_GET_SIZE_RANGE(obj, key, field, min_val, max_val)                 \
  do {                                                                         \
    size_t _v = json_get_size((obj), (key), (field));                          \
    if (_v >= (min_val) && _v <= (max_val))                                    \
      (field) = _v;                                                            \
  } while (0)

/* ============================================================================
 * JSON file I/O helpers
 * ============================================================================
 * Functions to load and save JSON from/to files. These handle all the
 * boilerplate: file opening, size checking, memory allocation, parsing, etc.
 */

/* Load JSON from a file.
 * Returns: cJSON object on success, NULL on failure (error set)
 * max_size: Maximum file size in bytes (0 = default 1MB limit)
 * Caller owns the returned cJSON object and must call cJSON_Delete(). */
cJSON *json_load_from_file(const char *path, size_t max_size, char **error);

/* Save JSON to a file.
 * json: The cJSON object to serialize
 * path: File path to write to
 * secure: If true, creates file with 0600 permissions (owner read/write only)
 * Returns: true on success, false on failure (error set) */
bool json_save_to_file(cJSON *json, const char *path, bool secure, char **error);

#endif /* LACE_JSON_HELPERS_H */
