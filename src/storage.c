#include "storage.h"

#include "config.h"
#include "schema.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_int_value(const char *text, long *out_value) {
    char *end_ptr;
    long value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    errno = 0;
    value = strtol(text, &end_ptr, 10);
    if (errno != 0 || *end_ptr != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return 0;
    }

    *out_value = value;
    return 1;
}

static int parse_float_value(const char *text, double *out_value) {
    char *end_ptr;
    double value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    errno = 0;
    value = strtod(text, &end_ptr);
    if (errno != 0 || *end_ptr != '\0' || !isfinite(value)) {
        return 0;
    }

    *out_value = value;
    return 1;
}

static int is_valid_storage_date(const char *text) {
    static const int DAYS_IN_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int index;
    int year;
    int month;
    int day;
    int max_day;

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

    year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
    month = (text[5] - '0') * 10 + (text[6] - '0');
    day = (text[8] - '0') * 10 + (text[9] - '0');

    if (month < 1 || month > 12) {
        return 0;
    }

    max_day = DAYS_IN_MONTH[month - 1];
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
        if (month == 2) {
            max_day = 29;
        }
    }

    return day >= 1 && day <= max_day;
}

static int utf8_char_count(const char *text, int *out_count) {
    int count = 0;
    int index = 0;

    if (text == NULL || out_count == NULL) {
        return 0;
    }

    while (text[index] != '\0') {
        unsigned char lead = (unsigned char)text[index];
        int width = 0;
        int offset;

        if ((lead & 0x80u) == 0) {
            width = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            if (lead < 0xC2u) {
                return 0;
            }
            width = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            width = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            if (lead > 0xF4u) {
                return 0;
            }
            width = 4;
        } else {
            return 0;
        }

        for (offset = 1; offset < width; ++offset) {
            unsigned char continuation = (unsigned char)text[index + offset];
            if (continuation == '\0' || (continuation & 0xC0u) != 0x80u) {
                return 0;
            }
        }

        index += width;
        count++;
    }

    *out_count = count;
    return 1;
}

static void strip_newline(char *line) {
    size_t length = strlen(line);

    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[length - 1] = '\0';
        length--;
    }
}

static int line_was_truncated(const char *line, size_t buffer_size) {
    size_t length = strlen(line);

    if (length == 0) {
        return 0;
    }

    if (line[length - 1] == '\n' || line[length - 1] == '\r') {
        return 0;
    }

    return length == buffer_size - 1;
}

static int count_pipe_fields(const char *line) {
    int field_count = 1;
    int index = 0;

    if (line == NULL || line[0] == '\0') {
        return 1;
    }

    while (line[index] != '\0') {
        if (line[index] == '|') {
            field_count++;
        }
        index++;
    }

    return field_count;
}

static int decode_storage_field(char *field, int *is_null) {
    char decoded[MAX_TOKEN_LEN];
    int source_index = 0;
    int target_index = 0;

    if (strcmp(field, NULL_STORAGE_TOKEN) == 0) {
        field[0] = '\0';
        *is_null = 1;
        return 1;
    }

    while (field[source_index] != '\0') {
        if (target_index >= MAX_TOKEN_LEN - 1) {
            return 0;
        }

        if (field[source_index] == '\\') {
            if (field[source_index + 1] != '\\') {
                return 0;
            }
            decoded[target_index++] = '\\';
            source_index += 2;
            continue;
        }

        decoded[target_index++] = field[source_index++];
    }

    decoded[target_index] = '\0';
    strcpy(field, decoded);
    *is_null = 0;
    return 1;
}

static int encode_storage_field(const char *value, char *encoded, size_t size) {
    size_t source_index = 0;
    size_t target_index = 0;

    while (value[source_index] != '\0') {
        if (value[source_index] == '\\') {
            if (target_index + 2 >= size) {
                return 0;
            }
            encoded[target_index++] = '\\';
            encoded[target_index++] = '\\';
        } else {
            if (target_index + 1 >= size) {
                return 0;
            }
            encoded[target_index++] = value[source_index];
        }
        source_index++;
    }

    encoded[target_index] = '\0';
    return 1;
}

static int validate_loaded_row(Row *row, Schema *schema) {
    int index;

    for (index = 0; index < schema->column_count; ++index) {
        ColumnDef *column = &schema->columns[index];

        if (row->is_null[index]) {
            if (!column->nullable || column->is_primary_key) {
                return 0;
            }
            continue;
        }

        if (column->type == COL_INT) {
            long value;
            if (!parse_int_value(row->data[index], &value)) {
                return 0;
            }
        } else if (column->type == COL_FLOAT) {
            double value;
            if (!parse_float_value(row->data[index], &value)) {
                return 0;
            }
        } else if (column->type == COL_DATE) {
            if (!is_valid_storage_date(row->data[index])) {
                return 0;
            }
        } else if (column->type == COL_VARCHAR) {
            int char_count = 0;

            if (!utf8_char_count(row->data[index], &char_count)) {
                return 0;
            }
            if (column->max_length > 0 && char_count > column->max_length) {
                return 0;
            }
        }
    }

    return 1;
}

