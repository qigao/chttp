#include "chttp_connection_recovery.h"
#include <core/clock.h>
#include <limits.h>
#include <string.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>
#endif

int chttp_connection_recovery_validate(const chttp_connection_recovery_options *options) {
  if (options == NULL || options->size != sizeof(*options) || options->max_attempts == 0u ||
      options->initial_backoff_ms > options->maximum_backoff_ms ||
      (options->max_attempts > 1u && options->episode_timeout_ms == 0u))
    return SALTS_EINVAL;
  return SALTS_OK;
}

static cnet_reconnect_failure_kind recovery_classify(void *user,
    cnet_connection_state state, const cnet_error *error) {
  chttp_connection_recovery *r = user;
  bool transient = false;
  bool refused = false;
  if (r->permanent || r->stopping) return CNET_RECONNECT_PERMANENT;
  if (error != NULL && (error->status == SALTS_EAUTH ||
      error->tls.reason != CRYPTO_TLS_FAILURE_NONE)) return CNET_RECONNECT_SECURITY;
  if (error == NULL && state == CNET_CONNECTION_CLOSED)
    return r->tls && !r->transport_connected ? CNET_RECONNECT_PERMANENT : CNET_RECONNECT_TRANSIENT;
  if (error != NULL) {
    /* Native completion failures may have a generic Salts EIO status. CNet's
     * classifier contract preserves the native error precisely for this
     * explicit whitelist; unknown I/O errors are permanent. */
    if (error->status == SALTS_EIO) {
      const int64_t native = error->native_status;
      const uint64_t code = (uint64_t)(native < 0 ? -native : native);
#if defined(_WIN32)
      switch (code) {
      case WSAECONNREFUSED: case ERROR_CONNECTION_REFUSED:
        refused = true; transient = true; break;
      case WSAECONNRESET: case WSAECONNABORTED: case WSAETIMEDOUT: case ERROR_CONNECTION_ABORTED:
      case ERROR_NETNAME_DELETED: case ERROR_SEM_TIMEOUT:
        transient = true; break;
      default: break;
      }
#else
      switch (code) {
      case ECONNREFUSED: refused = true; transient = true; break;
      case ECONNRESET: case ECONNABORTED: case ETIMEDOUT: transient = true; break;
      default: break;
      }
#endif
    }
    switch (error->status) {
    case SALTS_ECONNREFUSED: refused = true; transient = true; break;
    case SALTS_ECONNRESET: case SALTS_ECONNABORTED: case SALTS_ETIMEDOUT:
      transient = true; break;
    default: break;
    }
  }
  /* Before verified TLS CONNECTED, an undiagnosed reset/timeout can be a
   * rejected SNI/identity/handshake. Only a proven TCP refusal is retryable. */
  if (r->tls && !r->transport_connected && !refused) return CNET_RECONNECT_PERMANENT;
  return transient ? CNET_RECONNECT_TRANSIENT : CNET_RECONNECT_PERMANENT;
}

static void recovery_state(void *user, cnet_connection connection,
    cnet_connection_state state, const cnet_error *error) {
  chttp_connection_recovery *r = user;
  cnet_managed_dial_snapshot snapshot;
  cnet_error deadline_error;
  const int status = cnet_managed_dial_get_snapshot(&r->dial, &snapshot);
  r->in_callback = true;
  if (status != SALTS_OK) {
    r->last_status = status;
    r->permanent = true;
    r->fail(r->user, status, 0);
    r->in_callback = false;
    return;
  }
  *r->managed = snapshot.managed;
  *r->connection = connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    r->ticket = snapshot.recovery_ticket;
    r->transport_connected = true;
  }
  if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    r->terminal = true;
    r->failed_after_ready = r->ready;
    r->ready = false;
    if (r->permanent && r->last_status == SALTS_ETIMEDOUT) {
      deadline_error = error != NULL ? *error : (cnet_error){0};
      deadline_error.status = SALTS_ETIMEDOUT;
      deadline_error.native_status = r->last_native_status;
      deadline_error.stage = "connection-recovery";
      error = &deadline_error;
      /* Native close has really settled. Report the episode failure to the
       * HTTP observer instead of letting it interpret this as response EOF. */
      state = CNET_CONNECTION_FAILED;
    } else {
      r->last_status = error != NULL ? error->status : SALTS_ECONNRESET;
      r->last_native_status = error != NULL ? error->native_status : 0;
    }
    if (r->pooled->slot != 0u && r->pool_bound)
      (void)cnet_pool_begin_drain(r->pool, *r->pooled);
  }
  r->observer.on_state(r->observer.user, connection, state, error);
  r->in_callback = false;
}

