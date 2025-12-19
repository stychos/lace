/*
 * lace - Database Viewer and Manager
 * MySQL/MariaDB driver - uses libmariadb C API
 */

#include "../db.h"
#include "../connstr.h"
#include "../../util/str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <mysql/mysql.h>

/* MySQL connection data */
typedef struct {
    MYSQL *mysql;
    char *database;
    bool is_mariadb;  /* Connection scheme was mariadb:// */
} MySqlData;

/* Forward declarations */
static DbConnection *mysql_driver_connect(const char *connstr, char **err);
static void mysql_driver_disconnect(DbConnection *conn);
static bool mysql_driver_ping(DbConnection *conn);
static ConnStatus mysql_driver_status(DbConnection *conn);
static const char *mysql_driver_get_error(DbConnection *conn);
static char **mysql_driver_list_tables(DbConnection *conn, size_t *count, char **err);
static TableSchema *mysql_driver_get_table_schema(DbConnection *conn, const char *table, char **err);
static ResultSet *mysql_driver_query(DbConnection *conn, const char *sql, char **err);
static int64_t mysql_driver_exec(DbConnection *conn, const char *sql, char **err);
static ResultSet *mysql_driver_query_page(DbConnection *conn, const char *table,
                                   size_t offset, size_t limit,
                                   const char *order_by, bool desc, char **err);
static bool mysql_driver_update_cell(DbConnection *conn, const char *table,
                              const char *pk_col, const DbValue *pk_val,
                              const char *col, const DbValue *new_val,
                              char **err);
static bool mysql_driver_delete_row(DbConnection *conn, const char *table,
                              const char *pk_col, const DbValue *pk_val,
                              char **err);
static void mysql_driver_free_result(ResultSet *rs);
static void mysql_driver_free_schema(TableSchema *schema);
static void mysql_driver_free_string_list(char **list, size_t count);
static void mysql_driver_library_cleanup(void);

/* Driver definitions - both mysql and mariadb use the same implementation */
DbDriver mysql_driver = {
    .name = "mysql",
    .display_name = "MySQL",
    .connect = mysql_driver_connect,
    .disconnect = mysql_driver_disconnect,
    .ping = mysql_driver_ping,
    .status = mysql_driver_status,
    .get_error = mysql_driver_get_error,
    .list_databases = NULL,
    .list_tables = mysql_driver_list_tables,
    .get_table_schema = mysql_driver_get_table_schema,
    .query = mysql_driver_query,
    .exec = mysql_driver_exec,
    .query_page = mysql_driver_query_page,
    .update_cell = mysql_driver_update_cell,
    .insert_row = NULL,
    .delete_row = mysql_driver_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = mysql_driver_free_result,
    .free_schema = mysql_driver_free_schema,
    .free_string_list = mysql_driver_free_string_list,
    .library_cleanup = mysql_driver_library_cleanup,
};

DbDriver mariadb_driver = {
    .name = "mariadb",
    .display_name = "MariaDB",
    .connect = mysql_driver_connect,
    .disconnect = mysql_driver_disconnect,
    .ping = mysql_driver_ping,
    .status = mysql_driver_status,
    .get_error = mysql_driver_get_error,
    .list_databases = NULL,
    .list_tables = mysql_driver_list_tables,
    .get_table_schema = mysql_driver_get_table_schema,
    .query = mysql_driver_query,
    .exec = mysql_driver_exec,
    .query_page = mysql_driver_query_page,
    .update_cell = mysql_driver_update_cell,
    .insert_row = NULL,
    .delete_row = mysql_driver_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = mysql_driver_free_result,
    .free_schema = mysql_driver_free_schema,
    .free_string_list = mysql_driver_free_string_list,
    .library_cleanup = mysql_driver_library_cleanup,
};

/* Map MySQL field type to DbValueType */
static DbValueType mysql_type_to_db_type(enum enum_field_types type) {
    switch (type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
            return DB_TYPE_INT;

        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return DB_TYPE_FLOAT;

        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            return DB_TYPE_BLOB;

        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
            return DB_TYPE_TIMESTAMP;

        default:
            return DB_TYPE_TEXT;
    }
}

