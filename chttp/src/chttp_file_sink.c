#include "chttp_file_sink.h"

#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdlib.h>
#include <string.h>

enum { CHTTP_FILE_CREATE_MODE = 0600 };

static void chttp_file_sink_notify(chttp_file_sink_transfer *transfer) {
  if (transfer != NULL && transfer->ready != NULL) transfer->ready(transfer->ready_user);
}

static void chttp_file_sink_fail(chttp_file_sink_transfer *transfer, int status,
                                 int native_status) {
  if (transfer == NULL || transfer->status != SALTS_OK) return;
  transfer->status = status;
  transfer->native_status = native_status;
}

static int chttp_file_sink_map_submit(chttp_file_sink_transfer *transfer,
                                      cflow_io_file_submit_status status) {
  int mapped;
  switch (status) {
  case CFLOW_IO_FILE_SUBMIT_ACCEPTED:
    return SALTS_OK;
  case CFLOW_IO_FILE_SUBMIT_FULL:
    mapped = SALTS_ENOBUFS;
    break;
  case CFLOW_IO_FILE_SUBMIT_CLOSED:
    mapped = SALTS_ESHUTDOWN;
    break;
  case CFLOW_IO_FILE_SUBMIT_UNSUPPORTED:
    mapped = SALTS_ENOTSUP;
    break;
  case CFLOW_IO_FILE_SUBMIT_ACCESS_DENIED:
    mapped = SALTS_EPERM;
    break;
  case CFLOW_IO_FILE_SUBMIT_LEASE_IN_USE:
    mapped = SALTS_EBUSY;
    break;
  case CFLOW_IO_FILE_SUBMIT_ID_EXHAUSTED:
    mapped = SALTS_ERANGE;
    break;
  case CFLOW_IO_FILE_SUBMIT_INVALID_ARGUMENT:
  default:
    mapped = SALTS_EINVAL;
    break;
  }
  chttp_file_sink_fail(transfer, mapped, 0);
  return mapped;
}

static void chttp_file_sink_complete(void *user, cflow_io_request_id request_id,
                                     cflow_io_lease_id lease_id,
                                     cflow_io_native_file_operation_kind operation_kind,
                                     const cflow_io_completion *completion) {
  chttp_file_sink_transfer *transfer = (chttp_file_sink_transfer *)user;
  (void)lease_id;
  if (transfer == NULL || completion == NULL || !transfer->pending ||
      transfer->pending_request != request_id || transfer->pending_operation != operation_kind) {
    if (transfer != NULL) chttp_file_sink_fail(transfer, SALTS_EPROTO, 0);
    chttp_file_sink_notify(transfer);
    return;
  }
  transfer->pending = false;
  transfer->pending_request = 0u;
  if (completion->kind == CFLOW_IO_COMPLETION_FAILED) {
    chttp_file_sink_fail(transfer, SALTS_EIO, completion->error);
  } else if (completion->kind == CFLOW_IO_COMPLETION_CANCELLED) {
    chttp_file_sink_fail(transfer, SALTS_ECANCELED, completion->error);
  } else if (operation_kind == CFLOW_IO_NATIVE_FILE_WRITE_AT) {
    if (completion->kind != CFLOW_IO_COMPLETION_OK || completion->bytes == 0u ||
        completion->bytes > transfer->submitted_size ||
        completion->bytes > transfer->buffer_size - transfer->buffer_offset) {
      chttp_file_sink_fail(transfer, SALTS_EIO, completion->error);
    } else {
      transfer->buffer_offset += completion->bytes;
      transfer->file_offset += completion->bytes;
      transfer->transferred += completion->bytes;
      if (transfer->progress != NULL)
        transfer->progress(transfer->progress_user, transfer->transferred, 0u);
      if (transfer->buffer_offset == transfer->buffer_size) {
        transfer->buffer_offset = 0u;
        transfer->buffer_size = 0u;
      }
    }
  } else if (operation_kind == CFLOW_IO_NATIVE_FILE_FLUSH) {
    if (completion->kind != CFLOW_IO_COMPLETION_OK)
      chttp_file_sink_fail(transfer, SALTS_EIO, completion->error);
    else transfer->flushed = true;
  } else {
    chttp_file_sink_fail(transfer, SALTS_EPROTO, 0);
  }
  chttp_file_sink_notify(transfer);
}

