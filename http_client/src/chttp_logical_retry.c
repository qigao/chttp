#include "chttp_logical_retry.h"
#include "chttp_client_internal.h"
#include "chttp_response_owner.h"
#include "chttp_retry.h"
#include "chttp_tls.h"
#include <core/clock.h>
#include <file/fs.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_logical_operation {
  chttp_logical_runtime *runtime;
  chttp_retry_request input;
  chttp_request_options request;
  chttp_request_replay replay;
  chttp_body_source source;
  chttp_body_sink sink;
  chttp_retry_budget budget;
  chttp_attempt_facts facts;
  chttp_error error;
  chttp_request physical;
  chttp_logical_request handle;
  chttp_logical_complete_fn complete;
  chttp_tls_profile profile;
  cflow_io_file_runtime *file_runtime;
  chttp_file_transfer file_source;
  chttp_file_sink_transfer file_sink;
  fs_stat_t source_stat;
  tstr path, temporary_path;
  chttp_response response;
  unsigned file_mode;
  void *user;
  uint32_t generation, idle_ms;
  bool live, done, delivered, prepared, readmit, canceled, starting, success, source_stat_set;
  bool temporary_created;
} chttp_logical_operation;

struct chttp_logical_runtime {
  chttp_async_client client;
  chttp_client_config config;
  chttp_logical_operation *operations;
  bool sealed;
};

int chttp_logical_runtime_init(chttp_logical_runtime **out, chttp_async_client *client,
                               const chttp_client_config *config) {
  if (config->request_capacity > SIZE_MAX / sizeof(chttp_logical_operation)) return SALTS_ERANGE;
  chttp_logical_runtime *r = calloc(1u, sizeof(*r));
  if (r == NULL) return SALTS_ENOMEM;
  r->operations = calloc(config->request_capacity, sizeof(*r->operations));
  if (r->operations == NULL) {
    free(r);
    return SALTS_ENOMEM;
  }
  r->client = *client;
  r->config = *config;
  *out = r;
  return SALTS_OK;
}

static void logical_release(chttp_logical_operation *op) {
  const uint32_t generation = op->generation;
  chttp_retry_request_destroy(&op->input);
  chttp_tls_profile_release(op->profile.impl);
  chttp_response_destroy(&op->response);
  tstr_free(op->path);
  tstr_free(op->temporary_path);
  *op = (chttp_logical_operation){.generation = generation};
}

static void logical_deliver(chttp_logical_operation *op, const chttp_response_view *response,
                            const chttp_error *error) {
  if (op->delivered) return;
  op->delivered = true;
  chttp_async_client_logical_complete(&op->runtime->client, op->complete, op->user, op->handle,
                                      response, error);
}

static void logical_failure(chttp_logical_operation *op, int status, const char *stage) {
  op->error.status = status;
  op->error.stage = stage;
  logical_deliver(op, NULL, &op->error);
}

static void logical_attempt_complete(void *user, chttp_request request,
                                     const chttp_response_view *response,
                                     const chttp_error *error) {
  chttp_logical_operation *op = user;
  op->done = true;
  const int status = chttp_async_client_attempt_facts(&op->runtime->client, request, &op->facts);
  if (status != SALTS_OK) {
    logical_failure(op, status, "attempt-facts");
    return;
  }
  if (op->canceled || op->runtime->sealed) {
    op->error = (chttp_error){op->canceled ? SALTS_ECANCELED : SALTS_ESHUTDOWN, 0,
                              op->canceled ? "cancel" : "shutdown"};
    if (op->file_mode == 0u) logical_deliver(op, NULL, &op->error);
  } else if (error == NULL) {
    if (response == NULL) logical_failure(op, SALTS_EPROTO, "response");
    else if (op->file_mode != 0u) {
      const int copied = chttp_response_copy(response, &op->response);
      if (copied != SALTS_OK) op->error = (chttp_error){copied, 0, "response-copy"};
      else op->success = true;
    } else logical_deliver(op, response, NULL);
  } else op->error = *error;
}

