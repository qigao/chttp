#ifndef CHTTP_FILE_SINK_H
#define CHTTP_FILE_SINK_H

#include <cflow/io_file.h>
#include <chttp/chttp.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum chttp_file_sink_result {
  CHTTP_FILE_SINK_READY = 0,
  CHTTP_FILE_SINK_WAIT,
  CHTTP_FILE_SINK_ERROR
} chttp_file_sink_result;

typedef void (*chttp_file_sink_ready_fn)(void *user);

typedef struct chttp_file_sink_transfer {
  cflow_io_file file;
  unsigned char *buffer;
  chttp_progress_fn progress;
  void *progress_user;
  chttp_file_sink_ready_fn ready;
  void *ready_user;
  cflow_io_request_id pending_request;
  cflow_io_lease_id next_lease;
  uint64_t file_offset;
  size_t buffer_capacity;
  size_t buffer_offset;
  size_t buffer_size;
  size_t submitted_size;
  size_t transferred;
  int status;
  int native_status;
  cflow_io_native_file_operation_kind pending_operation;
  bool pending;
  bool flushed;
  bool close_requested;
} chttp_file_sink_transfer;

int chttp_file_sink_transfer_open(chttp_file_sink_transfer *transfer,
                                  cflow_io_file_runtime *runtime, const char *path,
                                  size_t buffer_capacity, chttp_progress_fn progress,
                                  void *progress_user);
void chttp_file_sink_transfer_set_ready(chttp_file_sink_transfer *transfer,
                                        chttp_file_sink_ready_fn ready, void *ready_user);
int chttp_file_sink_transfer_append(chttp_file_sink_transfer *transfer, const void *data,
                                    size_t size);
chttp_file_sink_result chttp_file_sink_transfer_advance(chttp_file_sink_transfer *transfer);
chttp_file_sink_result chttp_file_sink_transfer_write(chttp_file_sink_transfer *transfer,
                                                      const void *data, size_t size);
chttp_file_sink_result chttp_file_sink_transfer_flush(chttp_file_sink_transfer *transfer);
int chttp_file_sink_transfer_flush_drain(chttp_file_sink_transfer *transfer,
                                         cflow_io_file_runtime *runtime);
bool chttp_file_sink_transfer_ready(const chttp_file_sink_transfer *transfer);
size_t chttp_file_sink_transfer_transferred(const chttp_file_sink_transfer *transfer);
int chttp_file_sink_transfer_status(const chttp_file_sink_transfer *transfer,
                                    int *out_native_status);
int chttp_file_sink_transfer_close(chttp_file_sink_transfer *transfer);
int chttp_file_sink_transfer_destroy(chttp_file_sink_transfer *transfer);
int chttp_file_sink_transfer_drain_destroy(chttp_file_sink_transfer *transfer,
                                           cflow_io_file_runtime *runtime);

#endif /* CHTTP_FILE_SINK_H */
