/*
 * lace - Database Viewer and Manager
 * PostgreSQL driver - uses libpq C API
 */

#include "../db.h"
#include "../connstr.h"
#include "../../util/str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <libpq-fe.h>

/* PostgreSQL connection data */
typedef struct {
    PGconn *conn;
    char *database;
} PgData;

/* Forward declarations */
static DbConnection *pg_connect(const char *connstr, char **err);
static void pg_disconnect(DbConnection *conn);
static bool pg_ping(DbConnection *conn);
static ConnStatus pg_status(DbConnection *conn);
static const char *pg_get_error(DbConnection *conn);
static char **pg_list_tables(DbConnection *conn, size_t *count, char **err);
static TableSchema *pg_get_table_schema(DbConnection *conn, const char *table, char **err);
static ResultSet *pg_query(DbConnection *conn, const char *sql, char **err);
static int64_t pg_exec(DbConnection *conn, const char *sql, char **err);
static ResultSet *pg_query_page(DbConnection *conn, const char *table,
                                size_t offset, size_t limit,
                                const char *order_by, bool desc, char **err);
static bool pg_update_cell(DbConnection *conn, const char *table,
                           const char *pk_col, const DbValue *pk_val,
                           const char *col, const DbValue *new_val,
                           char **err);
static bool pg_delete_row(DbConnection *conn, const char *table,
                          const char *pk_col, const DbValue *pk_val,
                          char **err);
static void pg_free_result(ResultSet *rs);
static void pg_free_schema(TableSchema *schema);
static void pg_free_string_list(char **list, size_t count);

/* Driver definition */
DbDriver postgres_driver = {
    .name = "postgres",
    .display_name = "PostgreSQL",
    .connect = pg_connect,
    .disconnect = pg_disconnect,
    .ping = pg_ping,
    .status = pg_status,
    .get_error = pg_get_error,
    .list_databases = NULL,
    .list_tables = pg_list_tables,
    .get_table_schema = pg_get_table_schema,
    .query = pg_query,
    .exec = pg_exec,
    .query_page = pg_query_page,
    .update_cell = pg_update_cell,
    .insert_row = NULL,
    .delete_row = pg_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = pg_free_result,
    .free_schema = pg_free_schema,
    .free_string_list = pg_free_string_list,
};

/* Map PostgreSQL OID to DbValueType */
static DbValueType pg_oid_to_db_type(Oid oid) {
    switch (oid) {
        case 20:   /* int8 */
        case 21:   /* int2 */
        case 23:   /* int4 */
        case 26:   /* oid */
            return DB_TYPE_INT;

        case 700:  /* float4 */
        case 701:  /* float8 */
        case 1700: /* numeric */
            return DB_TYPE_FLOAT;

        case 16:   /* bool */
            return DB_TYPE_BOOL;

        case 17:   /* bytea */
            return DB_TYPE_BLOB;

        case 1082: /* date */
        case 1083: /* time */
        case 1114: /* timestamp */
        case 1184: /* timestamptz */
            return DB_TYPE_TIMESTAMP;

        default:
            return DB_TYPE_TEXT;
    }
}

/* Get DbValue from PGresult */
static DbValue pg_get_value(PGresult *res, int row, int col, Oid oid) {
    DbValue val = {0};

    if (PQgetisnull(res, row, col)) {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
        return val;
    }

    val.is_null = false;
    char *value = PQgetvalue(res, row, col);
    int len = PQgetlength(res, row, col);
    DbValueType type = pg_oid_to_db_type(oid);

    switch (type) {
        case DB_TYPE_INT:
            val.type = DB_TYPE_INT;
            val.int_val = strtoll(value, NULL, 10);
            break;

        case DB_TYPE_FLOAT:
            val.type = DB_TYPE_FLOAT;
            val.float_val = strtod(value, NULL);
            break;

        case DB_TYPE_BOOL:
            val.type = DB_TYPE_BOOL;
            val.bool_val = (value[0] == 't' || value[0] == 'T' || value[0] == '1');
            break;

        case DB_TYPE_BLOB:
            val.type = DB_TYPE_BLOB;
            /* PostgreSQL bytea in text format needs unescaping */
            if (len >= 2 && value[0] == '\\' && value[1] == 'x') {
                /* Hex format: \xDEADBEEF */
                size_t hex_len = (len - 2) / 2;
                val.blob.data = malloc(hex_len);
                if (val.blob.data) {
                    val.blob.len = hex_len;
                    for (size_t i = 0; i < hex_len; i++) {
                        unsigned int byte;
                        sscanf(value + 2 + i * 2, "%2x", &byte);
                        val.blob.data[i] = (uint8_t)byte;
                    }
                }
            } else {
                /* Escape format or raw */
                val.blob.data = malloc(len);
                if (val.blob.data) {
                    memcpy(val.blob.data, value, len);
                    val.blob.len = len;
                }
            }
            break;

        default:
            val.type = DB_TYPE_TEXT;
            val.text.len = len;
            val.text.data = malloc(len + 1);
            if (val.text.data) {
                memcpy(val.text.data, value, len);
                val.text.data[len] = '\0';
            } else {
                val.text.len = 0;
            }
            break;
    }

    return val;
}