static int logical_admit(chttp_logical_operation *op) {
  int status = chttp_async_client_submit_logical_attempt(
      &op->runtime->client, &op->request, op->file_mode == 1u ? &op->file_source : NULL,
      op->file_mode == 2u ? &op->file_sink : NULL, op->budget.gate.deadline_ms, op->idle_ms,
      &op->physical);
  if (status == SALTS_OK) {
    op->done = false;
    op->prepared = false;
    op->readmit = false;
    op->starting = false;
    op->success = false;
    op->facts = (chttp_attempt_facts){0};
  }
  return status;
}

static int logical_submit(chttp_async_client *client, const chttp_request_options *request,
                          const chttp_request_timeouts *timeouts, const chttp_retry_options *retry,
                          const chttp_request_replay *replay, chttp_logical_complete_fn complete,
                          void *user, chttp_logical_request *out, const chttp_retry_files *files) {
  if (out == NULL) return SALTS_EINVAL;
  *out = (chttp_logical_request){0};
  if (request == NULL || request->on_complete != NULL || complete == NULL || timeouts == NULL ||
      timeouts->total_timeout_ms == 0u || retry == NULL || retry->size != sizeof(*retry) ||
      retry->max_attempts == 0u || retry->initial_backoff_ms > retry->maximum_backoff_ms)
    return SALTS_EINVAL;
  const uint64_t now = monotonic_ms();
  if (now > UINT64_MAX - timeouts->total_timeout_ms) return SALTS_ERANGE;
  if (retry->max_attempts > 1u &&
      ((request->body_source != NULL && (replay == NULL || replay->rewind_source == NULL ||
                                         !request->body_source->content_length_known)) ||
       (request->body_sink != NULL && (replay == NULL || replay->rollback_sink == NULL))))
    return SALTS_ENOTSUP;
  if (files != NULL &&
      ((files->upload_path == NULL) == (files->download_path == NULL) || request->body != NULL ||
       request->body_size != 0u || request->body_source != NULL || request->body_sink != NULL))
    return SALTS_EINVAL;
  chttp_logical_runtime *r;
  int status = chttp_async_client_logical_runtime(client, &r);
  if (status != SALTS_OK) return status;
  size_t i;
  for (i = 0u; i < r->config.request_capacity; ++i)
    if (!r->operations[i].live && r->operations[i].generation != UINT32_MAX) break;
  if (i == r->config.request_capacity) return SALTS_ENOBUFS;
  chttp_logical_operation *op = &r->operations[i];
  ++op->generation;
  op->runtime = r;
  op->complete = complete;
  op->user = user;
  op->handle = (chttp_logical_request){(uint32_t)i + 1u, op->generation};
  op->idle_ms = timeouts->response_idle_timeout_ms;
  if (replay != NULL) op->replay = *replay;
  const chttp_options input = {.connection_uri = request->connection_uri,
                               .authority = request->authority,
                               .target = request->target,
                               .headers = request->headers,
                               .header_count = request->header_count,
                               .body = request->body,
                               .body_size = request->body_size,
                               .protocol = request->protocol};
  status = chttp_retry_request_copy(&op->input, &input, &r->config);
  if (status != SALTS_OK) goto rejected;
  chttp_tls_profile_impl *profile;
  status = chttp_tls_profile_acquire(request->tls, &profile);
  if (status != SALTS_OK) goto rejected;
  op->profile.impl = profile;
  op->request = *request;
  op->request.connection_uri = op->input.options.connection_uri;
  op->request.authority = op->input.options.authority;
  op->request.target = op->input.options.target;
  op->request.headers = op->input.options.headers;
  op->request.body = op->input.options.body;
  op->request.tls = profile != NULL ? &op->profile : NULL;
  if (request->body_source != NULL) {
    op->source = *request->body_source;
    op->request.body_source = &op->source;
  }
  if (request->body_sink != NULL) {
    op->sink = *request->body_sink;
    op->request.body_sink = &op->sink;
  }
  op->request.on_complete = logical_attempt_complete;
  op->request.user = op;
  chttp_retry_budget_init(&op->budget, retry, request->method,
                          request->body_source != NULL ? op->source.content_length
                                                       : request->body_size,
                          now + timeouts->total_timeout_ms);
  if (files != NULL) {
    op->file_mode = files->upload_path != NULL ? 1u : 2u;
    const char *path = op->file_mode == 1u ? files->upload_path : files->download_path;
    size_t length = 0u;
    while (length < r->config.max_header_bytes && path[length] != '\0')
      ++length;
    if (length == 0u || length == r->config.max_header_bytes) {
      status = SALTS_EMSGSIZE;
      goto rejected;
    }
    op->path = tstr_dup_len(path, length);
    if (op->path == NULL) {
      status = SALTS_ENOMEM;
      goto rejected;
    }
    status = chttp_async_client_file_runtime(client, &op->file_runtime);
    if (status != SALTS_OK) goto rejected;
    if (op->file_mode == 2u) {
      status = chttp_file_temp_path(op->path, &op->temporary_path);
      if (status != SALTS_OK) goto rejected;
    }
    op->starting = true;
    op->live = true;
    *out = op->handle;
    return SALTS_OK;
  }
  status = logical_admit(op);
  if (status != SALTS_OK) goto rejected;
  op->live = true;
  *out = op->handle;
  return SALTS_OK;
rejected:
  logical_release(op);
  return status;
}