int chttp_file_sink_transfer_open(chttp_file_sink_transfer *transfer,
                                  cflow_io_file_runtime *runtime, const char *path,
                                  size_t buffer_capacity, chttp_progress_fn progress,
                                  void *progress_user) {
  cflow_io_file_config config = {0};
  int status;
  if (transfer == NULL || transfer->file.impl != NULL || runtime == NULL || path == NULL ||
      path[0] == '\0' || buffer_capacity == 0u)
    return SALTS_EINVAL;
  memset(transfer, 0, sizeof(*transfer));
  transfer->buffer = (unsigned char *)malloc(buffer_capacity);
  if (transfer->buffer == NULL) return SALTS_ENOMEM;
  transfer->buffer_capacity = buffer_capacity;
  transfer->progress = progress;
  transfer->progress_user = progress_user;
  transfer->next_lease = 1u;
  config.open_flags = CFLOW_IO_FILE_WRITE | CFLOW_IO_FILE_CREATE | CFLOW_IO_FILE_TRUNCATE;
  config.create_mode = CHTTP_FILE_CREATE_MODE;
  config.completion = chttp_file_sink_complete;
  config.completion_user = transfer;
  config.runtime = runtime;
  status = cflow_io_file_open(&transfer->file, path, &config);
  if (status != SALTS_OK) {
    free(transfer->buffer);
    memset(transfer, 0, sizeof(*transfer));
    return status;
  }
  if (progress != NULL) progress(progress_user, 0u, 0u);
  return SALTS_OK;
}

void chttp_file_sink_transfer_set_ready(chttp_file_sink_transfer *transfer,
                                        chttp_file_sink_ready_fn ready, void *ready_user) {
  if (transfer == NULL) return;
  transfer->ready = ready;
  transfer->ready_user = ready_user;
}

int chttp_file_sink_transfer_append(chttp_file_sink_transfer *transfer, const void *data,
                                    size_t size) {
  int status = SALTS_OK;
  if (transfer == NULL || transfer->file.impl == NULL || (data == NULL && size != 0u))
    return SALTS_EINVAL;
  if (transfer->status != SALTS_OK) return transfer->status;
  if (transfer->pending || transfer->buffer_offset != 0u) status = SALTS_EBUSY;
  if (transfer->buffer_size > transfer->buffer_capacity ||
      size > transfer->buffer_capacity - transfer->buffer_size)
    status = SALTS_ENOBUFS;
  if (status != SALTS_OK) {
    chttp_file_sink_fail(transfer, status, 0);
    return status;
  }
  if (size != 0u) memcpy(transfer->buffer + transfer->buffer_size, data, size);
  transfer->buffer_size += size;
  transfer->flushed = false;
  return SALTS_OK;
}

static chttp_file_sink_result chttp_file_sink_submit_write(chttp_file_sink_transfer *transfer) {
  cflow_io_file_submit_result submitted;
  cflow_io_lease_id lease;
  const size_t size = transfer->buffer_size - transfer->buffer_offset;
  lease = transfer->next_lease++;
  if (transfer->next_lease == 0u) transfer->next_lease = 1u;
  transfer->submitted_size = size;
  transfer->pending_operation = CFLOW_IO_NATIVE_FILE_WRITE_AT;
  submitted =
      cflow_io_file_try_write_at(&transfer->file, lease, transfer->buffer + transfer->buffer_offset,
                                 size, transfer->file_offset);
  if (submitted.status != CFLOW_IO_FILE_SUBMIT_ACCEPTED) {
    (void)chttp_file_sink_map_submit(transfer, submitted.status);
    return CHTTP_FILE_SINK_ERROR;
  }
  transfer->pending = true;
  transfer->pending_request = submitted.request_id;
  return CHTTP_FILE_SINK_WAIT;
}

chttp_file_sink_result chttp_file_sink_transfer_advance(chttp_file_sink_transfer *transfer) {
  if (transfer == NULL || transfer->file.impl == NULL) return CHTTP_FILE_SINK_ERROR;
  if (transfer->status != SALTS_OK) return CHTTP_FILE_SINK_ERROR;
  if (transfer->pending) return CHTTP_FILE_SINK_WAIT;
  if (transfer->buffer_offset < transfer->buffer_size)
    return chttp_file_sink_submit_write(transfer);
  transfer->buffer_offset = 0u;
  transfer->buffer_size = 0u;
  return CHTTP_FILE_SINK_READY;
}

chttp_file_sink_result chttp_file_sink_transfer_write(chttp_file_sink_transfer *transfer,
                                                      const void *data, size_t size) {
  const int status = chttp_file_sink_transfer_append(transfer, data, size);
  if (status != SALTS_OK) return CHTTP_FILE_SINK_ERROR;
  return chttp_file_sink_transfer_advance(transfer);
}

chttp_file_sink_result chttp_file_sink_transfer_flush(chttp_file_sink_transfer *transfer) {
  cflow_io_file_submit_result submitted;
  cflow_io_lease_id lease;
  chttp_file_sink_result write_result;
  if (transfer == NULL || transfer->file.impl == NULL) return CHTTP_FILE_SINK_ERROR;
  if (transfer->status != SALTS_OK) return CHTTP_FILE_SINK_ERROR;
  write_result = chttp_file_sink_transfer_advance(transfer);
  if (write_result != CHTTP_FILE_SINK_READY) return write_result;
  if (transfer->flushed) return CHTTP_FILE_SINK_READY;
  lease = transfer->next_lease++;
  if (transfer->next_lease == 0u) transfer->next_lease = 1u;
  transfer->submitted_size = 0u;
  transfer->pending_operation = CFLOW_IO_NATIVE_FILE_FLUSH;
  submitted = cflow_io_file_try_flush(&transfer->file, lease);
  if (submitted.status != CFLOW_IO_FILE_SUBMIT_ACCEPTED) {
    (void)chttp_file_sink_map_submit(transfer, submitted.status);
    return CHTTP_FILE_SINK_ERROR;
  }
  transfer->pending = true;
  transfer->pending_request = submitted.request_id;
  return CHTTP_FILE_SINK_WAIT;
}