static DbConnection *pg_connect(const char *connstr, char **err) {
    ConnString *cs = connstr_parse(connstr, err);
    if (!cs) {
        return NULL;
    }

    if (!str_eq(cs->driver, "postgres") && !str_eq(cs->driver, "postgresql")) {
        connstr_free(cs);
        if (err) *err = str_dup("Not a PostgreSQL connection string");
        return NULL;
    }

    /* Build libpq connection string */
    const char *host = cs->host ? cs->host : "localhost";
    int port = cs->port > 0 ? cs->port : 5432;
    const char *user = cs->user ? cs->user : "postgres";
    const char *password = cs->password;
    const char *database = cs->database ? cs->database : "postgres";

    char *pq_connstr;
    if (password) {
        pq_connstr = str_printf("host=%s port=%d user=%s password=%s dbname=%s",
                                host, port, user, password, database);
    } else {
        pq_connstr = str_printf("host=%s port=%d user=%s dbname=%s",
                                host, port, user, database);
    }

    if (!pq_connstr) {
        connstr_free(cs);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    PGconn *pgconn = PQconnectdb(pq_connstr);
    free(pq_connstr);

    if (PQstatus(pgconn) != CONNECTION_OK) {
        if (err) *err = str_printf("Connection failed: %s", PQerrorMessage(pgconn));
        PQfinish(pgconn);
        connstr_free(cs);
        return NULL;
    }

    /* Set client encoding to UTF-8 */
    PQsetClientEncoding(pgconn, "UTF8");

    PgData *data = calloc(1, sizeof(PgData));
    if (!data) {
        PQfinish(pgconn);
        connstr_free(cs);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    data->conn = pgconn;
    data->database = str_dup(database);

    DbConnection *conn = calloc(1, sizeof(DbConnection));
    if (!conn) {
        PQfinish(pgconn);
        free(data->database);
        free(data);
        connstr_free(cs);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    conn->driver = &postgres_driver;
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

static void pg_disconnect(DbConnection *conn) {
    if (!conn) return;

    PgData *data = conn->driver_data;
    if (data) {
        if (data->conn) {
            PQfinish(data->conn);
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

static bool pg_ping(DbConnection *conn) {
    if (!conn) return false;

    PgData *data = conn->driver_data;
    if (!data || !data->conn) return false;

    /* Check connection status */
    if (PQstatus(data->conn) != CONNECTION_OK) {
        /* Try to reset */
        PQreset(data->conn);
        return PQstatus(data->conn) == CONNECTION_OK;
    }

    return true;
}

static ConnStatus pg_status(DbConnection *conn) {
    return conn ? conn->status : CONN_STATUS_DISCONNECTED;
}

static const char *pg_get_error(DbConnection *conn) {
    return conn ? conn->last_error : NULL;
}

static int64_t pg_exec(DbConnection *conn, const char *sql, char **err) {
    if (!conn || !sql) {
        if (err) *err = str_dup("Invalid parameters");
        return -1;
    }

    PgData *data = conn->driver_data;
    if (!data || !data->conn) {
        if (err) *err = str_dup("Not connected");
        return -1;
    }

    PGresult *res = PQexec(data->conn, sql);
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        if (err) *err = str_dup(PQerrorMessage(data->conn));
        PQclear(res);
        return -1;
    }

    char *affected = PQcmdTuples(res);
    int64_t count = affected && *affected ? strtoll(affected, NULL, 10) : 0;
    PQclear(res);
    return count;
}

static bool pg_update_cell(DbConnection *conn, const char *table,
                           const char *pk_col, const DbValue *pk_val,
                           const char *col, const DbValue *new_val,
                           char **err) {
    if (!conn || !table || !pk_col || !pk_val || !col || !new_val) {
        if (err) *err = str_dup("Invalid parameters");
        return false;
    }

    PgData *data = conn->driver_data;
    if (!data || !data->conn) {
        if (err) *err = str_dup("Not connected");
        return false;
    }

    /* Build parameterized UPDATE statement */
    char *sql = str_printf("UPDATE \"%s\" SET \"%s\" = $1 WHERE \"%s\" = $2", table, col, pk_col);
    if (!sql) {
        if (err) *err = str_dup("Memory allocation failed");
        return false;
    }

    /* Prepare parameter values */
    const char *paramValues[2];
    int paramLengths[2];
    int paramFormats[2] = {0, 0};  /* Text format */
    char new_buf[64], pk_buf[64];
    char *new_str = NULL, *pk_str = NULL;

    /* Parameter 1: new value */
    if (new_val->is_null) {
        paramValues[0] = NULL;
        paramLengths[0] = 0;
    } else {
        switch (new_val->type) {
            case DB_TYPE_INT:
                snprintf(new_buf, sizeof(new_buf), "%lld", (long long)new_val->int_val);
                paramValues[0] = new_buf;
                paramLengths[0] = strlen(new_buf);
                break;
            case DB_TYPE_FLOAT:
                snprintf(new_buf, sizeof(new_buf), "%g", new_val->float_val);
                paramValues[0] = new_buf;
                paramLengths[0] = strlen(new_buf);
                break;
            case DB_TYPE_BOOL:
                paramValues[0] = new_val->bool_val ? "t" : "f";
                paramLengths[0] = 1;
                break;
            case DB_TYPE_BLOB:
                /* For blob, we'd need binary format - skip for now */
                new_str = str_dup("\\x");
                paramValues[0] = new_str;
                paramLengths[0] = strlen(new_str);
                break;
            default:
                paramValues[0] = new_val->text.data;
                paramLengths[0] = new_val->text.len;
                break;
        }
    }

    /* Parameter 2: primary key value */
    if (pk_val->is_null) {
        paramValues[1] = NULL;
        paramLengths[1] = 0;
    } else {
        switch (pk_val->type) {
            case DB_TYPE_INT:
                snprintf(pk_buf, sizeof(pk_buf), "%lld", (long long)pk_val->int_val);
                paramValues[1] = pk_buf;
                paramLengths[1] = strlen(pk_buf);
                break;
            case DB_TYPE_FLOAT:
                snprintf(pk_buf, sizeof(pk_buf), "%g", pk_val->float_val);
                paramValues[1] = pk_buf;
                paramLengths[1] = strlen(pk_buf);
                break;
            default:
                paramValues[1] = pk_val->text.data;
                paramLengths[1] = pk_val->text.len;
                break;
        }
    }

    PGresult *res = PQexecParams(data->conn, sql, 2, NULL,
                                  paramValues, paramLengths, paramFormats, 0);
    free(sql);
    free(new_str);
    free(pk_str);

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK) {
        if (err) *err = str_dup(PQerrorMessage(data->conn));
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}

static bool pg_delete_row(DbConnection *conn, const char *table,
                          const char *pk_col, const DbValue *pk_val,
                          char **err) {
    if (!conn || !table || !pk_col || !pk_val) {
        if (err) *err = str_dup("Invalid parameters");
        return false;
    }

    PgData *data = conn->driver_data;
    if (!data || !data->conn) {
        if (err) *err = str_dup("Not connected");
        return false;
    }

    /* Build parameterized DELETE statement */
    char *sql = str_printf("DELETE FROM \"%s\" WHERE \"%s\" = $1", table, pk_col);
    if (!sql) {
        if (err) *err = str_dup("Memory allocation failed");
        return false;
    }

    /* Prepare parameter value */
    const char *paramValues[1];
    int paramLengths[1];
    int paramFormats[1] = {0};  /* Text format */
    char pk_buf[64];

    /* Parameter 1: primary key value */
    if (pk_val->is_null) {
        paramValues[0] = NULL;
        paramLengths[0] = 0;
    } else {
        switch (pk_val->type) {
            case DB_TYPE_INT:
                snprintf(pk_buf, sizeof(pk_buf), "%lld", (long long)pk_val->int_val);
                paramValues[0] = pk_buf;
                paramLengths[0] = strlen(pk_buf);
                break;
            case DB_TYPE_FLOAT:
                snprintf(pk_buf, sizeof(pk_buf), "%g", pk_val->float_val);
                paramValues[0] = pk_buf;
                paramLengths[0] = strlen(pk_buf);
                break;
            default:
                paramValues[0] = pk_val->text.data;
                paramLengths[0] = pk_val->text.len;
                break;
        }
    }

    PGresult *res = PQexecParams(data->conn, sql, 1, NULL,
                                  paramValues, paramLengths, paramFormats, 0);
    free(sql);

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK) {
        if (err) *err = str_dup(PQerrorMessage(data->conn));
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}

static char **pg_list_tables(DbConnection *conn, size_t *count, char **err) {
    if (!conn || !count) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    *count = 0;

    PgData *data = conn->driver_data;
    if (!data || !data->conn) {
        if (err) *err = str_dup("Not connected");
        return NULL;
    }

    const char *sql = "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename";
    PGresult *res = PQexec(data->conn, sql);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (err) *err = str_dup(PQerrorMessage(data->conn));
        PQclear(res);
        return NULL;
    }

    int num_rows = PQntuples(res);
    char **tables = malloc(num_rows * sizeof(char *));
    if (!tables) {
        PQclear(res);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    for (int i = 0; i < num_rows; i++) {
        tables[i] = str_dup(PQgetvalue(res, i, 0));
        (*count)++;
    }

    PQclear(res);
    return tables;
}

static TableSchema *pg_get_table_schema(DbConnection *conn, const char *table, char **err) {
    if (!conn || !table) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    PgData *data = conn->driver_data;
    if (!data || !data->conn) {
        if (err) *err = str_dup("Not connected");
        return NULL;
    }

    /* Query column information */
    const char *sql = "SELECT column_name, data_type, is_nullable, column_default "
                      "FROM information_schema.columns "
                      "WHERE table_schema = 'public' AND table_name = $1 "
                      "ORDER BY ordinal_position";

    const char *paramValues[1] = {table};
    PGresult *res = PQexecParams(data->conn, sql, 1, NULL, paramValues, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (err) *err = str_dup(PQerrorMessage(data->conn));
        PQclear(res);
        return NULL;
    }

    TableSchema *schema = calloc(1, sizeof(TableSchema));
    if (!schema) {
        PQclear(res);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    schema->name = str_dup(table);
    int num_rows = PQntuples(res);
    schema->columns = calloc(num_rows, sizeof(ColumnDef));
    if (!schema->columns) {
        PQclear(res);
        db_schema_free(schema);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    for (int i = 0; i < num_rows; i++) {
        ColumnDef *col = &schema->columns[schema->num_columns];
        memset(col, 0, sizeof(ColumnDef));

        col->name = str_dup(PQgetvalue(res, i, 0));
        col->type_name = str_dup(PQgetvalue(res, i, 1));
        col->nullable = str_eq_nocase(PQgetvalue(res, i, 2), "YES");

        char *default_val = PQgetvalue(res, i, 3);
        col->default_val = (default_val && *default_val) ? str_dup(default_val) : NULL;

        /* Map type */
        char *type = col->type_name;
        if (strstr(type, "int") || strstr(type, "serial"))
            col->type = DB_TYPE_INT;
        else if (strstr(type, "float") || strstr(type, "double") ||
                 strstr(type, "numeric") || strstr(type, "decimal"))
            col->type = DB_TYPE_FLOAT;
        else if (strstr(type, "bool"))
            col->type = DB_TYPE_BOOL;
        else if (strstr(type, "bytea"))
            col->type = DB_TYPE_BLOB;
        else if (strstr(type, "timestamp") || strstr(type, "date") || strstr(type, "time"))
            col->type = DB_TYPE_TIMESTAMP;
        else
            col->type = DB_TYPE_TEXT;

        schema->num_columns++;
    }

    PQclear(res);

    /* Get primary key info */
    sql = "SELECT a.attname FROM pg_index i "
          "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey) "
          "WHERE i.indrelid = $1::regclass AND i.indisprimary";

    paramValues[0] = table;
    res = PQexecParams(data->conn, sql, 1, NULL, paramValues, NULL, NULL, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int pk_rows = PQntuples(res);
        for (int i = 0; i < pk_rows; i++) {
            char *pk_col = PQgetvalue(res, i, 0);
            for (size_t j = 0; j < schema->num_columns; j++) {
                if (str_eq(schema->columns[j].name, pk_col)) {
                    schema->columns[j].primary_key = true;
                    break;
                }
            }
        }
    }

    PQclear(res);
    return schema;
}

static ResultSet *pg_query(DbConnection *conn, const char *sql, char **err) {
    if (!conn || !sql) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    PgData *data = conn->driver_data;
    if (!data || !data->conn) {
        if (err) *err = str_dup("Not connected");
        return NULL;
    }

    PGresult *res = PQexec(data->conn, sql);
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        if (err) *err = str_dup(PQerrorMessage(data->conn));
        PQclear(res);
        return NULL;
    }

    ResultSet *rs = calloc(1, sizeof(ResultSet));
    if (!rs) {
        PQclear(res);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    if (status == PGRES_COMMAND_OK) {
        /* Non-SELECT statement */
        PQclear(res);
        return rs;
    }

    /* Get column info */
    int num_fields = PQnfields(res);
    rs->num_columns = num_fields;
    rs->columns = calloc(num_fields, sizeof(ColumnDef));
    if (!rs->columns) {
        PQclear(res);
        free(rs);
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    for (int i = 0; i < num_fields; i++) {
        rs->columns[i].name = str_dup(PQfname(res, i));
        rs->columns[i].type = pg_oid_to_db_type(PQftype(res, i));
    }

    /* Get rows */
    int num_rows = PQntuples(res);
    if (num_rows > 0) {
        rs->rows = calloc(num_rows, sizeof(Row));
        if (!rs->rows) {
            PQclear(res);
            db_result_free(rs);
            if (err) *err = str_dup("Memory allocation failed");
            return NULL;
        }
    }

    for (int r = 0; r < num_rows; r++) {
        Row *row = &rs->rows[rs->num_rows];
        row->cells = calloc(num_fields, sizeof(DbValue));
        row->num_cells = num_fields;

        if (!row->cells) {
            PQclear(res);
            db_result_free(rs);
            if (err) *err = str_dup("Memory allocation failed");
            return NULL;
        }

        for (int c = 0; c < num_fields; c++) {
            row->cells[c] = pg_get_value(res, r, c, PQftype(res, c));
        }

        rs->num_rows++;
    }

    PQclear(res);
    return rs;
}

static ResultSet *pg_query_page(DbConnection *conn, const char *table,
                                size_t offset, size_t limit,
                                const char *order_by, bool desc, char **err) {
    if (!conn || !table) {
        if (err) *err = str_dup("Invalid parameters");
        return NULL;
    }

    /* Build query */
    char *sql;
    if (order_by) {
        sql = str_printf("SELECT * FROM \"%s\" ORDER BY \"%s\" %s LIMIT %zu OFFSET %zu",
                         table, order_by, desc ? "DESC" : "ASC", limit, offset);
    } else {
        sql = str_printf("SELECT * FROM \"%s\" LIMIT %zu OFFSET %zu", table, limit, offset);
    }

    if (!sql) {
        if (err) *err = str_dup("Memory allocation failed");
        return NULL;
    }

    ResultSet *rs = pg_query(conn, sql, err);
    free(sql);

    return rs;
}

static void pg_free_result(ResultSet *rs) {
    db_result_free(rs);
}

static void pg_free_schema(TableSchema *schema) {
    db_schema_free(schema);
}

static void pg_free_string_list(char **list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}
