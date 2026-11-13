#include "chttp_application_work.h"
#include "../../http_server/src/chttp_server_engine.h"
#include "../../http_common/http/chttp_response_fields.h"
#include <cflow/executor.h>
#include <threads/threads.h>
#include <core/clock.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct work_impl work_impl;
typedef struct work_item work_item;
struct chttp_application_work_reply {
  work_item *item;
  unsigned int status;
  chttp_header content_type;
  chttp_header *headers;
  char *header_storage;
  size_t header_count, header_used, header_wire, reply_used;
  const void *body;
  const char *file_path;
  uint64_t file_offset;
  size_t file_length;
  bool file_bounded;
  size_t size;
  int sealed;
  int finalized;
};
struct work_item {
  work_impl *owner;
  int busy;
  int business_done;
  int transport_done;
  int completion_queued;
  int session_done, gate_owned, business_waiting, session_stale;
  size_t session_bytes;
  chttp_session_work *session;
  const void *session_key;
  work_item *wait_next;
  chttp_server_response_terminal terminal;
  chttp_application_work_outcome outcome;
  size_t bytes;
  size_t storage_bytes, method_capacity, method_count;
  chttp_method_completion_record *methods;
  unsigned char *header_block;
  unsigned char *storage;
  chttp_server_request_view request;
  chttp_jwt_claims_view jwt;
  cnet_stream_peer peer;
  int64_t times[3];
  chttp_server_deferred deferred;
  chttp_application_work_task task;
  chttp_application_work_chain_hook hooks[CHTTP_APPLICATION_WORK_MAX_HOOKS];
  chttp_application_work_reply reply;
  chttp_server_owner_waker waker;
  chttp_body_source source;
  void (*source_cleanup)(void *, int);
  unsigned char *source_chunk;
  size_t source_capacity, source_request, source_size, source_offset;
  int source_status, source_close_status;
  bool source_selected, source_requested, source_ready, source_active;
  bool source_closed, source_cleaned, source_cleaning;
  chttp_server_admission_gate admission;
  chttp_application_admission_fn admission_fn;
  void *admission_user;
  int admission_status;
  chttp_server_admission_result admission_result;
};
struct work_impl {
  cflow_executor executor;
  sync_mutex_t mutex;
  sync_cond_t settled;
  chttp_application_work_config config;
  chttp_application_work_stats stats;
  work_item *items;
  size_t header_reservation;
  int accepting;
  work_item *waiting_head, *waiting_tail;
};
int chttp_application_work_reply_current(const chttp_application_work_reply *reply) {
  return reply && reply->item && cflow_executor_is_current(&reply->item->owner->executor);
}
int chttp_application_work_cancelled(const chttp_application_work_reply *reply, bool *out) {
  if (!out || !chttp_application_work_reply_current(reply)) return SALTS_EINVAL;
  work_item *item = reply->item;
  sync_mutex_lock(&item->owner->mutex);
  *out = item->transport_done && item->outcome.transport_status != SALTS_OK;
  sync_mutex_unlock(&item->owner->mutex);
  return SALTS_OK;
}

static void count_event(uint64_t *count) { if (*count != UINT64_MAX) ++*count; }