int chttp_async_client_submit_retry(chttp_async_client *client,
                                    const chttp_request_options *request,
                                    const chttp_request_timeouts *timeouts,
                                    const chttp_retry_options *retry,
                                    const chttp_request_replay *replay,
                                    chttp_logical_complete_fn complete, void *user,
                                    chttp_logical_request *out) {
  return logical_submit(client, request, timeouts, retry, replay, complete, user, out, NULL);
}

int chttp_async_client_submit_file_retry(chttp_async_client *client,
                                         const chttp_request_options *request,
                                         const chttp_request_timeouts *timeouts,
                                         const chttp_retry_options *retry,
                                         const chttp_retry_files *files,
                                         chttp_logical_complete_fn complete, void *user,
                                         chttp_logical_request *out) {
  if (files == NULL) {
    if (out != NULL) *out = (chttp_logical_request){0};
    return SALTS_EINVAL;
  }
  return logical_submit(client, request, timeouts, retry, NULL, complete, user, out, files);
}

static void logical_denied(chttp_logical_operation *op, const cnet_retry_result *decision) {
  if (decision->reason == CNET_RETRY_DEADLINE)
    logical_failure(op, SALTS_ETIMEDOUT, "retry-deadline");
  else if (decision->reason == CNET_RETRY_BUDGET &&
           op->budget.gate.attempts_used < op->budget.options.max_attempts)
    logical_failure(op, SALTS_ENOBUFS, "retry-byte-budget");
  else logical_deliver(op, NULL, &op->error);
}

static int logical_file_read(void *user, void *buffer, size_t capacity, size_t *size) {
  chttp_file_transfer *file = user;
  const chttp_file_source_result result = chttp_file_transfer_read(file, buffer, capacity, size);
  return result == CHTTP_FILE_SOURCE_WAIT    ? SALTS_EBUSY
         : result == CHTTP_FILE_SOURCE_ERROR ? chttp_file_transfer_status(file, NULL)
                                             : SALTS_OK;
}
static int logical_file_write(void *user, const void *data, size_t size) {
  chttp_file_sink_transfer *file = user;
  const chttp_file_sink_result result = chttp_file_sink_transfer_write(file, data, size);
  return result == CHTTP_FILE_SINK_WAIT    ? SALTS_EBUSY
         : result == CHTTP_FILE_SINK_ERROR ? chttp_file_sink_transfer_status(file, NULL)
                                           : SALTS_OK;
}