/* Get DbValue from result row */
static DbValue mysql_get_value(MYSQL_ROW row, unsigned long *lengths, int col, MYSQL_FIELD *field) {
    DbValue val = {0};

    if (row[col] == NULL) {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
        return val;
    }

    val.is_null = false;
    DbValueType type = mysql_type_to_db_type(field->type);

    switch (type) {
        case DB_TYPE_INT:
            val.type = DB_TYPE_INT;
            val.int_val = strtoll(row[col], NULL, 10);
            break;

        case DB_TYPE_FLOAT:
            val.type = DB_TYPE_FLOAT;
            val.float_val = strtod(row[col], NULL);
            break;

        case DB_TYPE_BLOB:
            val.type = DB_TYPE_BLOB;
            val.blob.len = lengths[col];
            val.blob.data = malloc(lengths[col]);
            if (val.blob.data) {
                memcpy(val.blob.data, row[col], lengths[col]);
            } else {
                val.blob.len = 0;
            }
            break;

        default:
            val.type = DB_TYPE_TEXT;
            val.text.len = lengths[col];
            val.text.data = malloc(lengths[col] + 1);
            if (val.text.data) {
                memcpy(val.text.data, row[col], lengths[col]);
                val.text.data[lengths[col]] = '\0';
            } else {
                val.text.len = 0;
            }
            break;
    }

    return val;
}