int chttp_file_sink_transfer_flush_drain(chttp_file_sink_transfer *transfer,
                                         cflow_io_file_runtime *runtime) {
  chttp_file_sink_result result;
  if (transfer == NULL || runtime == NULL || transfer->file.impl == NULL) return SALTS_EINVAL;
  if (!cflow_io_file_operation_supported(&transfer->file, CFLOW_IO_NATIVE_FILE_FLUSH))
    return SALTS_ENOTSUP;
  result = chttp_file_sink_transfer_flush(transfer);
  while (result == CHTTP_FILE_SINK_WAIT) {
    size_t progressed = 0u;
    int status = cflow_io_file_runtime_run_ready(runtime, 64u, &progressed);
    if (status != SALTS_OK) return status;
    result = chttp_file_sink_transfer_flush(transfer);
    if (result == CHTTP_FILE_SINK_WAIT && progressed == 0u) salts_thread_yield();
  }
  return result == CHTTP_FILE_SINK_READY ? SALTS_OK
                                         : chttp_file_sink_transfer_status(transfer, NULL);
}

bool chttp_file_sink_transfer_ready(const chttp_file_sink_transfer *transfer) {
  return transfer != NULL && (transfer->status != SALTS_OK || !transfer->pending);
}

size_t chttp_file_sink_transfer_transferred(const chttp_file_sink_transfer *transfer) {
  return transfer != NULL ? transfer->transferred : 0u;
}

int chttp_file_sink_transfer_status(const chttp_file_sink_transfer *transfer,
                                    int *out_native_status) {
  if (transfer == NULL) return SALTS_EINVAL;
  if (out_native_status != NULL) *out_native_status = transfer->native_status;
  return transfer->status;
}

int chttp_file_sink_transfer_close(chttp_file_sink_transfer *transfer) {
  int status;
  if (transfer == NULL || transfer->file.impl == NULL) return SALTS_EINVAL;
  if (transfer->close_requested) return SALTS_OK;
  chttp_file_sink_transfer_set_ready(transfer, NULL, NULL);
  status = cflow_io_file_close(&transfer->file);
  if (status == SALTS_OK || status == SALTS_EALREADY) transfer->close_requested = true;
  return status == SALTS_EALREADY ? SALTS_OK : status;
}

int chttp_file_sink_transfer_destroy(chttp_file_sink_transfer *transfer) {
  size_t transferred;
  int terminal_status;
  int native_status;
  int status;
  if (transfer == NULL) return SALTS_EINVAL;
  if (transfer->file.impl != NULL) {
    if (!transfer->close_requested || !cflow_io_file_is_quiescent(&transfer->file))
      return SALTS_EBUSY;
    status = cflow_io_file_destroy(&transfer->file);
    if (status != SALTS_OK) return status;
  }
  transferred = transfer->transferred;
  terminal_status = transfer->status;
  native_status = transfer->native_status;
  free(transfer->buffer);
  memset(transfer, 0, sizeof(*transfer));
  transfer->transferred = transferred;
  transfer->status = terminal_status;
  transfer->native_status = native_status;
  return SALTS_OK;
}

int chttp_file_sink_transfer_drain_destroy(chttp_file_sink_transfer *transfer,
                                           cflow_io_file_runtime *runtime) {
  int first_status = SALTS_OK;
  int status;
  if (transfer == NULL || runtime == NULL) return SALTS_EINVAL;
  if (transfer->file.impl == NULL) return SALTS_OK;
  for (;;) {
    status = chttp_file_sink_transfer_close(transfer);
    if (status == SALTS_OK) break;
    if (status != SALTS_ENOBUFS) return status;
    {
      size_t progressed = 0u;
      status = cflow_io_file_runtime_run_ready(runtime, 64u, &progressed);
      if (status != SALTS_OK) return status;
      if (progressed == 0u) salts_thread_yield();
    }
  }
  while (!cflow_io_file_is_quiescent(&transfer->file)) {
    size_t progressed = 0u;
    status = cflow_io_file_runtime_run_ready(runtime, 64u, &progressed);
    if (status != SALTS_OK && first_status == SALTS_OK) first_status = status;
    if (status != SALTS_OK) break;
    if (progressed == 0u) salts_thread_yield();
  }
  if (first_status != SALTS_OK) return first_status;
  return chttp_file_sink_transfer_destroy(transfer);
}
