#include "executor.h"

#include "config.h"
#include "schema.h"
#include "storage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_valid_int(const char *text) {
    char *end_ptr;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    strtol(text, &end_ptr, 10);
    return *end_ptr == '\0';
}

static int is_valid_float(const char *text) {
    char *end_ptr;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    strtod(text, &end_ptr);
    return *end_ptr == '\0';
}

static int is_valid_date(const char *text) {
    int index;

    if (text == NULL || strlen(text) != 10) {
        return 0;
    }

    for (index = 0; index < 10; ++index) {
        if (index == 4 || index == 7) {
            if (text[index] != '-') {
                return 0;
            }
        } else if (!isdigit((unsigned char)text[index])) {
            return 0;
        }
    }

    return 1;
}

static int contains_forbidden_storage_char(const char *text) {
    return strchr(text, '|') != NULL || strchr(text, '\n') != NULL || strchr(text, '\r') != NULL;
}

static int find_primary_key_index(Schema *schema) {
    int index;

    for (index = 0; index < schema->column_count; ++index) {
        if (schema->columns[index].is_primary_key) {
            return index;
        }
    }

    return -1;
}

static int validate_value(ColumnDef *column, const char *value, int is_null) {
    if (is_null) {
        if (!column->nullable || column->is_primary_key) {
            fprintf(stderr,
                    "[ERROR] Executor: column '%s' does not allow NULL\n",
                    column->name);
            return 0;
        }
        return 1;
    }

    if (contains_forbidden_storage_char(value)) {
        fprintf(stderr,
                "[ERROR] Executor: column '%s' contains unsupported storage character\n",
                column->name);
        return 0;
    }

    if (column->type == COL_INT && !is_valid_int(value)) {
        fprintf(stderr,
                "[ERROR] Executor: type mismatch for column '%s' (expected INT)\n",
                column->name);
        return 0;
    }

    if (column->type == COL_FLOAT && !is_valid_float(value)) {
        fprintf(stderr,
                "[ERROR] Executor: type mismatch for column '%s' (expected FLOAT)\n",
                column->name);
        return 0;
    }

    if (column->type == COL_DATE && !is_valid_date(value)) {
        fprintf(stderr,
                "[ERROR] Executor: type mismatch for column '%s' (expected DATE)\n",
                column->name);
        return 0;
    }

    if (column->type == COL_VARCHAR &&
        column->max_length > 0 &&
        (int)strlen(value) > column->max_length) {
        fprintf(stderr,
                "[ERROR] Executor: value too long for column '%s' (max %d)\n",
                column->name,
                column->max_length);
        return 0;
    }

    return 1;
}

static int ensure_primary_key_is_unique(Schema *schema, Row *row) {
    ColumnList star_columns;
    WhereClause where_clause;
    ResultSet *result;
    int pk_index;

    pk_index = find_primary_key_index(schema);
    if (pk_index < 0) {
        return 1;
    }

    memset(&star_columns, 0, sizeof(star_columns));
    star_columns.is_star = 1;

    memset(&where_clause, 0, sizeof(where_clause));
    where_clause.condition_count = 1;
    strncpy(where_clause.conditions[0].column_name,
            schema->columns[pk_index].name,
            sizeof(where_clause.conditions[0].column_name) - 1);
    strncpy(where_clause.conditions[0].operator,
            "=",
            sizeof(where_clause.conditions[0].operator) - 1);
    strncpy(where_clause.conditions[0].value,
            row->data[pk_index],
            sizeof(where_clause.conditions[0].value) - 1);

    result = storage_select(schema->table_name, schema, &star_columns, &where_clause);
    if (result == NULL) {
        return 0;
    }

    if (result->row_count > 0) {
        fprintf(stderr,
                "[ERROR] Executor: duplicate primary key for column '%s'\n",
                schema->columns[pk_index].name);
        free_result_set(result);
        return 0;
    }

    free_result_set(result);
    return 1;
}