static void recovery_receive(void *user, cnet_connection connection,
    const cnet_receive_view *view) {
  chttp_connection_recovery *r = user;
  r->in_callback = true;
  r->observer.on_receive(r->observer.user, connection, view);
  r->in_callback = false;
}
static void recovery_send(void *user, cnet_connection connection, size_t size) {
  chttp_connection_recovery *r = user;
  r->in_callback = true;
  r->observer.on_send(r->observer.user, connection, size);
  r->in_callback = false;
}

static int recovery_admit(void *user) {
  chttp_connection_recovery *r = user;
  int status;
  if (r->stopping || r->permanent) return SALTS_ESHUTDOWN;
  if (r->terminal && !r->settled(r->user)) return SALTS_EBUSY;
  if (r->terminal && r->pool_bound) {
    status = cnet_pool_terminal(r->pool, *r->pooled);
    if (status != SALTS_OK) return status;
    *r->pooled = (cnet_pool_connection){0};
    r->pool_bound = false;
  }
  if (r->pooled->slot == 0u) {
    status = cnet_pool_reserve_connecting(r->pool, &r->key, r->pooled);
    if (status != SALTS_OK) return status;
  }
  if (r->terminal) {
    status = r->reset(r->user);
    if (status != SALTS_OK) return status;
  }
  r->terminal = false;
  r->transport_connected = false;
  r->failed_after_ready = false;
  r->failure_delivered = false;
  return SALTS_OK;
}

int chttp_connection_recovery_init(chttp_connection_recovery *r,
    const chttp_connection_recovery_options *options, cnet_client *client,
    cnet_manager *manager, cnet_client_pool *pool, const cnet_pool_key *key,
    cnet_pool_connection *pooled, cnet_managed_connection *managed, cnet_connection *connection,
    uint64_t deadline_at_ms, const cnet_connect_options *connect, int (*reset)(void *), bool (*settled)(void *),
    void (*fail)(void *, int, int), void *user) {
  const uint64_t now = monotonic_ms();
  cnet_managed_dial_config config = {0};
  int status = chttp_connection_recovery_validate(options);
  if (status != SALTS_OK) return status;
  if (UINT64_MAX - now < options->episode_timeout_ms) return SALTS_ERANGE;
  if (deadline_at_ms != 0u && deadline_at_ms <= now) return SALTS_ETIMEDOUT;
  *r = (chttp_connection_recovery){.manager = manager, .pool = pool, .key = *key,
      .pooled = pooled, .managed = managed, .connection = connection,
      .observer = connect->observer, .reset = reset, .settled = settled, .fail = fail,
      .user = user, .configured = true,
      .tls = connect->tls_client != NULL || connect->tls != NULL ||
             strncmp(connect->uri, "tls://", sizeof("tls://") - 1u) == 0};
  config.size = sizeof(config);
  config.version = CNET_MANAGED_DIAL_VERSION;
  config.manager = manager;
  config.client = client;
  config.connection = *connect;
  config.connection.observer = (cnet_observer){.on_state = recovery_state,
      .on_receive = recovery_receive, .on_send = recovery_send, .user = r};
  config.recovery = (cnet_reconnect_config){.size = sizeof(config.recovery),
      .version = CNET_RECOVERY_POLICY_VERSION, .max_attempts = options->max_attempts,
      .deadline_ms = now + options->episode_timeout_ms,
      .initial_backoff_ms = options->initial_backoff_ms,
      .maximum_backoff_ms = options->maximum_backoff_ms, .jitter_seed = options->jitter_seed};
  config.recovery_episode_ms = options->episode_timeout_ms;
  if (deadline_at_ms != 0u && deadline_at_ms < config.recovery.deadline_ms)
    config.recovery.deadline_ms = deadline_at_ms;
  config.classify = recovery_classify;
  config.classify_user = r;
  status = cnet_managed_dial_init_admitted(&r->dial, &config, recovery_admit, r);
  if (status != SALTS_OK) return status;
  return chttp_connection_recovery_progress(r);
}