static DbConnection *mysql_driver_connect(const char *connstr, char **err) {
    ConnString *cs = connstr_parse(connstr, err);
    if (!cs) {
        return NULL;
    }

    /* Accept both mysql:// and mariadb:// */
    bool is_mariadb = str_eq(cs->driver, "mariadb");
    if (!str_eq(cs->driver, "mysql") && !is_mariadb) {
        connstr_free(cs);
        if (err) *err = str_dup("Not a MySQL/MariaDB connection string");
        return NULL;
    }

    /* Initialize MySQL library (safe to call multiple times) */
    mysql_library_init(0, NULL, NULL);

    MYSQL *mysql = mysql_init(NULL);
    if (!mysql) {
        connstr_free(cs);
        if (err) *err = str_dup("Failed to initialize MySQL connection");
        return NULL;
    }

    /* Set connection timeout */
    unsigned int timeout = 10;
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    /* Enable automatic reconnection */
    bool reconnect = true;
    mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);

    /* Set character set */
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    const char *host = cs->host ? cs->host : "localhost";
    int port = cs->port > 0 ? cs->port : 3306;
    const char *user = cs->user ? cs->user : "root";
    const char *password = cs->password;
    const char *database = cs->database ? cs->database : "mysql";

    MYSQL *result = mysql_real_connect(mysql, host, user, password,
                                       database, port, NULL, 0);
    if (!result) {
        if (err) *err = str_printf("Connection failed: %s", mysql_error(mysql));
        mysql_close(mysql);
        connstr_free(cs);
        return NULL;
    }

    MySqlData *data = calloc(1, sizeof(MySqlData));
    if (!data) {
        mysql_close(mysql);
        connstr_free(cs);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    data->mysql = mysql;
    data->database = str_dup(database);
    data->is_mariadb = is_mariadb;

    DbConnection *conn = calloc(1, sizeof(DbConnection));
    if (!conn) {
        mysql_close(mysql);
        free(data->database);
        free(data);
        connstr_free(cs);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    conn->driver = is_mariadb ? &mariadb_driver : &mysql_driver;
    conn->connstr = str_dup(connstr);
    conn->database = str_dup(database);
    conn->host = str_dup(host);
    conn->port = port;
    conn->user = str_dup(user);
    conn->status = CONN_STATUS_CONNECTED;
    conn->driver_data = data;

    connstr_free(cs);
    return conn;
}

static void mysql_driver_disconnect(DbConnection *conn) {
    if (!conn) return;

    MySqlData *data = conn->driver_data;
    if (data) {
        if (data->mysql) {
            mysql_close(data->mysql);
        }
        free(data->database);
        free(data);
    }

    free(conn->connstr);
    free(conn->database);
    free(conn->host);
    free(conn->user);
    free(conn->last_error);
    free(conn);
}

static bool mysql_driver_ping(DbConnection *conn) {
    if (!conn) return false;

    MySqlData *data = conn->driver_data;
    if (!data || !data->mysql) return false;

    return mysql_ping(data->mysql) == 0;
}

static ConnStatus mysql_driver_status(DbConnection *conn) {
    return conn ? conn->status : CONN_STATUS_DISCONNECTED;
}

static const char *mysql_driver_get_error(DbConnection *conn) {
    return conn ? conn->last_error : NULL;
}

static int64_t mysql_driver_exec(DbConnection *conn, const char *sql, char **err) {
    if (!conn || !sql) {
        if (err) *err = str_dup("Invalid parameters");
        return -1;
    }

    MySqlData *data = conn->driver_data;
    if (!data || !data->mysql) {
        if (err) *err = str_dup("Not connected");
        return -1;
    }

    if (mysql_query(data->mysql, sql) != 0) {
        if (err) *err = str_dup(mysql_error(data->mysql));
        return -1;
    }

    return (int64_t)mysql_affected_rows(data->mysql);
}

static bool mysql_driver_update_cell(DbConnection *conn, const char *table,
                              const char *pk_col, const DbValue *pk_val,
                              const char *col, const DbValue *new_val,
                              char **err) {
    if (!conn || !table || !pk_col || !pk_val || !col || !new_val) {
        if (err) *err = str_dup("Invalid parameters");
        return false;
    }

    MySqlData *data = conn->driver_data;
    if (!data || !data->mysql) {
        if (err) *err = str_dup("Not connected");
        return false;
    }

    /* Build parameterized UPDATE statement */
    char *sql = str_printf("UPDATE `%s` SET `%s` = ? WHERE `%s` = ?", table, col, pk_col);
    if (!sql) {
        if (err) *err = str_dup("Memory allocation failed");
        return false;
    }

    MYSQL_STMT *stmt = mysql_stmt_init(data->mysql);
    if (!stmt) {
        free(sql);
        if (err) *err = str_dup("Failed to initialize statement");
        return false;
    }

    if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
        if (err) *err = str_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        free(sql);
        return false;
    }
    free(sql);

    /* Bind parameters */
    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));

    /* Parameter 1: new value */
    char *new_str = NULL;
    long long new_int = 0;
    double new_float = 0;
    my_bool new_is_null = new_val->is_null;
    unsigned long new_len = 0;

    if (new_val->is_null) {
        bind[0].buffer_type = MYSQL_TYPE_NULL;
    } else {
        switch (new_val->type) {
            case DB_TYPE_INT:
                bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
                new_int = new_val->int_val;
                bind[0].buffer = &new_int;
                break;
            case DB_TYPE_FLOAT:
                bind[0].buffer_type = MYSQL_TYPE_DOUBLE;
                new_float = new_val->float_val;
                bind[0].buffer = &new_float;
                break;
            case DB_TYPE_BLOB:
                bind[0].buffer_type = MYSQL_TYPE_BLOB;
                bind[0].buffer = (void *)new_val->blob.data;
                new_len = new_val->blob.len;
                bind[0].length = &new_len;
                break;
            default:
                bind[0].buffer_type = MYSQL_TYPE_STRING;
                new_str = new_val->text.data;
                bind[0].buffer = new_str;
                new_len = new_val->text.len;
                bind[0].length = &new_len;
                break;
        }
    }
    bind[0].is_null = &new_is_null;

    /* Parameter 2: primary key value */
    char *pk_str = NULL;
    long long pk_int = 0;
    double pk_float = 0;
    my_bool pk_is_null = pk_val->is_null;
    unsigned long pk_len = 0;

    if (pk_val->is_null) {
        bind[1].buffer_type = MYSQL_TYPE_NULL;
    } else {
        switch (pk_val->type) {
            case DB_TYPE_INT:
                bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
                pk_int = pk_val->int_val;
                bind[1].buffer = &pk_int;
                break;
            case DB_TYPE_FLOAT:
                bind[1].buffer_type = MYSQL_TYPE_DOUBLE;
                pk_float = pk_val->float_val;
                bind[1].buffer = &pk_float;
                break;
            default:
                bind[1].buffer_type = MYSQL_TYPE_STRING;
                pk_str = pk_val->text.data;
                bind[1].buffer = pk_str;
                pk_len = pk_val->text.len;
                bind[1].length = &pk_len;
                break;
        }
    }
    bind[1].is_null = &pk_is_null;

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
        if (err) *err = str_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        if (err) *err = str_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    mysql_stmt_close(stmt);
    return true;
}