static int add_bytes(size_t *total, size_t bytes) {
  if (bytes > SIZE_MAX - *total) return SALTS_ERANGE;
  *total += bytes;
  return SALTS_OK;
}
static int add_array(size_t *total, size_t count, size_t element) {
  if (count > SIZE_MAX / element) return SALTS_ERANGE;
  return add_bytes(total, count * element);
}
static int add_string(size_t *total, const char *s) {
  if (s == NULL) return SALTS_OK;
  const size_t len = strlen(s);
  if (len == SIZE_MAX) return SALTS_ERANGE;
  return add_bytes(total, len + 1u);
}
static char *copy_string(unsigned char **next, const char *s) {
  if (s == NULL) return NULL;
  const size_t bytes = strlen(s) + 1u;
  char *out = (char *)*next;
  memcpy(out, s, bytes); *next += bytes;
  return out;
}
static int snapshot_size(const chttp_server_request_view *r, size_t *bytes) {
  int status;
  *bytes = 0u;
  if (r == NULL || (r->header_count && !r->headers) || (r->param_count && !r->params) ||
      (r->body_size && !r->body)) return SALTS_EINVAL;
  /* No live owner-affine application state is smuggled into the worker. */
  if (r->body_streamed || r->body_sink_user) return SALTS_ENOTSUP;
#define ADD(expr) do { status = (expr); if (status != SALTS_OK) return status; } while (0)
  ADD(add_array(bytes, r->header_count, sizeof(chttp_header)));
  ADD(add_array(bytes, r->param_count, sizeof(chttp_server_param)));
  const chttp_jwt_claims_view *jwt = r->jwt_claims;
  if (jwt) {
    if (jwt->audience_count && !jwt->audiences) return SALTS_EINVAL;
    ADD(add_array(bytes, jwt->audience_count, sizeof(char *)));
    ADD(add_string(bytes, jwt->issuer)); ADD(add_string(bytes, jwt->subject));
    ADD(add_string(bytes, jwt->jwt_id));
    for (size_t i = 0; i < jwt->audience_count; ++i) ADD(add_string(bytes, jwt->audiences[i]));
  }
  ADD(add_string(bytes, r->target)); ADD(add_string(bytes, r->path));
  ADD(add_string(bytes, r->peer_certificate_sha256)); ADD(add_bytes(bytes, r->body_size));
  for (size_t i = 0; i < r->header_count; ++i) {
    if (!r->headers[i].name || !r->headers[i].value) return SALTS_EINVAL;
    ADD(add_string(bytes, r->headers[i].name)); ADD(add_string(bytes, r->headers[i].value));
  }
  for (size_t i = 0; i < r->param_count; ++i) {
    if (!r->params[i].name || !r->params[i].value) return SALTS_EINVAL;
    ADD(add_string(bytes, r->params[i].name)); ADD(add_string(bytes, r->params[i].value));
  }
#undef ADD
  return SALTS_OK;
}
static void snapshot_copy(work_item *item, const chttp_server_request_view *r) {
  unsigned char *next = item->storage + item->session_bytes;
  chttp_header *headers = (chttp_header *)next;
  next += r->header_count * sizeof(*headers);
  chttp_server_param *params = (chttp_server_param *)next;
  next += r->param_count * sizeof(*params);
  const char **audiences = (const char **)next;
  if (r->jwt_claims) next += r->jwt_claims->audience_count * sizeof(*audiences);
  item->request = *r;
  item->request.session = chttp_session_work_view(item->session);
  item->request.headers = r->header_count ? headers : NULL;
  item->request.params = r->param_count ? params : NULL;
  item->request.target = copy_string(&next, r->target);
  item->request.path = copy_string(&next, r->path);
  item->request.peer_certificate_sha256 = copy_string(&next, r->peer_certificate_sha256);
  item->request.body = r->body_size ? next : NULL;
  if (r->body_size) memcpy(next, r->body, r->body_size);
  next += r->body_size;
  for (size_t i = 0; i < r->header_count; ++i) {
    headers[i].name = copy_string(&next, r->headers[i].name);
    headers[i].value = copy_string(&next, r->headers[i].value);
  }
  for (size_t i = 0; i < r->param_count; ++i) {
    params[i].name = copy_string(&next, r->params[i].name);
    params[i].value = copy_string(&next, r->params[i].value);
  }
  if (r->peer) { item->peer = *r->peer; item->request.peer = &item->peer; }
  if (r->jwt_claims) {
    const chttp_jwt_claims_view *j = r->jwt_claims;
    item->jwt = *j;
    item->jwt.issuer = copy_string(&next, j->issuer);
    item->jwt.subject = copy_string(&next, j->subject);
    item->jwt.jwt_id = copy_string(&next, j->jwt_id);
    item->jwt.audiences = j->audience_count ? audiences : NULL;
    for (size_t i = 0; i < j->audience_count; ++i) audiences[i] = copy_string(&next, j->audiences[i]);
    const int64_t *source[3] = {j->issued_at, j->not_before, j->expires_at};
    const int64_t **target[3] = {&item->jwt.issued_at, &item->jwt.not_before, &item->jwt.expires_at};
    for (size_t i = 0; i < 3u; ++i) {
      *target[i] = NULL;
      if (source[i]) { item->times[i] = *source[i]; *target[i] = &item->times[i]; }
    }
    item->request.jwt_claims = &item->jwt;
  }
}
int chttp_application_work_respond(chttp_application_work_reply *reply, unsigned int code,
    const char *type, const void *body, size_t size) {
  if (!reply || !reply->item || code < 200u || code > 599u ||
      (type && !type[0]) || (size && !body))
    return SALTS_EINVAL;
  if ((code == 204u || code == 205u || code == 304u) && size) return SALTS_EINVAL;
  if (type) {
    chttp_response_field_size measured;
    const int status = chttp_response_field_measure("Content-Type", type, false, &measured);
    if (status != SALTS_OK) return status;
  }
  work_item *item = reply->item;
  if (!cflow_executor_is_current(&item->owner->executor)) return SALTS_EINVAL;
  if (reply->sealed || reply->finalized) return SALTS_EALREADY;
  size_t bytes = size;
  const int status = add_string(&bytes, type);
  if (status != SALTS_OK) return status;
  if (bytes > item->owner->config.reply_bytes - reply->reply_used) return SALTS_EMSGSIZE;
  unsigned char *out = item->storage + item->storage_bytes - item->owner->config.reply_bytes + reply->reply_used;
  const char *content_type = copy_string(&out, type);
  if (size) memcpy(out, body, size);
  if (type) reply->headers[0] = (chttp_header){"Content-Type", content_type};
  reply->reply_used += bytes;
  reply->body = size ? out : NULL; reply->size = size;
  reply->status = code; reply->sealed = 1;
  return SALTS_OK;
}
typedef struct header_budget { size_t count, used, wire; } header_budget;
/* Reserve one non-Content-Type upsert against a virtual budget. Multi-field
 * callers supply distinct names so existing rows remain the lookup fact source. */