static int find_primary_key_index(Schema *schema) {
    int index;

    if (schema == NULL) {
        return -1;
    }

    for (index = 0; index < schema->column_count; ++index) {
        if (schema->columns[index].is_primary_key) {
            return index;
        }
    }

    return -1;
}

static void free_seen_primary_keys(char **values, size_t count) {
    size_t index;

    if (values == NULL) {
        return;
    }

    for (index = 0; index < count; ++index) {
        free(values[index]);
    }
    free(values);
}

static int append_seen_primary_key(char ***values,
                                   size_t *count,
                                   size_t *capacity,
                                   const char *value) {
    char **resized_values;
    char *copy;

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 16u : (*capacity * 2u);

        resized_values = (char **)realloc(*values, new_capacity * sizeof(char *));
        if (resized_values == NULL) {
            return 0;
        }

        *values = resized_values;
        *capacity = new_capacity;
    }

    copy = (char *)malloc(strlen(value) + 1);
    if (copy == NULL) {
        return 0;
    }

    strcpy(copy, value);
    (*values)[*count] = copy;
    (*count)++;
    return 1;
}

static int has_seen_primary_key(char **values, size_t count, const char *value) {
    size_t index;

    for (index = 0; index < count; ++index) {
        if (strcmp(values[index], value) == 0) {
            return 1;
        }
    }

    return 0;
}

/* 파이프 구분 포맷을 그대로 분해한다. */
static int split_pipe_line(const char *line,
                           char fields[MAX_COLUMNS][MAX_TOKEN_LEN],
                           int null_flags[MAX_COLUMNS],
                           int expected_fields) {
    int field_index = 0;
    int value_length = 0;
    int cursor = 0;
    int overflow = 0;

    if (count_pipe_fields(line) != expected_fields) {
        return 0;
    }

    for (field_index = 0; field_index < expected_fields; ++field_index) {
        fields[field_index][0] = '\0';
        null_flags[field_index] = 0;
    }

    field_index = 0;
    while (line[cursor] != '\0' && field_index < expected_fields) {
        if (line[cursor] == '|') {
            fields[field_index][value_length] = '\0';
            field_index++;
            value_length = 0;
            cursor++;
            continue;
        }

        if (value_length < MAX_TOKEN_LEN - 1) {
            fields[field_index][value_length++] = line[cursor];
        } else {
            overflow = 1;
        }
        cursor++;
    }

    if (field_index < expected_fields) {
        fields[field_index][value_length] = '\0';
    }

    if (overflow) {
        return 0;
    }

    for (field_index = 0; field_index < expected_fields; ++field_index) {
        if (!decode_storage_field(fields[field_index], &null_flags[field_index])) {
            return 0;
        }
    }

    return 1;
}

static int validate_header_line(const char *line, Schema *schema) {
    char fields[MAX_COLUMNS][MAX_TOKEN_LEN];
    int null_flags[MAX_COLUMNS];
    int index;

    if (!split_pipe_line(line, fields, null_flags, schema->column_count)) {
        return 0;
    }

    for (index = 0; index < schema->column_count; ++index) {
        if (null_flags[index] || strcmp(fields[index], schema->columns[index].name) != 0) {
            return 0;
        }
    }

    return 1;
}

