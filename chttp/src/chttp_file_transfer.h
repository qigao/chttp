#ifndef CHTTP_FILE_TRANSFER_H
#define CHTTP_FILE_TRANSFER_H

#include <cflow/io_file.h>
#include <chttp/chttp.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum chttp_file_source_result {
  CHTTP_FILE_SOURCE_DATA = 0,
  CHTTP_FILE_SOURCE_WAIT,
  CHTTP_FILE_SOURCE_EOF,
  CHTTP_FILE_SOURCE_ERROR
} chttp_file_source_result;

typedef void (*chttp_file_ready_fn)(void *user);

typedef struct chttp_file_transfer {
  cflow_io_file file;
  unsigned char *chunk;
  chttp_progress_fn progress;
  void *progress_user;
  chttp_file_ready_fn ready;
  void *ready_user;
  cflow_io_request_id pending_request;
  cflow_io_lease_id next_lease;
  uint64_t file_offset;
  size_t chunk_capacity;
  size_t ready_offset;
  size_t ready_size;
  size_t submitted_size;
  size_t transferred;
  size_t total;
  int status;
  int native_status;
  bool pending;
  bool eof_ready;
  bool close_requested;
  bool owner_release_requested;
} chttp_file_transfer;

int chttp_file_transfer_open_read(chttp_file_transfer *transfer, cflow_io_file_runtime *runtime,
                                  const char *path, size_t total, size_t chunk_capacity,
                                  chttp_progress_fn progress, void *progress_user);

void chttp_file_transfer_set_ready(chttp_file_transfer *transfer, chttp_file_ready_fn ready,
                                   void *ready_user);

chttp_file_source_result chttp_file_transfer_read(chttp_file_transfer *transfer, void *buffer,
                                                  size_t capacity, size_t *out_size);

bool chttp_file_transfer_ready(const chttp_file_transfer *transfer);
size_t chttp_file_transfer_transferred(const chttp_file_transfer *transfer);
int chttp_file_transfer_status(const chttp_file_transfer *transfer, int *out_native_status);
int chttp_file_transfer_close(chttp_file_transfer *transfer);
int chttp_file_transfer_destroy(chttp_file_transfer *transfer);
int chttp_file_transfer_drain_destroy(chttp_file_transfer *transfer,
                                      cflow_io_file_runtime *runtime);

#endif /* CHTTP_FILE_TRANSFER_H */