static int reserve_header(const chttp_application_work_reply *reply, const char *name,
    const chttp_response_field_size *measured, header_budget *budget, size_t *slot) {
  const chttp_application_work_config *limits = &reply->item->owner->config;
  size_t index = 0u, previous = 0u;
  for (; index < reply->header_count; ++index) {
    const chttp_header *field = &reply->headers[index + 1u];
    if (chttp_response_ascii_equal(field->name, name)) {
      previous = strlen(field->name) + strlen(field->value) + 4u; break;
    }
  }
  const bool replacing = index < reply->header_count;
  if ((!replacing && budget->count == limits->header_capacity) ||
      measured->wire > limits->header_bytes - (budget->wire - previous)) return SALTS_ENOBUFS;
  const size_t bytes = measured->value + 1u + (replacing ? 0u : measured->name + 1u);
  if (bytes > limits->header_bytes - budget->used) return SALTS_ENOBUFS;
  budget->used += bytes; budget->wire = budget->wire - previous + measured->wire;
  if (!replacing) ++budget->count;
  *slot = index;
  return SALTS_OK;
}
int chttp_application_work_set_header(chttp_application_work_reply *reply, const char *name, const char *value) {
  if (!chttp_application_work_reply_current(reply)) return SALTS_EINVAL;
  chttp_response_field_size measured;
  const int status = chttp_response_field_measure(name, value, false, &measured);
  if (status != SALTS_OK) return status;
  if (reply->finalized) return SALTS_EALREADY;
  work_item *item = reply->item;
  const chttp_application_work_config *limits = &item->owner->config;
  if (chttp_response_ascii_equal(name, "Content-Type")) {
    const size_t bytes = measured.value + 1u;
    if (bytes > limits->reply_bytes - reply->reply_used) return SALTS_EMSGSIZE;
    unsigned char *next = item->storage + item->storage_bytes - limits->reply_bytes + reply->reply_used;
    const char *copy = copy_string(&next, value);
    reply->headers[0] = (chttp_header){"Content-Type", copy}; reply->reply_used += bytes;
    return SALTS_OK;
  }
  size_t index;
  header_budget budget = {reply->header_count, reply->header_used, reply->header_wire};
  const int reserved = reserve_header(reply, name, &measured, &budget, &index);
  if (reserved != SALTS_OK) return reserved;
  const bool replacing = index < reply->header_count;
  unsigned char *next = (unsigned char *)reply->header_storage + reply->header_used;
  const char *name_copy = replacing ? reply->headers[index + 1u].name : copy_string(&next, name);
  const char *value_copy = copy_string(&next, value);
  reply->headers[index + 1u] = (chttp_header){name_copy, value_copy};
  reply->header_used = budget.used; reply->header_wire = budget.wire; reply->header_count = budget.count;
  return SALTS_OK;
}
int chttp_application_work_response_file(chttp_application_work_reply *reply, unsigned int code,
    const char *type, const char *path) {
  if (!path || !path[0]) return SALTS_EINVAL;
  size_t bytes = 0u;
  int status = add_string(&bytes, path);
  if (status != SALTS_OK) return status;
  status = chttp_application_work_respond(reply, code, type, path, bytes);
  if (status == SALTS_OK) {
    reply->file_path = reply->body; reply->body = NULL; reply->size = 0u;
  }
  return status;
}
int chttp_application_work_response_source(chttp_application_work_reply *reply, unsigned int code,
    const char *type, const chttp_body_source *source, void (*cleanup)(void *, int), size_t chunk_bytes) {
  if (!chttp_application_work_reply_current(reply) || !source || !source->read || !chunk_bytes ||
      (source->content_length_known != 0 && source->content_length_known != 1) ||
      code == 204u || code == 205u || code == 304u) return SALTS_EINVAL;
  if (reply->sealed || reply->finalized) return SALTS_EALREADY;
  size_t bytes = chunk_bytes;
  const int measured = add_string(&bytes, type);
  if (measured != SALTS_OK) return measured;
  work_item *item = reply->item;
  if (bytes > item->owner->config.reply_bytes - reply->reply_used) return SALTS_EMSGSIZE;
  const int status = chttp_application_work_respond(reply, code, type, NULL, 0u);
  if (status != SALTS_OK) return status;
  item->source = *source; item->source_cleanup = cleanup;
  item->source_capacity = chunk_bytes;
  item->source_chunk = item->storage + item->storage_bytes - item->owner->config.reply_bytes + reply->reply_used;
  reply->reply_used += chunk_bytes;
  sync_mutex_lock(&item->owner->mutex); item->source_selected = true; sync_mutex_unlock(&item->owner->mutex);
  return SALTS_OK;
}
/* Planner fields are unique and exclude Content-Type. Simulate their shared
 * append/upsert arena costs before changing any row; commit uses set_header. */