int evaluate_condition(Row *row, Schema *schema, Condition *cond) {
    int column_index;
    const char *actual;

    if (row == NULL || schema == NULL || cond == NULL) {
        return 0;
    }

    column_index = schema_get_column_index(schema, cond->column_name);
    if (column_index < 0 || column_index >= row->column_count) {
        return 0;
    }

    actual = row->data[column_index];

    if (cond->value_is_null) {
        if (strcmp(cond->operator, "=") == 0) {
            return row->is_null[column_index];
        }
        if (strcmp(cond->operator, "!=") == 0 || strcmp(cond->operator, "<>") == 0) {
            return !row->is_null[column_index];
        }
        return 0;
    }

    if (row->is_null[column_index]) {
        return 0;
    }

    if (schema->columns[column_index].type == COL_INT) {
        long left;
        long right;

        if (!parse_int_value(actual, &left) || !parse_int_value(cond->value, &right)) {
            return 0;
        }

        if (strcmp(cond->operator, "=") == 0) {
            return left == right;
        }
        if (strcmp(cond->operator, "!=") == 0 || strcmp(cond->operator, "<>") == 0) {
            return left != right;
        }
        if (strcmp(cond->operator, "<") == 0) {
            return left < right;
        }
        if (strcmp(cond->operator, ">") == 0) {
            return left > right;
        }
        if (strcmp(cond->operator, "<=") == 0) {
            return left <= right;
        }
        if (strcmp(cond->operator, ">=") == 0) {
            return left >= right;
        }
        return 0;
    }

    if (schema->columns[column_index].type == COL_FLOAT) {
        double left;
        double right;

        if (!parse_float_value(actual, &left) || !parse_float_value(cond->value, &right)) {
            return 0;
        }

        if (strcmp(cond->operator, "=") == 0) {
            return left == right;
        }
        if (strcmp(cond->operator, "!=") == 0 || strcmp(cond->operator, "<>") == 0) {
            return left != right;
        }
        if (strcmp(cond->operator, "<") == 0) {
            return left < right;
        }
        if (strcmp(cond->operator, ">") == 0) {
            return left > right;
        }
        if (strcmp(cond->operator, "<=") == 0) {
            return left <= right;
        }
        if (strcmp(cond->operator, ">=") == 0) {
            return left >= right;
        }
        return 0;
    }

    if (strcmp(cond->operator, "=") == 0) {
        return strcmp(actual, cond->value) == 0;
    }
    if (strcmp(cond->operator, "!=") == 0 || strcmp(cond->operator, "<>") == 0) {
        return strcmp(actual, cond->value) != 0;
    }
    if (strcmp(cond->operator, "<") == 0) {
        return strcmp(actual, cond->value) < 0;
    }
    if (strcmp(cond->operator, ">") == 0) {
        return strcmp(actual, cond->value) > 0;
    }
    if (strcmp(cond->operator, "<=") == 0) {
        return strcmp(actual, cond->value) <= 0;
    }
    if (strcmp(cond->operator, ">=") == 0) {
        return strcmp(actual, cond->value) >= 0;
    }

    return 0;
}

int evaluate_where(Row *row, Schema *schema, WhereClause *where) {
    int index;
    int result;

    if (where == NULL || where->condition_count == 0) {
        return 1;
    }

    result = evaluate_condition(row, schema, &where->conditions[0]);
    for (index = 1; index < where->condition_count; ++index) {
        if (strcmp(where->logical_op, "OR") == 0) {
            result = result || evaluate_condition(row, schema, &where->conditions[index]);
        } else {
            result = result && evaluate_condition(row, schema, &where->conditions[index]);
        }
    }

    return result;
}

int storage_insert(const char *table_name, Row *row, Schema *schema) {
    char path[MAX_PATH_LEN];
    FILE *check_file;
    FILE *file;
    int column_index;
    int file_exists = 0;
    int needs_header = 1;

    (void)table_name;

    if (row == NULL || schema == NULL) {
        fprintf(stderr, "[ERROR] Storage: insert input is NULL\n");
        return -1;
    }

    snprintf(path, sizeof(path), "%s/%s.tbl", g_data_dir, schema->table_name);

    check_file = fopen(path, "rb");
    if (check_file != NULL) {
        long file_size = -1;

        file_exists = 1;
        if (fseek(check_file, 0, SEEK_END) == 0) {
            file_size = ftell(check_file);
        }
        fclose(check_file);

        if (file_size < 0) {
            fprintf(stderr, "[ERROR] Storage: failed to inspect %s\n", path);
            return -1;
        }

        needs_header = (file_size == 0);
    }

    file = fopen(path, file_exists ? "a" : "w");
    if (file == NULL) {
        fprintf(stderr, "[ERROR] Storage: failed to open %s\n", path);
        return -1;
    }

    /* 새 파일일 때만 헤더를 한 번 기록한다. */
    if (needs_header) {
        for (column_index = 0; column_index < schema->column_count; ++column_index) {
            fprintf(file, "%s", schema->columns[column_index].name);
            if (column_index + 1 < schema->column_count) {
                fputc('|', file);
            }
        }
        fputc('\n', file);
    }

    for (column_index = 0; column_index < schema->column_count; ++column_index) {
        char encoded[(MAX_TOKEN_LEN * 2) + 1];

        if (row->is_null[column_index]) {
            fprintf(file, "%s", NULL_STORAGE_TOKEN);
        } else {
            if (!encode_storage_field(row->data[column_index], encoded, sizeof(encoded))) {
                fclose(file);
                fprintf(stderr, "[ERROR] Storage: failed to encode value for %s\n", path);
                return -1;
            }
            fprintf(file, "%s", encoded);
        }
        if (column_index + 1 < schema->column_count) {
            fputc('|', file);
        }
    }
    fputc('\n', file);

    fclose(file);
    return 0;
}