bool chttp_connection_recovery_opening_retry(chttp_connection_recovery *r) {
  cnet_managed_dial_snapshot s;
  return !r->stopping && !r->permanent && !r->failed_after_ready &&
      cnet_managed_dial_get_snapshot(&r->dial, &s) == SALTS_OK &&
      !s.recovery.sealed && s.recovery.attempts < s.recovery.max_attempts &&
      monotonic_ms() < s.recovery.deadline_ms;
}

int chttp_connection_recovery_ready(chttp_connection_recovery *r) {
  const int status = cnet_managed_dial_protocol_ready(&r->dial, r->ticket, monotonic_ms());
  if (status == SALTS_OK) r->ready = true;
  return status;
}
int chttp_connection_recovery_bind(chttp_connection_recovery *r, size_t capacity) {
  const int status = cnet_pool_bind_ready(r->pool, *r->pooled, *r->managed, capacity);
  if (status == SALTS_OK) r->pool_bound = true;
  return status;
}
int chttp_connection_recovery_seal(chttp_connection_recovery *r) {
  r->stopping = true;
  r->permanent = true;
  r->defer_stop = false;
  if (r->in_callback) return SALTS_OK;
  return r->dial.impl != NULL ? cnet_managed_dial_seal(&r->dial) : SALTS_OK;
}

int chttp_connection_recovery_progress(chttp_connection_recovery *r) {
  cnet_managed_dial_snapshot s;
  uint64_t wait = 0u;
  int status;
  if (r->dial.impl == NULL) return SALTS_OK;
  status = cnet_managed_dial_get_snapshot(&r->dial, &s);
  if (status != SALTS_OK) return status;
  *r->managed = s.managed;
  if (r->defer_stop && s.managed.slot != 0u && !r->terminal) return SALTS_OK;
  if (!r->stopping && !r->ready && monotonic_ms() >= s.recovery.deadline_ms) {
    r->last_status = SALTS_ETIMEDOUT;
    r->permanent = true;
  }
  if (r->permanent || s.recovery.sealed ||
      (r->terminal && s.recovery.attempts >= s.recovery.max_attempts)) {
    if (!r->failure_delivered && (r->terminal || s.managed.slot == 0u)) {
      r->failure_delivered = true;
      r->fail(r->user, r->last_status != SALTS_OK ? r->last_status : SALTS_ESHUTDOWN,
              r->last_native_status);
    }
    status = chttp_connection_recovery_seal(r);
    if (status != SALTS_OK) return status;
    if (s.managed.slot != 0u || !r->settled(r->user)) return SALTS_OK;
    if (r->pooled->slot != 0u) {
      status = cnet_pool_terminal(r->pool, *r->pooled);
      if (status != SALTS_OK) return status;
      *r->pooled = (cnet_pool_connection){0};
    }
    return cnet_managed_dial_destroy(&r->dial);
  }
  status = cnet_managed_dial_advance(&r->dial, monotonic_ms(), &wait);
  if (status != SALTS_OK && status != SALTS_EBUSY) {
    r->last_status = status;
    r->permanent = true;
    return status;
  }
  status = cnet_managed_dial_get_snapshot(&r->dial, &s);
  if (status == SALTS_OK) {
    *r->managed = s.managed;
    *r->connection = s.connection;
  }
  return status;
}

uint32_t chttp_connection_recovery_wait(chttp_connection_recovery *r, uint32_t wait_ms) {
  cnet_managed_dial_snapshot s;
  uint64_t due, now, remaining;
  if (r->dial.impl == NULL || r->stopping || r->ready ||
      cnet_managed_dial_get_snapshot(&r->dial, &s) != SALTS_OK) return wait_ms;
  now = monotonic_ms();
  due = s.recovery.deadline_ms;
  if (s.managed.slot == 0u && s.recovery.next_attempt_ms < due) due = s.recovery.next_attempt_ms;
  remaining = due > now ? due - now : 0u;
  return remaining < wait_ms ? (uint32_t)remaining : wait_ms;
}