static int file_headers_fit(const chttp_application_work_reply *reply, const chttp_server_file_plan *plan) {
  header_budget budget = {reply->header_count, reply->header_used, reply->header_wire};
  for (size_t i = 0u; i < plan->header_count; ++i) {
    const chttp_header *field = &plan->headers[i];
    chttp_response_field_size measured;
    const int status = chttp_response_field_measure(field->name, field->value, false, &measured);
    if (status != SALTS_OK) return status;
    size_t slot;
    const int reserved = reserve_header(reply, field->name, &measured, &budget, &slot);
    if (reserved != SALTS_OK) return reserved;
  }
  return SALTS_OK;
}
int chttp_application_work_serve_file(chttp_application_work_reply *reply, const chttp_server_file_options *options) {
  if (!chttp_application_work_reply_current(reply)) return SALTS_EINVAL;
  if (reply->sealed || reply->finalized) return SALTS_EALREADY;
  chttp_server_file_plan plan;
  int status = chttp_server_file_plan_make(&reply->item->request, options, &plan);
  if (status != SALTS_OK) return status;
  status = file_headers_fit(reply, &plan);
  if (status != SALTS_OK) return status;
  size_t bytes = 0u;
  if (plan.send_file) {
    status = add_string(&bytes, options->path);
    if (status != SALTS_OK) return status;
  }
  /* respond validates/copies path and MIME before mutation. Every metadata
   * row was preflighted above; no allocation or external call follows sealing. */
  status = chttp_application_work_respond(reply, plan.status_code, plan.content_type,
      plan.send_file ? options->path : NULL, bytes);
  if (status != SALTS_OK) return status;
  if (plan.send_file) {
    reply->file_path = reply->body; reply->body = NULL; reply->size = 0u;
    reply->file_offset = plan.offset; reply->file_length = plan.length; reply->file_bounded = true;
  }
  for (size_t i = 0u; i < plan.header_count; ++i) {
    status = chttp_application_work_set_header(reply, plan.headers[i].name, plan.headers[i].value);
    if (status != SALTS_OK) return status;
  }
  return SALTS_OK;
}
static int reserve_method(void *context, chttp_method_completion_record **out) {
  work_item *item = context;
  if (!item || !cflow_executor_is_current(&item->owner->executor)) return SALTS_EINVAL;
  if (item->method_count == item->method_capacity) return SALTS_ENOBUFS;
  *out = &item->methods[item->method_count++];
  **out = (chttp_method_completion_record){0};
  return SALTS_OK;
}
chttp_method_completion_sink chttp_application_work_method_sink(chttp_application_work_reply *reply) {
  return (chttp_method_completion_sink){reply ? reply->item : NULL, reserve_method};
}
static void file_prepared(void *context, int status) {
  work_item *item = context;
  if (status == SALTS_OK) return;
  /* Normal per-request I/O failure, not a broken executor publication. */
  sync_mutex_lock(&item->owner->mutex);
  item->outcome.publication_status = status;
  sync_mutex_unlock(&item->owner->mutex);
}
static void source_progress(work_item *);
static bool source_native_ready(void *user) {
  work_item *item = user;
  sync_mutex_lock(&item->owner->mutex);
  const bool ready = item->source_ready && !item->source_active;
  sync_mutex_unlock(&item->owner->mutex);
  return ready;
}
static int source_native_read(void *user, void *buffer, size_t capacity, size_t *out) {
  work_item *item = user;
  *out = 0u;
  sync_mutex_lock(&item->owner->mutex);
  if (item->source_ready && !item->source_active) {
    const int status = item->source_status;
    if (status == SALTS_OK) {
      const size_t remaining = item->source_size - item->source_offset;
      *out = remaining < capacity ? remaining : capacity;
      if (*out) memcpy(buffer, item->source_chunk + item->source_offset, *out);
      item->source_offset += *out;
      if (item->source_size && item->source_offset == item->source_size) item->source_ready = false;
    }
    sync_mutex_unlock(&item->owner->mutex);
    return status;
  }
  item->source_requested = true;
  item->source_request = capacity < item->source_capacity ? capacity : item->source_capacity;
  sync_mutex_unlock(&item->owner->mutex);
  source_progress(item);
  return CHTTP_SERVER_WORKER_SOURCE_WAIT;
}
static void source_native_cleanup(void *user, int status) {
  work_item *item = user;
  sync_mutex_lock(&item->owner->mutex);
  if (!item->source_closed) { item->source_closed = true; item->source_close_status = status; }
  sync_mutex_unlock(&item->owner->mutex);
  source_progress(item);
}
static void publish(work_item *item, int business_status) {
  chttp_application_work_reply *draft = &item->reply;
  const int good = (business_status == SALTS_OK || item->session_stale) && draft->sealed;
  draft->finalized = 1;
  const size_t first = draft->headers[0].name ? 0u : 1u;
  const size_t header_count = good ? draft->header_count + (first == 0u) : 0u;
  chttp_session_work_seal(item->session, business_status == SALTS_OK && draft->sealed);
  const chttp_server_deferred_response response = {sizeof(response),
      good ? draft->status : 500u, good ? NULL : "text/plain",
      header_count ? draft->headers + first : NULL, header_count,
      good ? draft->body : "Internal Server Error", good ? draft->size : 21u};
  const chttp_server_deferred_file file = {.path = draft->file_path, .status_code = draft->status,
      .prepared = file_prepared, .user = item, .offset = draft->file_offset,
      .length = draft->file_length, .bounded = draft->file_bounded};
  const chttp_server_worker_source source = {{source_native_read, item,
      item->source.content_length, item->source.content_length_known}, source_native_ready, source_native_cleanup};
  int status = good && item->source_selected ?
      chttp_server_deferred_response_source(&item->deferred, draft->status, response.headers, response.header_count, &source) :
      good && draft->file_path ?
      chttp_server_deferred_response_file(&item->deferred, response.headers, response.header_count, &file) :
      chttp_server_deferred_reply(&item->deferred, &response);
  if (status != SALTS_OK && item->deferred.impl) {
    const int cancel_status = chttp_server_deferred_cancel(&item->deferred);
    if (cancel_status != SALTS_OK && cancel_status != SALTS_ENOENT && cancel_status != SALTS_EALREADY)
      status = cancel_status;
  }
  work_impl *impl = item->owner;
  sync_mutex_lock(&impl->mutex);
  if (item->source_selected && (!good || status != SALTS_OK)) {
    item->source_closed = true;
    item->source_close_status = business_status != SALTS_OK ? business_status : status;
  }
  /* The owner may have already recorded file preparation failure after READY. */
  if (status != SALTS_OK) item->outcome.publication_status = status;
  if (status != SALTS_OK && status != SALTS_ENOENT && status != SALTS_EALREADY &&
      impl->stats.publication_status == SALTS_OK) impl->stats.publication_status = status;
  sync_cond_broadcast(&impl->settled);
  sync_mutex_unlock(&impl->mutex);
}
int chttp_application_work_reject(const chttp_application_work_exchange *exchange,
    unsigned int code, const char *type, const void *body, size_t size) {
  if (!exchange || !exchange->result || code < 400u || code > 599u) return SALTS_EINVAL;
  const int status = chttp_application_work_respond(exchange->response, code, type, body, size);
  if (status == SALTS_OK) { exchange->result->status = SALTS_OK; exchange->result->rejected = true; }
  return status;
}
static cmeta_status invoke_user(void *arg, const chttp_application_work_exchange *exchange,
    chttp_application_work_result *result) {
  work_item *item = arg;
  result->status = item->task.run(item->task.user, exchange->request, exchange->response);
  if (result->status == SALTS_OK && !item->reply.sealed) result->status = SALTS_EPROTO;
  return result->status == SALTS_OK ? CMETA_OK : CMETA_CALLBACK_ERROR;
}
static void run_work(void *arg) {
  work_item *item = arg;
  sync_mutex_lock(&item->owner->mutex);
  const int cancelled = item->transport_done && item->outcome.transport_status != SALTS_OK;
  sync_mutex_unlock(&item->owner->mutex);
  if (cancelled) {
    item->outcome.business_status = SALTS_ECANCELED;
    publish(item, SALTS_ECANCELED);
    return;
  }
  const int begun = chttp_session_work_begin(item->session);
  if (begun != SALTS_OK) {
    item->outcome.business_status = begun;
    if (begun == SALTS_ENOENT) {
      item->session_stale = 1;
      (void)chttp_application_work_respond(&item->reply, 409u, "text/plain", "Session changed", 15u);
    }
    publish(item, begun);
    return;
  }
  chttp_application_work_result result = {SALTS_OK, false};
  const chttp_application_work_exchange exchange = {&item->request, &item->reply, &result};
  const chttp_application_work_chain chain = {item, invoke_user, item->hooks, item->task.hook_count};
  const cmeta_status invoked = chttp_application_work_chain_invoke(&chain, &exchange, &result);
  const int status = result.rejected || invoked == CMETA_OK ? result.status :
      result.status != SALTS_OK ? result.status : SALTS_EIO;
  item->outcome.business_status = status; item->outcome.rejected = result.rejected;
  publish(item, status);
}
static void cancel_work(void *arg) {
  work_item *item = arg;
  item->outcome.business_status = SALTS_ECANCELED;
  publish(item, SALTS_ECANCELED);
}
static void release_work(void *arg) {
  work_item *item = arg;
  work_impl *impl = item->owner;
  if (item->task.finalize) item->task.finalize(item->task.user);
  chttp_session_work_release(item->session);
  free(item->header_block);
  free(item->methods);
  free(item->storage);
  sync_mutex_lock(&impl->mutex);
  impl->stats.retained_bytes -= item->bytes;
  --impl->stats.pending;
  memset(item, 0, sizeof(*item));
  sync_cond_broadcast(&impl->settled);
  sync_mutex_unlock(&impl->mutex);
}
static void notify_completion(void *arg) {
  work_item *item = arg;
  chttp_server_request_view request = item->request;
  request.session = NULL;
  for (size_t i = 0u; i < item->method_count; ++i) {
    const chttp_method_completion_record *record = &item->methods[i];
    const chttp_method_completion completion = {&request, item->request.session,
        record->terminal, item->outcome};
    for (size_t j = record->entered; j != 0u; --j) {
      const chttp_method_hook *hook = &record->hooks[j - 1u];
      if (hook->after_completion) hook->after_completion(hook->context, &completion);
    }
  }
  if (item->task.after_completion)
    item->task.after_completion(item->task.user, &item->request, &item->outcome);
  work_impl *impl = item->owner;
  sync_mutex_lock(&impl->mutex);
  count_event(&impl->stats.completed);
  if (item->outcome.transport_status != SALTS_OK) count_event(&impl->stats.cancelled);
  sync_mutex_unlock(&impl->mutex);
}
static void completion_cancelled(void *arg) {
  /* The pool stays open until every admitted terminal notification settles. */
  work_item *item = arg;
  sync_mutex_lock(&item->owner->mutex);
  if (item->owner->stats.publication_status == SALTS_OK)
    item->owner->stats.publication_status = SALTS_EPROTO;
  sync_mutex_unlock(&item->owner->mutex);
}
static void business_finalized(void *arg);
static void source_finalized(void *arg);
static void invariant_failure(work_impl *impl) {
  sync_mutex_lock(&impl->mutex);
  impl->accepting = 0;
  if (impl->stats.publication_status == SALTS_OK) impl->stats.publication_status = SALTS_EPROTO;
  sync_cond_broadcast(&impl->settled);
  sync_mutex_unlock(&impl->mutex);
}
static cflow_admission_status post_business(work_item *item) {
  const cflow_executor_task task = {.run = run_work, .cancel = cancel_work,
      .finalize = business_finalized, .user = item};
  return cflow_executor_try_post_task(&item->owner->executor, &task);
}
static void source_run(void *arg) {
  work_item *item = arg;
  sync_mutex_lock(&item->owner->mutex);
  const int close_status = item->source_close_status;
  sync_mutex_unlock(&item->owner->mutex);
  if (item->source_cleaning) {
    if (item->source_cleanup) item->source_cleanup(item->source.user, close_status);
    return;
  }
  sync_mutex_lock(&item->owner->mutex);
  const bool closed = item->source_closed;
  const size_t capacity = item->source_request;
  sync_mutex_unlock(&item->owner->mutex);
  size_t bytes = 0u;
  int status = closed ? SALTS_ECANCELED : item->source.read(item->source.user, item->source_chunk, capacity, &bytes);
  if (status > SALTS_OK) status = SALTS_EPROTO;
  if (status == SALTS_OK && bytes > capacity) status = SALTS_EPROTO;
  sync_mutex_lock(&item->owner->mutex);
  item->source_status = status; item->source_size = bytes; item->source_offset = 0u;
  sync_mutex_unlock(&item->owner->mutex);
}
static void source_cancelled(void *arg) {
  work_item *item = arg;
  sync_mutex_lock(&item->owner->mutex);
  item->source_status = SALTS_ECANCELED;
  sync_mutex_unlock(&item->owner->mutex);
  invariant_failure(item->owner); /* Admitted cleanup must never be dropped. */
}
static void unqueue_waiter(work_impl *impl, work_item *item) {
  work_item **cursor = &impl->waiting_head, *previous = NULL;
  while (*cursor && *cursor != item) { previous = *cursor; cursor = &(*cursor)->wait_next; }
  if (*cursor) {
    *cursor = item->wait_next;
    if (impl->waiting_tail == item) impl->waiting_tail = previous;
    item->wait_next = NULL; item->business_waiting = 0;
    --impl->stats.session_waiting;
  }
}
enum work_event { WORK_BUSINESS_DONE, WORK_TRANSPORT_DONE, WORK_SESSION_DONE, WORK_SOURCE_PROGRESS, WORK_SOURCE_DONE };
static void join_completion(work_item *item, enum work_event event, int status) {
  work_impl *impl = item->owner;
  work_item *launch = NULL;
  int notify = 0;
  bool source_post = false;
  chttp_server_owner_waker waker = {0};
  sync_mutex_lock(&impl->mutex);
  if (event == WORK_BUSINESS_DONE) item->business_done = 1;
  else if (event == WORK_SESSION_DONE) {
    item->session_done = 1; item->outcome.session_status = status;
  } else if (event == WORK_TRANSPORT_DONE) {
    item->transport_done = 1; item->outcome.transport_status = status;
    if (item->business_waiting) {
      unqueue_waiter(impl, item); launch = item; /* canceled: skip all user code */
    }
  }
  if (event == WORK_SOURCE_DONE) {
    item->source_active = false;
    if (item->source_cleaning) item->source_cleaned = true;
    else {
      item->source_requested = false;
      item->source_ready = true;
      waker = item->waker;
    }
  }
  if (item->business_done && item->session_done && item->gate_owned) {
    /* Lock order is worker -> Session. Owner commits drop the Session mutex
     * before signaling this join. Retire the pin before another record can
     * reuse the logical gate identity, and before completion can free storage. */
    chttp_session_work_release(item->session);
    item->gate_owned = 0;
    for (work_item *next = impl->waiting_head; next; next = next->wait_next) {
      if (next->session_key == item->session_key) {
        unqueue_waiter(impl, next); next->gate_owned = 1; launch = next; break;
      }
    }
  }
  if (item->business_done && item->source_selected && !item->source_active && !item->source_cleaned &&
      (item->source_closed || item->source_requested)) {
    item->source_active = true; item->source_cleaning = item->source_closed; source_post = true;
  }
  if (item->business_done && item->transport_done && (!item->session || item->session_done) &&
      (!item->source_selected || item->source_cleaned) && !item->source_active &&
      !item->completion_queued) {
    item->completion_queued = 1; notify = 1;
  }
  sync_mutex_unlock(&impl->mutex);
  /* All tail operations use captured owners or an exclusively reserved stage.
   * A successful post may finish/recycle the record before returning. */
  if (waker.impl) (void)chttp_server_owner_waker_signal(&waker);
  if (source_post) {
    const cflow_executor_task task = {.run = source_run, .cancel = source_cancelled,
        .finalize = source_finalized, .user = item};
    if (cflow_executor_try_post_task(&impl->executor, &task) != CFLOW_ADMISSION_ACCEPTED) invariant_failure(impl);
  }
  /* Each record has at most one queued stage. The gate adds no executor queue
   * or worker-blocking wait. Never access a record after successful posting. */
  if (launch && post_business(launch) != CFLOW_ADMISSION_ACCEPTED) invariant_failure(impl);
  if (!notify) return;
  const cflow_executor_task task = {.run = notify_completion, .cancel = completion_cancelled,
      .finalize = release_work, .user = item};
  if (cflow_executor_try_post_task(&impl->executor, &task) != CFLOW_ADMISSION_ACCEPTED)
    invariant_failure(impl); /* retain unsettled ownership; never inline or drop */
}
static void business_finalized(void *arg) { join_completion(arg, WORK_BUSINESS_DONE, SALTS_OK); }
static void source_progress(work_item *item) { join_completion(item, WORK_SOURCE_PROGRESS, SALTS_OK); }
static void source_finalized(void *arg) { join_completion(arg, WORK_SOURCE_DONE, SALTS_OK); }
static void response_terminal(void *arg, int status) { join_completion(arg, WORK_TRANSPORT_DONE, status); }
static void session_committed(void *arg, int status) { join_completion(arg, WORK_SESSION_DONE, status); }