static int logical_files_open(chttp_logical_operation *op) {
  const chttp_client_config *config = &op->runtime->config;
  size_t chunk = config->stream_chunk_bytes;
  if (chunk == 0u)
    chunk = config->network.max_send_bytes < 65536u ? config->network.max_send_bytes : 65536u;
  int status;
  if (op->file_mode == 1u) {
    if (op->file_source.file.impl != NULL) return SALTS_OK;
    fs_stat_t stat;
    status = fs_stat(op->path, &stat);
    if (status != SALTS_OK) return status;
    if (!stat.is_file) return SALTS_EISDIR;
    if (stat.size > SIZE_MAX || stat.size > config->max_request_body_bytes) return SALTS_EMSGSIZE;
    if (op->source_stat_set &&
        (stat.size != op->source_stat.size || stat.mtime != op->source_stat.mtime ||
         stat.ctime != op->source_stat.ctime))
      return SALTS_EIO;
    op->source_stat = stat;
    op->source_stat_set = true;
    op->budget.gate.request_body_bytes = (size_t)stat.size;
    op->file_source = (chttp_file_transfer){0};
    status = chttp_file_transfer_open_read(&op->file_source, op->file_runtime, op->path,
                                           (size_t)stat.size, chunk, NULL, NULL);
    if (status != SALTS_OK) return status;
    op->source = (chttp_body_source){logical_file_read, &op->file_source, (size_t)stat.size, 1};
    op->request.body_source = &op->source;
  } else if (op->file_mode == 2u) {
    if (op->file_sink.file.impl != NULL) return SALTS_OK;
    size_t capacity;
    status = chttp_async_client_file_sink_capacity(&op->runtime->client, &capacity);
    if (status != SALTS_OK) return status;
    op->file_sink = (chttp_file_sink_transfer){0};
    status = chttp_file_sink_transfer_open(&op->file_sink, op->file_runtime, op->temporary_path,
                                           capacity, NULL, NULL);
    if (status != SALTS_OK) return status;
    op->temporary_created = true;
    if (!cflow_io_file_operation_supported(&op->file_sink.file, CFLOW_IO_NATIVE_FILE_FLUSH))
      return SALTS_ENOTSUP;
    op->sink = (chttp_body_sink){logical_file_write, &op->file_sink};
    op->request.body_sink = &op->sink;
  }
  return SALTS_OK;
}

/* Each pass makes bounded progress. Only a quiescent native file is destroyed;
 * the next attempt can never observe a previous attempt's read/write callback. */
static int logical_files_close(chttp_logical_operation *op) {
  int status;
  if (op->file_source.file.impl != NULL) {
    status = chttp_file_transfer_close(&op->file_source);
    if (status != SALTS_OK) return status;
    if (!cflow_io_file_is_quiescent(&op->file_source.file)) return SALTS_EBUSY;
    status = chttp_file_transfer_destroy(&op->file_source);
    if (status != SALTS_OK) return status;
  }
  if (op->file_sink.file.impl != NULL) {
    if (op->success && !op->canceled && !op->runtime->sealed && op->response.status_code >= 200u &&
        op->response.status_code < 300u && !op->file_sink.close_requested) {
      {
        const chttp_file_sink_result result = chttp_file_sink_transfer_flush(&op->file_sink);
        if (result == CHTTP_FILE_SINK_WAIT) return SALTS_EBUSY;
        if (result == CHTTP_FILE_SINK_ERROR) {
          op->success = false;
          int native_status = 0;
          const int status = chttp_file_sink_transfer_status(&op->file_sink, &native_status);
          op->error = (chttp_error){status, native_status, "file-sync"};
        }
      }
    }
    status = chttp_file_sink_transfer_close(&op->file_sink);
    if (status != SALTS_OK) return status;
    if (!cflow_io_file_is_quiescent(&op->file_sink.file)) return SALTS_EBUSY;
    status = chttp_file_sink_transfer_destroy(&op->file_sink);
    if (status != SALTS_OK) return status;
  }
  return SALTS_OK;
}