ResultSet *storage_select(const char *table_name,
                          Schema *schema,
                          ColumnList *columns,
                          WhereClause *where) {
    char path[MAX_PATH_LEN];
    FILE *file;
    char line[4096];
    ResultSet *result;
    int index;
    int primary_key_index;
    char **seen_primary_keys = NULL;
    size_t seen_primary_key_count = 0;
    size_t seen_primary_key_capacity = 0;

    if (table_name == NULL || schema == NULL) {
        fprintf(stderr, "[ERROR] Storage: select input is NULL\n");
        return NULL;
    }

    result = (ResultSet *)calloc(1, sizeof(ResultSet));
    if (result == NULL) {
        fprintf(stderr, "[ERROR] Storage: failed to allocate result set\n");
        return NULL;
    }

    result->schema = schema;
    primary_key_index = find_primary_key_index(schema);

    if (columns == NULL || columns->is_star) {
        result->selected_count = schema->column_count;
        for (index = 0; index < schema->column_count; ++index) {
            result->selected_indexes[index] = index;
        }
    } else {
        result->selected_count = columns->count;
        for (index = 0; index < columns->count; ++index) {
            int column_index = schema_get_column_index(schema, columns->names[index]);
            if (column_index < 0) {
                fprintf(stderr,
                        "[ERROR] Storage: unknown column '%s' in projection\n",
                        columns->names[index]);
                free_result_set(result);
                return NULL;
            }
            result->selected_indexes[index] = column_index;
        }
    }

    snprintf(path, sizeof(path), "%s/%s.tbl", g_data_dir, table_name);
    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return result;
        }

        fprintf(stderr, "[ERROR] Storage: failed to open %s\n", path);
        free_result_set(result);
        return NULL;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        if (ferror(file)) {
            fprintf(stderr, "[ERROR] Storage: failed to read %s\n", path);
        } else {
            fprintf(stderr, "[ERROR] Storage: empty table file missing header in %s\n", path);
        }
        fclose(file);
        free_result_set(result);
        return NULL;
    }
    if (line_was_truncated(line, sizeof(line))) {
        fprintf(stderr, "[ERROR] Storage: header line too long in %s\n", path);
        fclose(file);
        free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
        free_result_set(result);
        return NULL;
    }
    strip_newline(line);
    if (!validate_header_line(line, schema)) {
        fprintf(stderr, "[ERROR] Storage: header does not match schema in %s\n", path);
        fclose(file);
        free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
        free_result_set(result);
        return NULL;
    }

    /* 저장 파일은 단순 텍스트라서 한 줄씩 읽으며 필터링한다. */
    while (fgets(line, sizeof(line), file) != NULL) {
        Row full_row;

        if (line_was_truncated(line, sizeof(line))) {
            fprintf(stderr, "[ERROR] Storage: row too long in %s\n", path);
            fclose(file);
            free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
            free_result_set(result);
            return NULL;
        }

        strip_newline(line);
        memset(&full_row, 0, sizeof(full_row));
        full_row.column_count = schema->column_count;
        if (!split_pipe_line(line, full_row.data, full_row.is_null, schema->column_count)) {
            fprintf(stderr, "[ERROR] Storage: malformed row in %s\n", path);
            fclose(file);
            free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
            free_result_set(result);
            return NULL;
        }
        if (!validate_loaded_row(&full_row, schema)) {
            fprintf(stderr, "[ERROR] Storage: invalid row data in %s\n", path);
            fclose(file);
            free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
            free_result_set(result);
            return NULL;
        }
        if (primary_key_index >= 0) {
            if (has_seen_primary_key(seen_primary_keys,
                                     seen_primary_key_count,
                                     full_row.data[primary_key_index])) {
                fprintf(stderr,
                        "[ERROR] Storage: duplicate primary key '%s' found in %s\n",
                        full_row.data[primary_key_index],
                        path);
                fclose(file);
                free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
                free_result_set(result);
                return NULL;
            }
            if (!append_seen_primary_key(&seen_primary_keys,
                                         &seen_primary_key_count,
                                         &seen_primary_key_capacity,
                                         full_row.data[primary_key_index])) {
                fprintf(stderr, "[ERROR] Storage: out of memory while reading %s\n", path);
                fclose(file);
                free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
                free_result_set(result);
                return NULL;
            }
        }

        if (!evaluate_where(&full_row, schema, where)) {
            continue;
        }

        if (result->row_count >= MAX_ROWS) {
            fprintf(stderr, "[ERROR] Storage: result exceeds MAX_ROWS in %s\n", path);
            fclose(file);
            free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
            free_result_set(result);
            return NULL;
        }

        (void)index;
        result->rows[result->row_count++] = full_row;
    }

    fclose(file);
    free_seen_primary_keys(seen_primary_keys, seen_primary_key_count);
    return result;
}

void free_result_set(ResultSet *rs) {
    free(rs);
}