int chttp_application_work_init(chttp_application_work *owner,
    const chttp_application_work_config *config) {
  if (!owner || !config || !config->workers || !config->capacity ||
      !config->retained_bytes || !config->reply_bytes || config->workers > (size_t)INT_MAX ||
      config->reply_bytes > config->retained_bytes) return SALTS_EINVAL;
  if (owner->impl) return SALTS_EALREADY;
  if ((!config->header_capacity) != (!config->header_bytes)) return SALTS_EINVAL;
  size_t header_reservation = 0u;
  if (config->header_capacity) {
    if (config->header_capacity == SIZE_MAX) return SALTS_ERANGE;
    int status = add_array(&header_reservation, config->header_capacity + 1u, sizeof(chttp_header));
    if (status == SALTS_OK) status = add_bytes(&header_reservation, config->header_bytes);
    if (status != SALTS_OK) return status;
    if (header_reservation > config->retained_bytes - config->reply_bytes) return SALTS_EINVAL;
  }
  if (config->capacity > SIZE_MAX / sizeof(work_item)) return SALTS_ERANGE;
  work_impl *impl = calloc(1u, sizeof(*impl));
  if (!impl) return SALTS_ENOMEM;
  impl->items = calloc(config->capacity, sizeof(*impl->items));
  if (!impl->items) { free(impl); return SALTS_ENOMEM; }
  impl->config = *config; impl->header_reservation = header_reservation;
  if (!cflow_executor_worker_init_with_capacity(&impl->executor, config->workers, config->capacity)) {
    free(impl->items); free(impl); return SALTS_EIO;
  }
  sync_mutex_init(&impl->mutex); sync_cond_init(&impl->settled);
  impl->accepting = 1; owner->impl = impl;
  return SALTS_OK;
}
static int reserve_work(work_impl *impl, size_t bytes, work_item **out) {
  work_item *item = NULL;
  int status = SALTS_OK;
  sync_mutex_lock(&impl->mutex);
  if (!impl->accepting) status = SALTS_ESHUTDOWN;
  else if (bytes > impl->config.retained_bytes - impl->stats.retained_bytes) status = SALTS_ENOBUFS;
  else {
    for (size_t i = 0u; i < impl->config.capacity; ++i)
      if (!impl->items[i].busy) { item = &impl->items[i]; break; }
    if (!item) status = SALTS_ENOBUFS;
  }
  if (status == SALTS_OK) {
    item->busy = 1; item->owner = impl; item->bytes = bytes;
    ++impl->stats.pending; impl->stats.retained_bytes += bytes;
    *out = item;
  } else count_event(&impl->stats.rejected);
  sync_mutex_unlock(&impl->mutex);
  return status;
}
static void run_admission(void *context) {
  work_item *item = context;
  bool canceled = false;
  int status = chttp_server_admission_gate_cancelled(&item->admission, &canceled);
  if (status == SALTS_OK)
    status = canceled ? SALTS_ECANCELED : item->admission_fn(item->admission_user, &item->request, &item->admission_result);
  item->admission_status = status;
}
static void cancel_admission(void *context) { ((work_item *)context)->admission_status = SALTS_ECANCELED; }
static void finalize_admission(void *context) {
  work_item *item = context;
  work_impl *impl = item->owner;
  chttp_server_admission_gate gate = item->admission;
  const chttp_server_admission_result result = item->admission_result;
  const int status = item->admission_status;
  sync_mutex_lock(&impl->mutex);
  count_event(&impl->stats.completed);
  if (status == SALTS_ECANCELED) count_event(&impl->stats.cancelled);
  sync_mutex_unlock(&impl->mutex);
  /* No input/frame/user state remains borrowed when the native owner resumes.
   * Record reuse is valid during this finalizer tail; never access item again. */
  release_work(item);
  if (chttp_server_admission_gate_complete(&gate, status, &result) != SALTS_OK)
    invariant_failure(impl);
}
int chttp_application_work_admit(chttp_application_work *owner, const chttp_server_request_view *request,
    chttp_server_admission_gate *gate, chttp_application_admission_fn fn, void *user) {
  if (!owner || !owner->impl || !request || !gate || !gate->impl || !gate->generation || !fn ||
      request->session || request->body || request->body_size || request->body_streamed)
    return SALTS_EINVAL;
  size_t bytes = 0u;
  int status = snapshot_size(request, &bytes);
  if (status != SALTS_OK) return status;
  work_impl *impl = owner->impl;
  work_item *item = NULL;
  status = reserve_work(impl, bytes, &item);
  if (status != SALTS_OK) return status;
  item->storage = malloc(bytes);
  if (!item->storage) { release_work(item); return SALTS_ENOMEM; }
  item->storage_bytes = bytes; snapshot_copy(item, request);
  item->admission = *gate; *gate = (chttp_server_admission_gate){0};
  item->admission_fn = fn; item->admission_user = user;
  const cflow_executor_task task = {.run = run_admission, .cancel = cancel_admission,
      .finalize = finalize_admission, .user = item};
  sync_mutex_lock(&impl->mutex); count_event(&impl->stats.accepted); sync_mutex_unlock(&impl->mutex);
  if (cflow_executor_try_post_task(&impl->executor, &task) != CFLOW_ADMISSION_ACCEPTED) {
    *gate = item->admission; release_work(item); invariant_failure(impl); return SALTS_EPROTO;
  }
  /* Accepted post can finalize/reuse item before returning. */
  return SALTS_OK;
}