static int logical_files_discard(chttp_logical_operation *op) {
  if (!op->temporary_created) return SALTS_OK;
  const int status = fs_unlink(op->temporary_path);
  if (status == SALTS_OK) op->temporary_created = false;
  return status;
}

static void logical_file_result(chttp_logical_operation *op) {
  if (op->file_mode == 2u && op->response.status_code >= 200u && op->response.status_code < 300u) {
    const int status = fs_rename(op->temporary_path, op->path);
    if (status != SALTS_OK) {
      logical_failure(op, status, "file-commit");
      return;
    }
    op->temporary_created = false;
  }
  const chttp_response_view view = {.http_major = op->response.http_major,
                                    .http_minor = op->response.http_minor,
                                    .status_code = op->response.status_code,
                                    .reason = op->response.reason,
                                    .headers = op->response.headers,
                                    .header_count = op->response.header_count,
                                    .body = op->response.body,
                                    .body_size = op->response.body_size,
                                    .protocol_keep_alive = op->response.protocol_keep_alive};
  logical_deliver(op, &view, NULL);
}

int chttp_logical_runtime_progress(chttp_logical_runtime *r) {
  if (r == NULL) return SALTS_OK;
  for (size_t i = 0u; i < r->config.request_capacity; ++i) {
    chttp_logical_operation *op = &r->operations[i];
    if (!op->live || chttp_async_request_pending(&r->client, op->physical)) continue;
    const uint64_t now = monotonic_ms();
    const bool terminating = op->delivered || op->canceled || r->sealed ||
                             (!op->success && now >= op->budget.gate.deadline_ms);
    if (op->file_mode != 0u && ((!op->starting && !op->readmit) || terminating)) {
      const int closed = logical_files_close(op);
      if (closed == SALTS_EBUSY) continue;
      if (closed != SALTS_OK) {
        logical_failure(op, closed, "file-close");
        return closed;
      }
      if (op->success && !op->canceled && !r->sealed && !op->delivered) logical_file_result(op);
    }
    if (op->delivered) {
      const int discarded = logical_files_discard(op);
      if (discarded != SALTS_OK) return discarded;
      logical_release(op);
      continue;
    }
    if (op->canceled || r->sealed) {
      logical_failure(op, op->canceled ? SALTS_ECANCELED : SALTS_ESHUTDOWN,
                      op->canceled ? "cancel" : "shutdown");
      const int discarded = logical_files_discard(op);
      if (discarded != SALTS_OK) return discarded;
      logical_release(op);
      continue;
    }
    if (now >= op->budget.gate.deadline_ms) {
      logical_failure(op, SALTS_ETIMEDOUT, "retry-deadline");
      const int discarded = logical_files_discard(op);
      if (discarded != SALTS_OK) return discarded;
      logical_release(op);
      continue;
    }
    if (op->starting) {
      int status = logical_files_open(op);
      if (status == SALTS_OK) status = logical_admit(op);
      if (status != SALTS_OK && status != SALTS_ENOBUFS) {
        op->starting = false;
        logical_failure(op, status, "file-admission");
      }
      continue;
    }
    cnet_retry_result decision;
    int status;
    if (!op->prepared) {
      status = chttp_retry_budget_prepare_replay(
          &op->budget, &op->facts,
          (op->request.body_sink != NULL && op->replay.rollback_sink != NULL) ||
              op->file_mode == 2u,
          op->error.status, now, &decision);
      op->prepared = true;
      if (status != SALTS_OK || !decision.allowed) {
        if (status != SALTS_OK) logical_failure(op, status, "retry-policy");
        else logical_denied(op, &decision);
        const int discarded = logical_files_discard(op);
        if (discarded != SALTS_OK) return discarded;
        logical_release(op);
        continue;
      }
    }
    if (!op->readmit) {
      status = chttp_retry_budget_advance(&op->budget, now, &decision);
      if (status != SALTS_OK) {
        logical_failure(op, status, "retry-policy");
        const int discarded = logical_files_discard(op);
        if (discarded != SALTS_OK) return discarded;
        logical_release(op);
        continue;
      }
      if (!decision.allowed) {
        if (decision.reason == CNET_RETRY_WAIT_BACKOFF) continue;
        logical_denied(op, &decision);
        const int discarded = logical_files_discard(op);
        if (discarded != SALTS_OK) return discarded;
        logical_release(op);
        continue;
      }
      status = op->request.body_source != NULL && op->file_mode == 0u
                   ? chttp_async_client_replay_hook(&r->client, op->replay.rewind_source,
                                                    op->replay.user)
                   : SALTS_OK;
      if (status == SALTS_OK && op->request.body_sink != NULL && op->file_mode == 0u)
        status =
            chttp_async_client_replay_hook(&r->client, op->replay.rollback_sink, op->replay.user);
      if (status != SALTS_OK) {
        logical_failure(op, status, "retry-replay");
        logical_release(op);
        continue;
      }
      op->readmit = true;
    }
    if (op->file_mode != 0u) {
      status = logical_files_open(op);
      if (status != SALTS_OK) {
        logical_failure(op, status, "file-reopen");
        op->readmit = false;
        continue;
      }
    }
    status = logical_admit(op);
    if (status != SALTS_OK && status != SALTS_ENOBUFS) {
      logical_failure(op, status, "retry-admission");
      if (op->file_mode == 0u) logical_release(op);
      else op->readmit = false;
    }
  }
  return SALTS_OK;
}