static bool mysql_driver_delete_row(DbConnection *conn, const char *table,
                              const char *pk_col, const DbValue *pk_val,
                              char **err) {
    if (!conn || !table || !pk_col || !pk_val) {
        if (err) *err = str_dup("Invalid parameters");
        return false;
    }

    MySqlData *data = conn->driver_data;
    if (!data || !data->mysql) {
        if (err) *err = str_dup("Not connected");
        return false;
    }

    /* Build parameterized DELETE statement */
    char *sql = str_printf("DELETE FROM `%s` WHERE `%s` = ?", table, pk_col);
    if (!sql) {
        if (err) *err = str_dup("Memory allocation failed");
        return false;
    }

    MYSQL_STMT *stmt = mysql_stmt_init(data->mysql);
    if (!stmt) {
        free(sql);
        if (err) *err = str_dup("Failed to initialize statement");
        return false;
    }

    if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
        if (err) *err = str_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        free(sql);
        return false;
    }
    free(sql);

    /* Bind parameter: primary key value */
    MYSQL_BIND bind[1];
    memset(bind, 0, sizeof(bind));

    char *pk_str = NULL;
    long long pk_int = 0;
    double pk_float = 0;
    my_bool pk_is_null = pk_val->is_null;
    unsigned long pk_len = 0;

    if (pk_val->is_null) {
        bind[0].buffer_type = MYSQL_TYPE_NULL;
    } else {
        switch (pk_val->type) {
            case DB_TYPE_INT:
                bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
                pk_int = pk_val->int_val;
                bind[0].buffer = &pk_int;
                break;
            case DB_TYPE_FLOAT:
                bind[0].buffer_type = MYSQL_TYPE_DOUBLE;
                pk_float = pk_val->float_val;
                bind[0].buffer = &pk_float;
                break;
            default:
                bind[0].buffer_type = MYSQL_TYPE_STRING;
                pk_str = pk_val->text.data;
                bind[0].buffer = pk_str;
                pk_len = pk_val->text.len;
                bind[0].length = &pk_len;
                break;
        }
    }
    bind[0].is_null = &pk_is_null;

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
        if (err) *err = str_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        if (err) *err = str_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    mysql_stmt_close(stmt);
    return true;
}

static char **mysql_driver_list_tables(DbConnection *conn, size_t *count, char **err) {
    if (!conn || !count) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    *count = 0;

    MySqlData *data = conn->driver_data;
    if (!data || !data->mysql) {
        if (err) *err = str_dup("Not connected");
        return NULL;
    }

    if (mysql_query(data->mysql, "SHOW TABLES") != 0) {
        if (err) *err = str_dup(mysql_error(data->mysql));
        return NULL;
    }

    MYSQL_RES *result = mysql_store_result(data->mysql);
    if (!result) {
        if (err) *err = str_dup(mysql_error(data->mysql));
        return NULL;
    }

    size_t num_rows = mysql_num_rows(result);
    char **tables = malloc(num_rows * sizeof(char *));
    if (!tables) {
        mysql_free_result(result);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        if (row[0]) {
            tables[*count] = str_dup(row[0]);
            (*count)++;
        }
    }

    mysql_free_result(result);
    return tables;
}

