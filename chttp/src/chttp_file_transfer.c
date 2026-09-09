#include "chttp_file_transfer.h"

#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdlib.h>
#include <string.h>

static void chttp_file_transfer_notify(chttp_file_transfer *transfer) {
  if (transfer != NULL && transfer->ready != NULL) transfer->ready(transfer->ready_user);
}

static void chttp_file_transfer_fail(chttp_file_transfer *transfer, int status, int native_status) {
  if (transfer == NULL || transfer->status != SALTS_OK) return;
  transfer->status = status;
  transfer->native_status = native_status;
}

static void chttp_file_transfer_complete(void *user, cflow_io_request_id request_id,
                                         cflow_io_lease_id lease_id,
                                         cflow_io_native_file_operation_kind operation_kind,
                                         const cflow_io_completion *completion) {
  chttp_file_transfer *transfer = (chttp_file_transfer *)user;
  (void)lease_id;
  if (transfer == NULL || completion == NULL || !transfer->pending ||
      transfer->pending_request != request_id || operation_kind != CFLOW_IO_NATIVE_FILE_READ_AT) {
    if (transfer != NULL) chttp_file_transfer_fail(transfer, SALTS_EPROTO, 0);
    chttp_file_transfer_notify(transfer);
    return;
  }
  transfer->pending = false;
  transfer->pending_request = 0u;
  if (completion->kind == CFLOW_IO_COMPLETION_FAILED) {
    chttp_file_transfer_fail(transfer, SALTS_EIO, completion->error);
  } else if (completion->kind == CFLOW_IO_COMPLETION_CANCELLED) {
    chttp_file_transfer_fail(transfer, SALTS_ECANCELED, completion->error);
  } else if (completion->bytes > transfer->submitted_size) {
    chttp_file_transfer_fail(transfer, SALTS_EPROTO, 0);
  } else if (transfer->file_offset >= transfer->total) {
    if (completion->kind == CFLOW_IO_COMPLETION_EOF || completion->bytes == 0u)
      transfer->eof_ready = true;
    else chttp_file_transfer_fail(transfer, SALTS_EPROTO, 0);
  } else if (completion->kind == CFLOW_IO_COMPLETION_EOF || completion->bytes == 0u) {
    chttp_file_transfer_fail(transfer, SALTS_EPROTO, 0);
  } else if (completion->kind != CFLOW_IO_COMPLETION_OK) {
    chttp_file_transfer_fail(transfer, SALTS_EPROTO, 0);
  } else {
    transfer->ready_offset = 0u;
    transfer->ready_size = completion->bytes;
    transfer->file_offset += completion->bytes;
  }
  chttp_file_transfer_notify(transfer);
}

static int chttp_file_transfer_map_submit(chttp_file_transfer *transfer,
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
  chttp_file_transfer_fail(transfer, mapped, 0);
  return mapped;
}

int chttp_file_transfer_open_read(chttp_file_transfer *transfer, cflow_io_file_runtime *runtime,
                                  const char *path, size_t total, size_t chunk_capacity,
                                  chttp_progress_fn progress, void *progress_user) {
  cflow_io_file_config config = {0};
  int status;
  if (transfer == NULL || transfer->file.impl != NULL || runtime == NULL || path == NULL ||
      path[0] == '\0' || chunk_capacity == 0u)
    return SALTS_EINVAL;
  memset(transfer, 0, sizeof(*transfer));
  transfer->chunk = (unsigned char *)malloc(chunk_capacity);
  if (transfer->chunk == NULL) return SALTS_ENOMEM;
  transfer->chunk_capacity = chunk_capacity;
  transfer->total = total;
  transfer->progress = progress;
  transfer->progress_user = progress_user;
  transfer->next_lease = 1u;
  config.open_flags = CFLOW_IO_FILE_READ;
  config.completion = chttp_file_transfer_complete;
  config.completion_user = transfer;
  config.runtime = runtime;
  status = cflow_io_file_open(&transfer->file, path, &config);
  if (status != SALTS_OK) {
    free(transfer->chunk);
    memset(transfer, 0, sizeof(*transfer));
    return status;
  }
  if (progress != NULL) progress(progress_user, 0u, total);
  return SALTS_OK;
}

void chttp_file_transfer_set_ready(chttp_file_transfer *transfer, chttp_file_ready_fn ready,
                                   void *ready_user) {
  if (transfer == NULL) return;
  transfer->ready = ready;
  transfer->ready_user = ready_user;
}