int chttp_application_work_submit(chttp_application_work *owner,
    const chttp_server_request_view *request, chttp_server_response *response,
    const chttp_application_work_task *task) {
  if (!owner || !owner->impl || !response || !task || !task->run) return SALTS_EINVAL;
  if (task->hook_count > CHTTP_APPLICATION_WORK_MAX_HOOKS ||
      (task->hook_count && !task->hooks)) return SALTS_EINVAL;
  for (size_t i = 0u; i < task->hook_count; ++i) {
    if (!task->hooks[i].before && !task->hooks[i].after && !task->hooks[i].on_error)
      return SALTS_EINVAL;
  }
  work_impl *impl = owner->impl;
  size_t bytes;
  int status = snapshot_size(request, &bytes);
  if (status != SALTS_OK) return status;
  size_t session_bytes = 0u;
  if (request->session) {
    status = chttp_session_work_size(request->session, &session_bytes);
    if (status != SALTS_OK) return status;
  }
  status = add_bytes(&bytes, session_bytes);
  if (status != SALTS_OK) return status;
  status = add_bytes(&bytes, impl->config.reply_bytes);
  if (status != SALTS_OK) return status;
  const size_t storage_bytes = bytes;
  status = add_array(&bytes, task->method_capacity, sizeof(chttp_method_completion_record));
  if (status != SALTS_OK) return status;
  status = add_bytes(&bytes, impl->header_reservation);
  if (status != SALTS_OK) return status;
  work_item *item = NULL;
  status = reserve_work(impl, bytes, &item);
  if (status != SALTS_OK) return status;
  item->session_bytes = session_bytes;
  item->storage_bytes = storage_bytes; item->method_capacity = task->method_capacity;
  item->storage = malloc(storage_bytes);
  if (!item->storage) { release_work(item); return SALTS_ENOMEM; }
  item->reply.headers = &item->reply.content_type;
  if (impl->header_reservation) {
    item->header_block = malloc(impl->header_reservation);
    if (!item->header_block) { release_work(item); return SALTS_ENOMEM; }
    item->reply.headers = (chttp_header *)item->header_block;
    item->reply.headers[0] = (chttp_header){0};
    item->reply.header_storage = (char *)(item->reply.headers + impl->config.header_capacity + 1u);
  }
  if (task->method_capacity) {
    item->methods = calloc(task->method_capacity, sizeof(*item->methods));
    if (!item->methods) { release_work(item); return SALTS_ENOMEM; }
  }
  if (request->session) {
    status = chttp_session_work_prepare(item->storage, session_bytes, request->session,
        session_committed, item, &item->session);
    if (status != SALTS_OK) { release_work(item); return status; }
    item->session_key = chttp_session_work_key(item->session);
  }
  snapshot_copy(item, request);
  item->reply.item = item;
  status = chttp_server_response_waker(response, &item->waker);
  if (status != SALTS_OK) { release_work(item); return status; }
  item->terminal = (chttp_server_response_terminal){.complete = response_terminal, .user = item};
  status = chttp_server_response_observe(response, &item->terminal);
  if (status != SALTS_OK) { release_work(item); return status; }
  status = chttp_server_response_defer_worker(response, &item->deferred, item->session);
  if (status != SALTS_OK) {
    const int detached = chttp_server_response_unobserve(response, &item->terminal);
    if (detached == SALTS_OK) { release_work(item); return status; }
    sync_mutex_lock(&impl->mutex); impl->accepting = 0;
    impl->stats.publication_status = SALTS_EPROTO;
    sync_cond_broadcast(&impl->settled); sync_mutex_unlock(&impl->mutex);
    return SALTS_EPROTO;
  }
  item->task = *task;
  if (task->hook_count) memcpy(item->hooks, task->hooks, task->hook_count * sizeof(*task->hooks));
  item->task.hooks = item->hooks;
  sync_mutex_lock(&impl->mutex);
  count_event(&impl->stats.accepted);
  if (item->session_key) {
    for (size_t i = 0u; i < impl->config.capacity; ++i)
      if (impl->items[i].gate_owned && impl->items[i].session_key == item->session_key) {
        item->business_waiting = 1; break;
      }
    if (item->business_waiting) {
      if (impl->waiting_tail) impl->waiting_tail->wait_next = item;
      else impl->waiting_head = item;
      impl->waiting_tail = item;
      ++impl->stats.session_waiting;
    } else item->gate_owned = 1;
  }
  const int waiting = item->business_waiting;
  sync_mutex_unlock(&impl->mutex);
  if (waiting) return SALTS_OK;
  const cflow_admission_status admitted = post_business(item);
  /* A successful post may run and finalize before the call returns. */
  if (admitted == CFLOW_ADMISSION_ACCEPTED) return SALTS_OK;
  /* A reserved item has a queue credit. Rejection is an invariant violation,
   * not ordinary ingress overload. Its already sealed response must settle. */
  sync_mutex_lock(&impl->mutex);
  impl->accepting = 0;
  if (impl->stats.publication_status == SALTS_OK) impl->stats.publication_status = SALTS_EPROTO;
  sync_mutex_unlock(&impl->mutex);
  item->task.finalize = NULL; /* A rejected descriptor never takes its caller payload. */
  item->task.after_completion = NULL;
  item->outcome.business_status = SALTS_EPROTO;
  publish(item, SALTS_EPROTO);
  business_finalized(item);
  return SALTS_EPROTO;
}
int chttp_application_work_is_current(const chttp_application_work *owner) {
  const work_impl *impl = owner ? owner->impl : NULL;
  return impl && cflow_executor_is_current((cflow_executor *)&impl->executor);
}
int chttp_application_work_admission_response(chttp_server_response *response, int status) {
  if (status == SALTS_ENOBUFS || status == SALTS_ESHUTDOWN)
    return chttp_server_reply(response, 503u, "text/plain", "Service Unavailable", 19u);
  if (status == SALTS_ENOENT)
    return chttp_server_reply(response, 409u, "text/plain", "Session changed", 15u);
  return status;
}
int chttp_application_work_get_stats(chttp_application_work *owner,
    chttp_application_work_stats *out) {
  if (!owner || !owner->impl || !out) return SALTS_EINVAL;
  work_impl *impl = owner->impl;
  sync_mutex_lock(&impl->mutex); *out = impl->stats; sync_mutex_unlock(&impl->mutex);
  return SALTS_OK;
}
static int wait_work(work_impl *impl, uint32_t timeout_ms) {
  if (cflow_executor_is_current(&impl->executor)) return SALTS_EBUSY;
  const uint64_t start = monotonic_ms();
  sync_mutex_lock(&impl->mutex);
  while (impl->stats.pending) {
    /* A known publication failure must not turn an infinite drain into a hang.
     * Return its original error while retaining every still-live record. */
    if (impl->stats.publication_status != SALTS_OK) {
      const int status = impl->stats.publication_status;
      sync_mutex_unlock(&impl->mutex); return status;
    }
    const uint64_t elapsed = monotonic_ms() - start;
    if (timeout_ms && elapsed >= timeout_ms) {
      sync_mutex_unlock(&impl->mutex); return SALTS_ETIMEDOUT;
    }
    if (timeout_ms) {
      const uint64_t left = timeout_ms - elapsed;
      const int status = sync_cond_timedwait(
          &impl->settled, &impl->mutex, left * UINT64_C(1000000));
      if (status != 0 && status != -ETIMEDOUT) {
        sync_mutex_unlock(&impl->mutex); return SALTS_EIO;
      }
    } else sync_cond_wait(&impl->settled, &impl->mutex);
  }
  sync_mutex_unlock(&impl->mutex);
  /* Last descriptor may still be returning after releasing the last item. */
  return cflow_executor_wait_idle(&impl->executor) ? SALTS_OK : SALTS_EBUSY;
}
int chttp_application_work_wait(chttp_application_work *owner, uint32_t timeout_ms) {
  if (!owner || !owner->impl) return SALTS_EINVAL;
  return wait_work(owner->impl, timeout_ms);
}
int chttp_application_work_close(chttp_application_work *owner, uint32_t timeout_ms) {
  if (!owner) return SALTS_EINVAL;
  if (!owner->impl) return SALTS_OK;
  work_impl *impl = owner->impl;
  if (cflow_executor_is_current(&impl->executor)) return SALTS_EBUSY;
  sync_mutex_lock(&impl->mutex); impl->accepting = 0; sync_mutex_unlock(&impl->mutex);
  const int waited = wait_work(impl, timeout_ms);
  if (waited != SALTS_OK) return waited;
  const int result = impl->stats.publication_status;
  if (!cflow_executor_shutdown(&impl->executor)) return SALTS_EBUSY;
  cflow_executor_destroy(&impl->executor);
  sync_cond_destroy(&impl->settled); sync_mutex_destroy(&impl->mutex);
  free(impl->items); free(impl); owner->impl = NULL;
  return result;
}