static int build_insert_row(Statement *stmt, Schema *schema, Row *row) {
    int provided_indices[MAX_COLUMNS];
    int index;

    memset(provided_indices, -1, sizeof(provided_indices));
    memset(row, 0, sizeof(*row));
    row->column_count = schema->column_count;

    if (stmt->insert_columns.count != stmt->value_count) {
        fprintf(stderr, "[ERROR] Executor: column count does not match value count\n");
        return 0;
    }

    /* 먼저 사용자가 넣은 컬럼들을 스키마 인덱스에 매핑한다. */
    for (index = 0; index < stmt->insert_columns.count; ++index) {
        int schema_index =
            schema_get_column_index(schema, stmt->insert_columns.names[index]);

        if (schema_index < 0) {
            fprintf(stderr,
                    "[ERROR] Executor: unknown column '%s'\n",
                    stmt->insert_columns.names[index]);
            return 0;
        }

        if (provided_indices[schema_index] != -1) {
            fprintf(stderr,
                    "[ERROR] Executor: duplicate column '%s' in INSERT\n",
                    stmt->insert_columns.names[index]);
            return 0;
        }

        if (!validate_value(&schema->columns[schema_index],
                            stmt->values[index],
                            stmt->value_is_null[index])) {
            return 0;
        }

        provided_indices[schema_index] = index;
    }

    /* 저장 직전에는 반드시 스키마 순서로 행을 재구성한다. */
    for (index = 0; index < schema->column_count; ++index) {
        int value_index = provided_indices[index];

        if (value_index == -1) {
            if (!schema->columns[index].nullable) {
                fprintf(stderr,
                        "[ERROR] Executor: missing value for non-nullable column '%s'\n",
                        schema->columns[index].name);
                return 0;
            }
            row->data[index][0] = '\0';
            continue;
        }

        if (stmt->value_is_null[value_index]) {
            row->data[index][0] = '\0';
            continue;
        }

        strncpy(row->data[index], stmt->values[value_index], MAX_TOKEN_LEN - 1);
        row->data[index][MAX_TOKEN_LEN - 1] = '\0';
    }

    return ensure_primary_key_is_unique(schema, row);
}

static void print_separator(const int *widths, int count) {
    int column_index;
    int dash_index;

    for (column_index = 0; column_index < count; ++column_index) {
        putchar('+');
        for (dash_index = 0; dash_index < widths[column_index] + 2; ++dash_index) {
            putchar('-');
        }
    }
    puts("+");
}

static void print_row_values(char values[MAX_COLUMNS][MAX_TOKEN_LEN], const int *widths, int count) {
    int column_index;

    for (column_index = 0; column_index < count; ++column_index) {
        printf("| %-*s ", widths[column_index], values[column_index]);
    }
    puts("|");
}

static void print_result_table(ResultSet *result) {
    int widths[MAX_COLUMNS];
    char headers[MAX_COLUMNS][MAX_TOKEN_LEN];
    int column_index;
    int row_index;

    /* 표 폭은 헤더와 실제 데이터 중 더 긴 쪽에 맞춘다. */
    for (column_index = 0; column_index < result->selected_count; ++column_index) {
        strncpy(headers[column_index],
                result->schema->columns[result->selected_indexes[column_index]].name,
                MAX_TOKEN_LEN - 1);
        headers[column_index][MAX_TOKEN_LEN - 1] = '\0';
        widths[column_index] = (int)strlen(headers[column_index]);
    }

    for (row_index = 0; row_index < result->row_count; ++row_index) {
        for (column_index = 0; column_index < result->selected_count; ++column_index) {
            int value_length = (int)strlen(result->rows[row_index].data[column_index]);
            if (value_length > widths[column_index]) {
                widths[column_index] = value_length;
            }
        }
    }

    print_separator(widths, result->selected_count);
    print_row_values(headers, widths, result->selected_count);
    print_separator(widths, result->selected_count);

    for (row_index = 0; row_index < result->row_count; ++row_index) {
        print_row_values(result->rows[row_index].data, widths, result->selected_count);
    }

    print_separator(widths, result->selected_count);
}

int execute_insert(Statement *stmt) {
    Schema *schema;
    Row row;
    int status;

    schema = schema_load(stmt->table_name);
    if (schema == NULL) {
        return -1;
    }

    if (!build_insert_row(stmt, schema, &row)) {
        schema_free(schema);
        return -1;
    }

    status = storage_insert(stmt->table_name, &row, schema);
    schema_free(schema);

    if (status != 0) {
        return -1;
    }

    puts("1 row inserted.");
    return 0;
}

int execute_select(Statement *stmt) {
    Schema *schema;
    ResultSet *result;
    int index;

    schema = schema_load(stmt->table_name);
    if (schema == NULL) {
        return -1;
    }

    if (!stmt->select_columns.is_star) {
        for (index = 0; index < stmt->select_columns.count; ++index) {
            if (schema_get_column_index(schema, stmt->select_columns.names[index]) < 0) {
                fprintf(stderr,
                        "[ERROR] Executor: unknown column '%s'\n",
                        stmt->select_columns.names[index]);
                schema_free(schema);
                return -1;
            }
        }
    }

    result = storage_select(stmt->table_name, schema, &stmt->select_columns, &stmt->where);
    if (result == NULL) {
        schema_free(schema);
        return -1;
    }

    print_result_table(result);
    printf("%d row(s) selected.\n", result->row_count);

    free_result_set(result);
    schema_free(schema);
    return 0;
}

int execute(Statement *stmt) {
    if (stmt == NULL) {
        fprintf(stderr, "[ERROR] Executor: statement is NULL\n");
        return -1;
    }

    if (stmt->type == STMT_INSERT) {
        return execute_insert(stmt);
    }

    if (stmt->type == STMT_SELECT) {
        return execute_select(stmt);
    }

    fprintf(stderr, "[ERROR] Executor: unsupported statement type\n");
    return -1;
}