static chttp_file_source_result chttp_file_transfer_submit_read(chttp_file_transfer *transfer,
                                                                size_t capacity) {
  cflow_io_file_submit_result submitted;
  size_t request_size = capacity;
  cflow_io_lease_id lease;
  if (request_size > transfer->chunk_capacity) request_size = transfer->chunk_capacity;
  if (transfer->file_offset < transfer->total) {
    const uint64_t remaining = (uint64_t)transfer->total - transfer->file_offset;
    if ((uint64_t)request_size > remaining) request_size = (size_t)remaining;
  } else {
    request_size = 1u;
  }
  lease = transfer->next_lease++;
  if (transfer->next_lease == 0u) transfer->next_lease = 1u;
  transfer->submitted_size = request_size;
  submitted = cflow_io_file_try_read_at(&transfer->file, lease, transfer->chunk, request_size,
                                        transfer->file_offset);
  if (submitted.status != CFLOW_IO_FILE_SUBMIT_ACCEPTED) {
    (void)chttp_file_transfer_map_submit(transfer, submitted.status);
    return CHTTP_FILE_SOURCE_ERROR;
  }
  transfer->pending = true;
  transfer->pending_request = submitted.request_id;
  return CHTTP_FILE_SOURCE_WAIT;
}

chttp_file_source_result chttp_file_transfer_read(chttp_file_transfer *transfer, void *buffer,
                                                  size_t capacity, size_t *out_size) {
  size_t available;
  size_t copied;
  if (out_size != NULL) *out_size = 0u;
  if (transfer == NULL || transfer->file.impl == NULL || buffer == NULL || capacity == 0u ||
      out_size == NULL)
    return CHTTP_FILE_SOURCE_ERROR;
  if (transfer->status != SALTS_OK) return CHTTP_FILE_SOURCE_ERROR;
  if (transfer->ready_offset < transfer->ready_size) {
    available = transfer->ready_size - transfer->ready_offset;
    copied = available < capacity ? available : capacity;
    memcpy(buffer, transfer->chunk + transfer->ready_offset, copied);
    transfer->ready_offset += copied;
    transfer->transferred += copied;
    *out_size = copied;
    if (transfer->progress != NULL)
      transfer->progress(transfer->progress_user, transfer->transferred, transfer->total);
    if (transfer->ready_offset == transfer->ready_size) {
      transfer->ready_offset = 0u;
      transfer->ready_size = 0u;
    }
    return CHTTP_FILE_SOURCE_DATA;
  }
  if (transfer->eof_ready) return CHTTP_FILE_SOURCE_EOF;
  if (transfer->pending) return CHTTP_FILE_SOURCE_WAIT;
  return chttp_file_transfer_submit_read(transfer, capacity);
}

bool chttp_file_transfer_ready(const chttp_file_transfer *transfer) {
  return transfer != NULL &&
         (transfer->status != SALTS_OK || transfer->eof_ready || transfer->ready_size != 0u);
}

size_t chttp_file_transfer_transferred(const chttp_file_transfer *transfer) {
  return transfer != NULL ? transfer->transferred : 0u;
}

int chttp_file_transfer_status(const chttp_file_transfer *transfer, int *out_native_status) {
  if (transfer == NULL) return SALTS_EINVAL;
  if (out_native_status != NULL) *out_native_status = transfer->native_status;
  return transfer->status;
}

int chttp_file_transfer_close(chttp_file_transfer *transfer) {
  int status;
  if (transfer == NULL || transfer->file.impl == NULL) return SALTS_EINVAL;
  if (transfer->close_requested) return SALTS_OK;
  chttp_file_transfer_set_ready(transfer, NULL, NULL);
  status = cflow_io_file_close(&transfer->file);
  if (status == SALTS_OK || status == SALTS_EALREADY) transfer->close_requested = true;
  return status == SALTS_EALREADY ? SALTS_OK : status;
}

int chttp_file_transfer_destroy(chttp_file_transfer *transfer) {
  int status;
  size_t transferred;
  size_t total;
  int terminal_status;
  int native_status;
  if (transfer == NULL) return SALTS_EINVAL;
  if (transfer->file.impl != NULL) {
    if (!transfer->close_requested || !cflow_io_file_is_quiescent(&transfer->file))
      return SALTS_EBUSY;
    status = cflow_io_file_destroy(&transfer->file);
    if (status != SALTS_OK) return status;
  }
  transferred = transfer->transferred;
  total = transfer->total;
  terminal_status = transfer->status;
  native_status = transfer->native_status;
  free(transfer->chunk);
  memset(transfer, 0, sizeof(*transfer));
  transfer->transferred = transferred;
  transfer->total = total;
  transfer->status = terminal_status;
  transfer->native_status = native_status;
  return SALTS_OK;
}

int chttp_file_transfer_drain_destroy(chttp_file_transfer *transfer,
                                      cflow_io_file_runtime *runtime) {
  int first_status = SALTS_OK;
  int status;
  if (transfer == NULL || runtime == NULL) return SALTS_EINVAL;
  if (transfer->file.impl == NULL) return SALTS_OK;
  for (;;) {
    status = chttp_file_transfer_close(transfer);
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
  return chttp_file_transfer_destroy(transfer);
}
