#include "chttp_application_work.h"
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
  const char *content_type;
  const void *body;
  size_t size;
  int sealed;
};
struct work_item {
  work_impl *owner;
  int busy;
  size_t bytes;
  unsigned char *storage;
  chttp_server_request_view request;
  chttp_jwt_claims_view jwt;
  cnet_stream_peer peer;
  int64_t times[3];
  chttp_server_deferred deferred;
  chttp_application_work_task task;
  chttp_application_work_reply reply;
};
struct work_impl {
  cflow_executor executor;
  sync_mutex_t mutex;
  sync_cond_t settled;
  chttp_application_work_config config;
  chttp_application_work_stats stats;
  work_item *items;
  int accepting;
};

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
  if (r->session || r->body_streamed || r->body_sink_user) return SALTS_ENOTSUP;
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
  unsigned char *next = item->storage;
  chttp_header *headers = (chttp_header *)next;
  next += r->header_count * sizeof(*headers);
  chttp_server_param *params = (chttp_server_param *)next;
  next += r->param_count * sizeof(*params);
  const char **audiences = (const char **)next;
  if (r->jwt_claims) next += r->jwt_claims->audience_count * sizeof(*audiences);
  item->request = *r;
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
  if (!reply || !reply->item || code < 200u || code > 599u || !type || (size && !body))
    return SALTS_EINVAL;
  work_item *item = reply->item;
  if (!cflow_executor_is_current(&item->owner->executor)) return SALTS_EINVAL;
  if (reply->sealed) return SALTS_EALREADY;
  size_t bytes = size;
  const int status = add_string(&bytes, type);
  if (status != SALTS_OK) return status;
  if (bytes > item->owner->config.reply_bytes) return SALTS_EMSGSIZE;
  unsigned char *out = item->storage + item->bytes - item->owner->config.reply_bytes;
  reply->content_type = copy_string(&out, type);
  if (size) memcpy(out, body, size);
  reply->body = size ? out : NULL; reply->size = size;
  reply->status = code; reply->sealed = 1;
  return SALTS_OK;
}
static void publish(work_item *item, int business_status) {
  chttp_application_work_reply *draft = &item->reply;
  const int good = business_status == SALTS_OK && draft->sealed;
  const chttp_server_deferred_response response = {sizeof(response),
      good ? draft->status : 500u, good ? draft->content_type : "text/plain",
      NULL, 0u, good ? draft->body : "Internal Server Error", good ? draft->size : 21u};
  int status = chttp_server_deferred_reply(&item->deferred, &response);
  if (status != SALTS_OK && item->deferred.impl) {
    const int cancel_status = chttp_server_deferred_cancel(&item->deferred);
    if (cancel_status != SALTS_OK && cancel_status != SALTS_ENOENT && cancel_status != SALTS_EALREADY)
      status = cancel_status;
  }
  work_impl *impl = item->owner;
  sync_mutex_lock(&impl->mutex);
  if (status == SALTS_ENOENT || status == SALTS_EALREADY) count_event(&impl->stats.cancelled);
  else if (status != SALTS_OK && impl->stats.publication_status == SALTS_OK)
    impl->stats.publication_status = status;
  count_event(&impl->stats.completed);
  sync_mutex_unlock(&impl->mutex);
}
static void run_work(void *arg) {
  work_item *item = arg;
  const int status = item->task.run(item->task.user, &item->request, &item->reply);
  publish(item, status);
}
static void cancel_work(void *arg) {
  work_item *item = arg;
  publish(item, SALTS_ECANCELED);
}
static void release_work(void *arg) {
  work_item *item = arg;
  work_impl *impl = item->owner;
  if (item->task.finalize) item->task.finalize(item->task.user);
  free(item->storage);
  sync_mutex_lock(&impl->mutex);
  impl->stats.retained_bytes -= item->bytes;
  --impl->stats.pending;
  memset(item, 0, sizeof(*item));
  sync_cond_broadcast(&impl->settled);
  sync_mutex_unlock(&impl->mutex);
}
int chttp_application_work_init(chttp_application_work *owner,
    const chttp_application_work_config *config) {
  if (!owner || !config || !config->workers || !config->capacity ||
      !config->retained_bytes || !config->reply_bytes || config->workers > (size_t)INT_MAX ||
      config->reply_bytes > config->retained_bytes) return SALTS_EINVAL;
  if (owner->impl) return SALTS_EALREADY;
  if (config->capacity > SIZE_MAX / sizeof(work_item)) return SALTS_ERANGE;
  work_impl *impl = calloc(1u, sizeof(*impl));
  if (!impl) return SALTS_ENOMEM;
  impl->items = calloc(config->capacity, sizeof(*impl->items));
  if (!impl->items) { free(impl); return SALTS_ENOMEM; }
  impl->config = *config;
  if (!cflow_executor_worker_init_with_capacity(&impl->executor, config->workers, config->capacity)) {
    free(impl->items); free(impl); return SALTS_EIO;
  }
  sync_mutex_init(&impl->mutex); sync_cond_init(&impl->settled);
  impl->accepting = 1; owner->impl = impl;
  return SALTS_OK;
}
int chttp_application_work_submit(chttp_application_work *owner,
    const chttp_server_request_view *request, chttp_server_response *response,
    const chttp_application_work_task *task) {
  if (!owner || !owner->impl || !response || !task || !task->run) return SALTS_EINVAL;
  work_impl *impl = owner->impl;
  size_t bytes;
  int status = snapshot_size(request, &bytes);
  if (status != SALTS_OK) return status;
  status = add_bytes(&bytes, impl->config.reply_bytes);
  if (status != SALTS_OK) return status;
  work_item *item = NULL;
  sync_mutex_lock(&impl->mutex);
  if (!impl->accepting) status = SALTS_ESHUTDOWN;
  else if (bytes > impl->config.retained_bytes - impl->stats.retained_bytes) status = SALTS_ENOBUFS;
  else {
    for (size_t i = 0; i < impl->config.capacity; ++i) {
      if (!impl->items[i].busy) { item = &impl->items[i]; break; }
    }
    if (!item) status = SALTS_ENOBUFS;
  }
  if (status == SALTS_OK) {
    item->busy = 1; item->owner = impl; item->bytes = bytes;
    ++impl->stats.pending; impl->stats.retained_bytes += bytes;
  } else count_event(&impl->stats.rejected);
  sync_mutex_unlock(&impl->mutex);
  if (status != SALTS_OK) return status;
  item->storage = malloc(bytes);
  if (!item->storage) { release_work(item); return SALTS_ENOMEM; }
  snapshot_copy(item, request);
  item->reply.item = item;
  status = chttp_server_response_defer(response, &item->deferred);
  if (status != SALTS_OK) { release_work(item); return status; }
  item->task = *task;
  const cflow_executor_task descriptor = {.run = run_work, .cancel = cancel_work,
      .finalize = release_work, .user = item};
  sync_mutex_lock(&impl->mutex); count_event(&impl->stats.accepted); sync_mutex_unlock(&impl->mutex);
  const cflow_admission_status admitted = cflow_executor_try_post_task(&impl->executor, &descriptor);
  /* A successful post may run and finalize before the call returns. */
  if (admitted == CFLOW_ADMISSION_ACCEPTED) return SALTS_OK;
  /* A reserved item has a queue credit. Rejection is an invariant violation,
   * not ordinary ingress overload. Its already sealed response must settle. */
  sync_mutex_lock(&impl->mutex);
  impl->accepting = 0;
  if (impl->stats.publication_status == SALTS_OK) impl->stats.publication_status = SALTS_EPROTO;
  sync_mutex_unlock(&impl->mutex);
  item->task.finalize = NULL; /* A rejected descriptor never takes its caller payload. */
  publish(item, SALTS_EPROTO); release_work(item);
  return SALTS_EPROTO;
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
