/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <stdint.h>
#include <stddef.h>

// The dependency package exposes the old libstdc++ ABI. Keep all Arrow C++
// objects behind this C boundary; no STL object crosses into the SQL engine.
#ifdef __cplusplus
extern "C" {
#endif
void *file_parquet_open(int owned_fd, char *error, size_t error_size);
int file_parquet_column_count(void *reader);
int file_parquet_column(void *reader, int index, const char **name, int *type, int *nullable);
int file_parquet_project(void *reader, const int *indices, int count, char *error, size_t size);
int file_parquet_next(void *reader, int *end, char *error, size_t size);
int file_parquet_cell(void *reader, int index, const char **text, size_t *length, int *is_null,
                      char *error, size_t size);
void file_parquet_close(void *reader);
#ifdef __cplusplus
}
#endif