uint32_t chttp_logical_runtime_wait(chttp_logical_runtime *r, uint32_t wait) {
  if (r == NULL) return wait;
  const uint64_t now = monotonic_ms();
  for (size_t i = 0u; i < r->config.request_capacity; ++i) {
    chttp_logical_operation *op = &r->operations[i];
    if (!op->live || op->delivered) continue;
    uint64_t next = op->budget.gate.deadline_ms;
    if (op->budget.waiting && op->budget.gate.backoff_not_before_ms < next)
      next = op->budget.gate.backoff_not_before_ms;
    const uint64_t remaining = next > now ? next - now : 0u;
    if (remaining < wait) wait = (uint32_t)remaining;
  }
  return wait;
}

int chttp_async_logical_request_cancel(chttp_async_client *client, chttp_logical_request handle) {
  chttp_logical_runtime *r;
  int status = chttp_async_client_logical_runtime(client, &r);
  if (status != SALTS_OK) return status;
  if (handle.slot == 0u || handle.slot > r->config.request_capacity) return SALTS_ENOENT;
  chttp_logical_operation *op = &r->operations[handle.slot - 1u];
  if (!op->live || op->generation != handle.generation) return SALTS_ENOENT;
  if (op->delivered || op->canceled) return SALTS_EALREADY;
  if (chttp_async_request_pending(client, op->physical) && !op->done) {
    status = chttp_async_request_cancel(client, op->physical);
    if (status != SALTS_OK) return status;
  }
  op->canceled = true;
  return SALTS_OK;
}

void chttp_logical_runtime_seal(chttp_logical_runtime *r) {
  if (r != NULL) r->sealed = true;
}
bool chttp_logical_runtime_empty(const chttp_logical_runtime *r) {
  if (r != NULL)
    for (size_t i = 0u; i < r->config.request_capacity; ++i)
      if (r->operations[i].live) return false;
  return true;
}
void chttp_logical_runtime_destroy(chttp_logical_runtime *r) {
  if (r == NULL) return;
  free(r->operations);
  free(r);
}