static TableSchema *mysql_driver_get_table_schema(DbConnection *conn, const char *table, char **err) {
    if (!conn || !table) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    MySqlData *data = conn->driver_data;
    if (!data || !data->mysql) {
        if (err) *err = str_dup("Not connected");
        return NULL;
    }

    char *sql = str_printf("DESCRIBE `%s`", table);
    if (!sql) {
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    if (mysql_query(data->mysql, sql) != 0) {
        if (err) *err = str_dup(mysql_error(data->mysql));
        free(sql);
        return NULL;
    }
    free(sql);

    MYSQL_RES *result = mysql_store_result(data->mysql);
    if (!result) {
        if (err) *err = str_dup(mysql_error(data->mysql));
        return NULL;
    }

    TableSchema *schema = calloc(1, sizeof(TableSchema));
    if (!schema) {
        mysql_free_result(result);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    schema->name = str_dup(table);
    size_t num_rows = mysql_num_rows(result);
    schema->columns = calloc(num_rows, sizeof(ColumnDef));
    if (!schema->columns) {
        mysql_free_result(result);
        db_schema_free(schema);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    /* DESCRIBE returns: Field, Type, Null, Key, Default, Extra */
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        ColumnDef *col = &schema->columns[schema->num_columns];
        memset(col, 0, sizeof(ColumnDef));

        col->name = row[0] ? str_dup(row[0]) : NULL;
        col->type_name = row[1] ? str_dup(row[1]) : NULL;
        col->nullable = row[2] && (str_eq_nocase(row[2], "YES"));
        col->primary_key = row[3] && (row[3][0] == 'P' && row[3][1] == 'R' && row[3][2] == 'I');
        col->default_val = (row[4] && row[4][0] && !str_eq(row[4], "NULL"))
                           ? str_dup(row[4]) : NULL;

        /* Map type */
        if (col->type_name) {
            char *type_lower = str_dup(col->type_name);
            if (type_lower) {
                for (char *c = type_lower; *c; c++) *c = tolower((unsigned char)*c);

                if (strstr(type_lower, "int") || strstr(type_lower, "serial"))
                    col->type = DB_TYPE_INT;
                else if (strstr(type_lower, "float") || strstr(type_lower, "double") ||
                         strstr(type_lower, "decimal") || strstr(type_lower, "numeric"))
                    col->type = DB_TYPE_FLOAT;
                else if (strstr(type_lower, "bool") || str_eq(type_lower, "tinyint(1)"))
                    col->type = DB_TYPE_BOOL;
                else if (strstr(type_lower, "blob") || strstr(type_lower, "binary"))
                    col->type = DB_TYPE_BLOB;
                else if (strstr(type_lower, "date") || strstr(type_lower, "time"))
                    col->type = DB_TYPE_TIMESTAMP;
                else
                    col->type = DB_TYPE_TEXT;

                free(type_lower);
            }
        }

        schema->num_columns++;
    }

    mysql_free_result(result);
    return schema;
}

static ResultSet *mysql_driver_query(DbConnection *conn, const char *sql, char **err) {
    if (!conn || !sql) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    MySqlData *data = conn->driver_data;
    if (!data || !data->mysql) {
        if (err) *err = str_dup("Not connected");
        return NULL;
    }

    if (mysql_query(data->mysql, sql) != 0) {
        if (err) *err = str_dup(mysql_error(data->mysql));
        return NULL;
    }

    MYSQL_RES *result = mysql_store_result(data->mysql);
    if (!result) {
        /* Check if this was an INSERT/UPDATE/DELETE (no result expected) */
        if (mysql_field_count(data->mysql) == 0) {
            /* Return empty result set for non-SELECT */
            ResultSet *rs = calloc(1, sizeof(ResultSet));
            return rs;
        }
        if (err) *err = str_dup(mysql_error(data->mysql));
        return NULL;
    }

    ResultSet *rs = calloc(1, sizeof(ResultSet));
    if (!rs) {
        mysql_free_result(result);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    /* Get column info */
    unsigned int num_fields = mysql_num_fields(result);
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    rs->num_columns = num_fields;
    rs->columns = calloc(num_fields, sizeof(ColumnDef));
    if (!rs->columns) {
        mysql_free_result(result);
        free(rs);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    for (unsigned int i = 0; i < num_fields; i++) {
        rs->columns[i].name = str_dup(fields[i].name);
        rs->columns[i].type = mysql_type_to_db_type(fields[i].type);
        rs->columns[i].nullable = !(fields[i].flags & NOT_NULL_FLAG);
        rs->columns[i].primary_key = (fields[i].flags & PRI_KEY_FLAG) != 0;
    }

    /* Get rows */
    size_t num_rows = mysql_num_rows(result);
    if (num_rows > 0) {
        rs->rows = calloc(num_rows, sizeof(Row));
        if (!rs->rows) {
            mysql_free_result(result);
            db_result_free(rs);
            if (err) *err = str_dup("Memory allocation failed");
            return NULL;
        }
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        unsigned long *lengths = mysql_fetch_lengths(result);

        Row *r = &rs->rows[rs->num_rows];
        r->cells = calloc(num_fields, sizeof(DbValue));
        r->num_cells = num_fields;

        if (!r->cells) {
            mysql_free_result(result);
            db_result_free(rs);
            if (err) *err = str_dup("Memory allocation failed");
            return NULL;
        }

        for (unsigned int i = 0; i < num_fields; i++) {
            r->cells[i] = mysql_get_value(row, lengths, i, &fields[i]);
        }

        rs->num_rows++;
    }

    mysql_free_result(result);
    return rs;
}

static ResultSet *mysql_driver_query_page(DbConnection *conn, const char *table,
                                   size_t offset, size_t limit,
                                   const char *order_by, bool desc, char **err) {
    if (!conn || !table) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    /* Build query */
    char *sql;
    if (order_by) {
        sql = str_printf("SELECT * FROM `%s` ORDER BY `%s` %s LIMIT %zu OFFSET %zu",
                         table, order_by, desc ? "DESC" : "ASC", limit, offset);
    } else {
        sql = str_printf("SELECT * FROM `%s` LIMIT %zu OFFSET %zu", table, limit, offset);
    }

    if (!sql) {
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    ResultSet *rs = mysql_driver_query(conn, sql, err);
    free(sql);

    return rs;
}

static void mysql_driver_free_result(ResultSet *rs) {
    db_result_free(rs);
}

static void mysql_driver_free_schema(TableSchema *schema) {
    db_schema_free(schema);
}

static void mysql_driver_free_string_list(char **list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

static void mysql_driver_library_cleanup(void) {
    static bool cleaned_up = false;
    if (cleaned_up) return;
    cleaned_up = true;
    mysql_library_end();
}
