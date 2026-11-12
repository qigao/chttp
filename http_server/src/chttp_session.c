#include "chttp_server_runtime.h"

#include <core/clock.h>
#include <core/platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CHTTP_SESSION_ID_RANDOM_BYTES = 16,
  CHTTP_SESSION_ID_TEXT_BYTES = 32,
  CHTTP_SESSION_ID_ATTEMPTS = 8,
  CHTTP_SESSION_COOKIE_BYTES = 256
};

static bool chttp_session_multiply(size_t left, size_t right, size_t *out) {
  if (out == NULL || (right != 0u && left > SIZE_MAX / right)) return false;
  *out = left * right;
  return true;
}

static uint32_t chttp_session_generation_next(uint32_t generation) {
  ++generation;
  return generation == 0u ? 1u : generation;
}

static void chttp_session_entries_clear(const chttp_server_impl *server,
                                        chttp_session_entry *entries) {
  size_t index;
  if (server == NULL || entries == NULL) return;
  for (index = 0u; index < server->config.session_entry_capacity; ++index) {
    entries[index].used = false;
    entries[index].key[0] = '\0';
    entries[index].value[0] = '\0';
  }
}

static void chttp_session_record_clear(const chttp_server_impl *server,
                                       chttp_session_record *record) {
  if (server == NULL || record == NULL) return;
  chttp_session_entries_clear(server, record->entries);
  record->id[0] = '\0';
  record->expires_at_ms = 0u;
  record->generation = chttp_session_generation_next(record->generation);
  record->used = false;
}

static void chttp_session_snapshot_clear(chttp_session_context *context) {
  if (context == NULL || context->server == NULL ||
      context->snapshot_entries == NULL)
    return;
  chttp_session_entries_clear(context->server, context->snapshot_entries);
}

static void chttp_session_snapshot_from_record(
    chttp_session_context *context, const chttp_session_record *record) {
  size_t index;
  if (context == NULL || context->server == NULL ||
      context->snapshot_entries == NULL || record == NULL)
    return;
  chttp_session_snapshot_clear(context);
  for (index = 0u; index < context->server->config.session_entry_capacity;
       ++index) {
    const chttp_session_entry *source = &record->entries[index];
    chttp_session_entry *target = &context->snapshot_entries[index];
    if (!source->used) continue;
    memcpy(target->key, source->key, strlen(source->key) + 1u);
    memcpy(target->value, source->value, strlen(source->value) + 1u);
    target->used = true;
  }
}

static void chttp_session_record_from_snapshot(
    const chttp_session_context *context, chttp_session_record *record) {
  size_t index;
  if (context == NULL || context->server == NULL ||
      context->snapshot_entries == NULL || record == NULL)
    return;
  chttp_session_entries_clear(context->server, record->entries);
  for (index = 0u; index < context->server->config.session_entry_capacity;
       ++index) {
    const chttp_session_entry *source = &context->snapshot_entries[index];
    chttp_session_entry *target = &record->entries[index];
    if (!source->used) continue;
    memcpy(target->key, source->key, strlen(source->key) + 1u);
    memcpy(target->value, source->value, strlen(source->value) + 1u);
    target->used = true;
  }
}

int chttp_session_context_init(chttp_session_context *context,
                               chttp_server_impl *server) {
  size_t key_stride;
  size_t value_stride;
  size_t key_bytes;
  size_t value_bytes;
  size_t index;
  if (context == NULL || server == NULL) return SALTS_EINVAL;
  *context = (chttp_session_context){.server = server};
  if (server->config.session_capacity == 0u) return SALTS_OK;

  key_stride = server->config.max_session_key_bytes + 1u;
  value_stride = server->config.max_session_value_bytes + 1u;
  if (key_stride == 0u || value_stride == 0u ||
      server->config.session_entry_capacity >
          SIZE_MAX / sizeof(*context->snapshot_entries) ||
      !chttp_session_multiply(server->config.session_entry_capacity,
                              key_stride, &key_bytes) ||
      !chttp_session_multiply(server->config.session_entry_capacity,
                              value_stride, &value_bytes))
    return SALTS_ERANGE;

  context->snapshot_entries = (chttp_session_entry *)calloc(
      server->config.session_entry_capacity, sizeof(*context->snapshot_entries));
  context->snapshot_keys = (char *)calloc(key_bytes, 1u);
  context->snapshot_values = (char *)calloc(value_bytes, 1u);
  if (context->snapshot_entries == NULL || context->snapshot_keys == NULL ||
      context->snapshot_values == NULL) {
    chttp_session_context_destroy(context);
    return SALTS_ENOMEM;
  }
  for (index = 0u; index < server->config.session_entry_capacity; ++index) {
    context->snapshot_entries[index].key =
        context->snapshot_keys + index * key_stride;
    context->snapshot_entries[index].value =
        context->snapshot_values + index * value_stride;
  }
  return SALTS_OK;
}

void chttp_session_context_reset(chttp_session_context *context) {
  chttp_server_impl *server;
  chttp_session_entry *entries;
  char *keys;
  char *values;
  if (context == NULL) return;
  server = context->server;
  entries = context->snapshot_entries;
  keys = context->snapshot_keys;
  values = context->snapshot_values;
  if (server != NULL && entries != NULL)
    chttp_session_entries_clear(server, entries);
  *context = (chttp_session_context){
      .server = server,
      .snapshot_entries = entries,
      .snapshot_keys = keys,
      .snapshot_values = values};
}

void chttp_session_context_destroy(chttp_session_context *context) {
  if (context == NULL) return;
  free(context->snapshot_values);
  free(context->snapshot_keys);
  free(context->snapshot_entries);
  *context = (chttp_session_context){0};
}

int chttp_session_store_init(chttp_server_impl *server) {
  size_t entry_count;
  size_t key_stride;
  size_t value_stride;
  size_t key_bytes;
  size_t value_bytes;
  size_t record_index;
  size_t entry_index;
  if (server == NULL) return SALTS_EINVAL;
  if (server->config.session_capacity == 0u) return SALTS_OK;
  key_stride = server->config.max_session_key_bytes + 1u;
  value_stride = server->config.max_session_value_bytes + 1u;
  if (key_stride == 0u || value_stride == 0u ||
      !chttp_session_multiply(server->config.session_capacity,
                              server->config.session_entry_capacity, &entry_count) ||
      !chttp_session_multiply(entry_count, key_stride, &key_bytes) ||
      !chttp_session_multiply(entry_count, value_stride, &value_bytes) ||
      server->config.session_capacity > SIZE_MAX / sizeof(*server->sessions) ||
      entry_count > SIZE_MAX / sizeof(*server->session_entries))
    return SALTS_ERANGE;
  server->sessions =
      (chttp_session_record *)calloc(server->config.session_capacity,
                                     sizeof(*server->sessions));
  server->session_entries =
      (chttp_session_entry *)calloc(entry_count, sizeof(*server->session_entries));
  server->session_keys = (char *)calloc(key_bytes, 1u);
  server->session_values = (char *)calloc(value_bytes, 1u);
  if (server->sessions == NULL || server->session_entries == NULL ||
      server->session_keys == NULL || server->session_values == NULL) {
    chttp_session_store_destroy(server);
    return SALTS_ENOMEM;
  }
  for (record_index = 0u; record_index < server->config.session_capacity;
       ++record_index) {
    chttp_session_record *record = &server->sessions[record_index];
    record->entries =
        server->session_entries +
        record_index * server->config.session_entry_capacity;
    for (entry_index = 0u;
         entry_index < server->config.session_entry_capacity; ++entry_index) {
      const size_t flat =
          record_index * server->config.session_entry_capacity + entry_index;
      record->entries[entry_index].key =
          server->session_keys + flat * key_stride;
      record->entries[entry_index].value =
          server->session_values + flat * value_stride;
    }
  }
  sync_mutex_init(&server->session_mutex);
  server->session_sync_initialized = true;
  return SALTS_OK;
}

void chttp_session_store_destroy(chttp_server_impl *server) {
  if (server == NULL) return;
  if (server->session_sync_initialized) {
    sync_mutex_destroy(&server->session_mutex);
    server->session_sync_initialized = false;
  }
  free(server->session_values);
  free(server->session_keys);
  free(server->session_entries);
  free(server->sessions);
  server->session_values = NULL;
  server->session_keys = NULL;
  server->session_entries = NULL;
  server->sessions = NULL;
}

static uint64_t chttp_session_expiry(const chttp_server_impl *server,
                                     uint64_t now_ms) {
  if ((uint64_t)server->config.session_idle_timeout_ms > UINT64_MAX - now_ms)
    return UINT64_MAX;
  return now_ms + (uint64_t)server->config.session_idle_timeout_ms;
}

static void chttp_session_expire_locked(chttp_server_impl *server,
                                        uint64_t now_ms) {
  size_t index;
  for (index = 0u; index < server->config.session_capacity; ++index) {
    chttp_session_record *record = &server->sessions[index];
    if (record->used && !record->worker_pins && record->expires_at_ms <= now_ms)
      chttp_session_record_clear(server, record);
  }
}

static bool chttp_session_hex_id(const char *value, size_t size) {
  size_t index;
  if (value == NULL || size != CHTTP_SESSION_ID_TEXT_BYTES) return false;
  for (index = 0u; index < size; ++index) {
    const unsigned char ch = (unsigned char)value[index];
    if (!((ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
          (ch >= (unsigned char)'a' && ch <= (unsigned char)'f') ||
          (ch >= (unsigned char)'A' && ch <= (unsigned char)'F')))
      return false;
  }
  return true;
}

static const char *chttp_session_cookie_value(
    const chttp_server_impl *server,
    const chttp_server_request_view *request,
    size_t *out_size) {
  const char *cookie = chttp_server_request_header(request, "Cookie");
  const size_t expected_name_size = strlen(server->session_cookie_name);
  if (out_size != NULL) *out_size = 0u;
  while (cookie != NULL && *cookie != '\0') {
    const char *name;
    const char *equals;
    const char *value;
    const char *end;
    const char *delimiter;
    while (*cookie == ' ' || *cookie == '\t' || *cookie == ';')
      ++cookie;
    name = cookie;
    equals = strchr(name, '=');
    end = strchr(name, ';');
    if (equals == NULL || (end != NULL && equals > end)) {
      cookie = end == NULL ? NULL : end + 1u;
      continue;
    }
    value = equals + 1u;
    delimiter = strchr(value, ';');
    end = delimiter == NULL ? value + strlen(value) : delimiter;
    while (equals != name && (equals[-1] == ' ' || equals[-1] == '\t'))
      --equals;
    while (value != end && (*value == ' ' || *value == '\t'))
      ++value;
    while (end != value && (end[-1] == ' ' || end[-1] == '\t'))
      --end;
    if ((size_t)(equals - name) == expected_name_size &&
        memcmp(name, server->session_cookie_name, expected_name_size) == 0) {
      if (out_size != NULL) *out_size = (size_t)(end - value);
      return value;
    }
    cookie = delimiter == NULL ? NULL : delimiter + 1u;
  }
  return NULL;
}

static bool chttp_session_record_matches(
    const chttp_session_context *context) {
  return context != NULL && context->record != NULL &&
         context->record->used &&
         context->record->generation == context->record_generation &&
         memcmp(context->record->id, context->id,
                CHTTP_SESSION_ID_TEXT_BYTES + 1u) == 0;
}

void chttp_session_request_begin(chttp_server_request_state *state,
                                 const chttp_server_request_view *request) {
  chttp_server_impl *server;
  chttp_session_context *context;
  const char *id;
  size_t id_size = 0u;
  size_t index;
  uint64_t now_ms;
  if (state == NULL || state->server == NULL) return;
  server = state->server;
  context = &state->session_context;
  chttp_session_context_reset(context);
  state->session.impl = context;
  if (server->config.session_capacity == 0u) return;

  id = chttp_session_cookie_value(server, request, &id_size);
  if (!chttp_session_hex_id(id, id_size)) return;
  context->presented = true;
  now_ms = monotonic_ms();

  sync_mutex_lock(&server->session_mutex);
  chttp_session_expire_locked(server, now_ms);
  for (index = 0u; index < server->config.session_capacity; ++index) {
    chttp_session_record *record = &server->sessions[index];
    if (record->used &&
        memcmp(record->id, id, CHTTP_SESSION_ID_TEXT_BYTES) == 0) {
      record->expires_at_ms = chttp_session_expiry(server, now_ms);
      context->record = record;
      context->record_generation = record->generation;
      memcpy(context->id, record->id, sizeof(context->id));
      chttp_session_snapshot_from_record(context, record);
      break;
    }
  }
  sync_mutex_unlock(&server->session_mutex);
}

static bool chttp_session_id_exists_locked(const chttp_server_impl *server,
                                           const char *id) {
  size_t index;
  for (index = 0u; index < server->config.session_capacity; ++index)
    if (server->sessions[index].used &&
        strcmp(server->sessions[index].id, id) == 0)
      return true;
  return false;
}

static int chttp_session_generate_id_locked(
    const chttp_server_impl *server,
    char out_id[CHTTP_SESSION_ID_TEXT_BYTES + 1u]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char random[CHTTP_SESSION_ID_RANDOM_BYTES];
  size_t attempt;
  size_t index;

  if (server == NULL || out_id == NULL) return SALTS_EINVAL;
  out_id[0] = '\0';
  for (attempt = 0u; attempt < CHTTP_SESSION_ID_ATTEMPTS; ++attempt) {
    int status = secure_random(random, sizeof(random));
    if (status != SALTS_OK) return status;
    for (index = 0u; index < sizeof(random); ++index) {
      out_id[index * 2u] = hex[random[index] >> 4u];
      out_id[index * 2u + 1u] = hex[random[index] & 0x0fu];
    }
    out_id[CHTTP_SESSION_ID_TEXT_BYTES] = '\0';
    if (!chttp_session_id_exists_locked(server, out_id)) return SALTS_OK;
  }
  out_id[0] = '\0';
  return SALTS_EALREADY;
}

static int chttp_session_create(chttp_session_context *context) {
  chttp_session_record *record = NULL;
  char id[CHTTP_SESSION_ID_TEXT_BYTES + 1u];
  size_t record_index;
  uint64_t now_ms;
  int status;
  if (context == NULL || context->server == NULL) return SALTS_EINVAL;

  if (context->staged) {
    context->staged_exists = true;
    context->created = true;
    context->invalidated = false;
    context->dirty = false;
    chttp_session_snapshot_clear(context);
    return SALTS_OK;
  }
  now_ms = monotonic_ms();
  sync_mutex_lock(&context->server->session_mutex);
  chttp_session_expire_locked(context->server, now_ms);
  for (record_index = 0u;
       record_index < context->server->config.session_capacity;
       ++record_index) {
    if (!context->server->sessions[record_index].used &&
        !context->server->sessions[record_index].worker_pins) {
      record = &context->server->sessions[record_index];
      break;
    }
  }
  if (record == NULL) {
    sync_mutex_unlock(&context->server->session_mutex);
    return SALTS_ENOBUFS;
  }

  status = chttp_session_generate_id_locked(context->server, id);
  if (status != SALTS_OK) {
    sync_mutex_unlock(&context->server->session_mutex);
    return status;
  }

  chttp_session_entries_clear(context->server, record->entries);
  record->generation = chttp_session_generation_next(record->generation);
  memcpy(record->id, id, sizeof(id));
  record->expires_at_ms = chttp_session_expiry(context->server, now_ms);
  record->used = true;

  context->record = record;
  context->record_generation = record->generation;
  memcpy(context->id, id, sizeof(context->id));
  context->created = true;
  context->invalidated = false;
  context->dirty = false;
  chttp_session_snapshot_clear(context);
  sync_mutex_unlock(&context->server->session_mutex);
  return SALTS_OK;
}

static chttp_session_context *chttp_session_context_get(
    chttp_session *session) {
  return session == NULL ? NULL : (chttp_session_context *)session->impl;
}

static const chttp_session_context *chttp_session_context_const_get(
    const chttp_session *session) {
  return session == NULL
             ? NULL
             : (const chttp_session_context *)session->impl;
}

const char *chttp_session_get(const chttp_session *session, const char *key) {
  const chttp_session_context *context =
      chttp_session_context_const_get(session);
  size_t index;
  if (context == NULL || key == NULL || (context->staged ? !context->staged_exists : context->record == NULL) ||
      context->invalidated)
    return NULL;
  for (index = 0u;
       index < context->server->config.session_entry_capacity; ++index) {
    const chttp_session_entry *entry = &context->snapshot_entries[index];
    if (entry->used && strcmp(entry->key, key) == 0) return entry->value;
  }
  return NULL;
}

int chttp_session_set(chttp_session *session, const char *key,
                      const char *value) {
  chttp_session_context *context = chttp_session_context_get(session);
  chttp_session_entry *free_entry = NULL;
  size_t key_size;
  size_t value_size;
  size_t index;
  int status;
  if (context == NULL || key == NULL || value == NULL || key[0] == '\0' ||
      context->invalidated)
    return SALTS_EINVAL;
  key_size = strlen(key);
  value_size = strlen(value);
  if (key_size > context->server->config.max_session_key_bytes ||
      value_size > context->server->config.max_session_value_bytes)
    return SALTS_EMSGSIZE;
  if (context->sealed) return SALTS_EALREADY;
  if (context->staged ? !context->staged_exists : context->record == NULL) {
    status = chttp_session_create(context);
    if (status != SALTS_OK) return status;
  }
  for (index = 0u;
       index < context->server->config.session_entry_capacity; ++index) {
    chttp_session_entry *entry = &context->snapshot_entries[index];
    if (entry->used && strcmp(entry->key, key) == 0) {
      memcpy(entry->value, value, value_size + 1u);
      context->dirty = true;
      return SALTS_OK;
    }
    if (!entry->used && free_entry == NULL) free_entry = entry;
  }
  if (free_entry == NULL) return SALTS_ENOBUFS;
  memcpy(free_entry->key, key, key_size + 1u);
  memcpy(free_entry->value, value, value_size + 1u);
  free_entry->used = true;
  context->dirty = true;
  return SALTS_OK;
}

int chttp_session_remove(chttp_session *session, const char *key) {
  chttp_session_context *context = chttp_session_context_get(session);
  size_t index;
  if (context == NULL || key == NULL || key[0] == '\0' ||
      context->invalidated)
    return SALTS_EINVAL;
  if (context->sealed) return SALTS_EALREADY;
  if (context->staged ? !context->staged_exists : context->record == NULL) return SALTS_ENOENT;
  for (index = 0u;
       index < context->server->config.session_entry_capacity; ++index) {
    chttp_session_entry *entry = &context->snapshot_entries[index];
    if (entry->used && strcmp(entry->key, key) == 0) {
      entry->used = false;
      entry->key[0] = '\0';
      entry->value[0] = '\0';
      context->dirty = true;
      return SALTS_OK;
    }
  }
  return SALTS_ENOENT;
}

int chttp_session_clear(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  if (context == NULL || context->invalidated) return SALTS_EINVAL;
  if (context->sealed) return SALTS_EALREADY;
  if (context->staged ? !context->staged_exists : context->record == NULL) return SALTS_OK;
  chttp_session_snapshot_clear(context);
  context->dirty = true;
  return SALTS_OK;
}

int chttp_session_regenerate(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  char id[CHTTP_SESSION_ID_TEXT_BYTES + 1u];
  int status;

  if (context == NULL || context->invalidated) return SALTS_EINVAL;
  if (context->sealed) return SALTS_EALREADY;
  if (context->staged) {
    context->staged_exists = true;
    context->staged_regenerate = true;
    context->created = true;
    return SALTS_OK;
  }
  if (context->record == NULL) return chttp_session_create(context);

  sync_mutex_lock(&context->server->session_mutex);
  if (!chttp_session_record_matches(context)) {
    sync_mutex_unlock(&context->server->session_mutex);
    return SALTS_ENOENT;
  }
  status = chttp_session_generate_id_locked(context->server, id);
  if (status == SALTS_OK) {
    memcpy(context->record->id, id, sizeof(id));
    context->record->expires_at_ms =
        chttp_session_expiry(context->server, monotonic_ms());
    context->record->generation =
        chttp_session_generation_next(context->record->generation);
    context->record_generation = context->record->generation;
    memcpy(context->id, id, sizeof(context->id));
    context->created = true;
    context->presented = false;
  }
  sync_mutex_unlock(&context->server->session_mutex);
  return status;
}

int chttp_session_invalidate(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  if (context == NULL) return SALTS_EINVAL;
  if (context->sealed) return SALTS_EALREADY;
  if (context->staged) {
    context->staged_exists = false;
    context->invalidated = true;
    context->dirty = false;
    chttp_session_snapshot_clear(context);
    return SALTS_OK;
  }
  if (context->record != NULL) {
    sync_mutex_lock(&context->server->session_mutex);
    if (chttp_session_record_matches(context))
      chttp_session_record_clear(context->server, context->record);
    sync_mutex_unlock(&context->server->session_mutex);
  }
  context->record = NULL;
  context->record_generation = 0u;
  context->id[0] = '\0';
  context->invalidated = true;
  context->created = false;
  context->dirty = false;
  chttp_session_snapshot_clear(context);
  return SALTS_OK;
}

static int chttp_session_set_cookie(chttp_server_impl *server,
    chttp_server_response *response, const char *id, uint32_t max_age_seconds) {
  char cookie[CHTTP_SESSION_COOKIE_BYTES];
  int cookie_size =
      snprintf(cookie, sizeof(cookie),
               "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%u%s",
               server->session_cookie_name, id,
               (unsigned int)max_age_seconds,
               server->config.session_cookie_secure ? "; Secure" : "");
  if (cookie_size < 0 || (size_t)cookie_size >= sizeof(cookie))
    return SALTS_EMSGSIZE;
  return chttp_server_response_set_header(response, "Set-Cookie",
                                          cookie);
}

static void chttp_session_discard_created(chttp_session_context *context) {
  if (context == NULL || context->server == NULL || !context->created ||
      context->record == NULL)
    return;
  sync_mutex_lock(&context->server->session_mutex);
  if (chttp_session_record_matches(context))
    chttp_session_record_clear(context->server, context->record);
  sync_mutex_unlock(&context->server->session_mutex);
  context->record = NULL;
  context->record_generation = 0u;
  context->id[0] = '\0';
  context->created = false;
  context->dirty = false;
}

int chttp_session_request_finish(chttp_server_request_state *state) {
  chttp_session_context *context;
  char id[CHTTP_SESSION_ID_TEXT_BYTES + 1u];
  uint32_t max_age_seconds;
  int stale = 0;
  int status;
  if (state == NULL || state->server == NULL ||
      state->server->config.session_capacity == 0u)
    return SALTS_OK;
  context = &state->session_context;
  if (context->invalidated)
    return chttp_session_set_cookie(state->server, &state->response, "", 0u);
  if (context->record == NULL) return SALTS_OK;

  sync_mutex_lock(&context->server->session_mutex);
  if (!chttp_session_record_matches(context)) {
    stale = 1;
    id[0] = '\0';
  } else {
    if (context->dirty)
      chttp_session_record_from_snapshot(context, context->record);
    context->record->expires_at_ms =
        chttp_session_expiry(context->server, monotonic_ms());
    memcpy(id, context->record->id, sizeof(id));
  }
  sync_mutex_unlock(&context->server->session_mutex);

  if (stale) {
    context->record = NULL;
    context->record_generation = 0u;
    context->id[0] = '\0';
    context->invalidated = true;
    context->created = false;
    context->dirty = false;
    return chttp_session_set_cookie(state->server, &state->response, "", 0u);
  }

  max_age_seconds =
      state->server->config.session_idle_timeout_ms / 1000u;
  if (state->server->config.session_idle_timeout_ms % 1000u != 0u)
    ++max_age_seconds;
  if (max_age_seconds == 0u) max_age_seconds = 1u;
  status = chttp_session_set_cookie(state->server, &state->response, id, max_age_seconds);
  if (status != SALTS_OK && context->created)
    chttp_session_discard_created(context);
  else if (status == SALTS_OK) {
    context->created = false;
    context->dirty = false;
  }
  return status;
}

void chttp_session_request_abort(chttp_server_request_state *state) {
  chttp_session_context *context;
  if (state == NULL || state->server == NULL ||
      state->server->config.session_capacity == 0u)
    return;
  context = &state->session_context;
  chttp_session_discard_created(context);
  context->record = NULL;
  context->record_generation = 0u;
  context->id[0] = '\0';
  context->dirty = false;
}

struct chttp_session_work {
  chttp_session_context context;
  chttp_session view;
  const chttp_session_context *origin; /* Only compared on owner-side attach. */
  chttp_session_record *record;
  uint32_t generation;
  char original_id[CHTTP_SESSION_ID_TEXT_BYTES + 1u];
  void (*committed)(void *, int);
  void *user;
  bool pinned, begun, commit_allowed, owner_done;
  int owner_status;
};

static bool session_work_matches(const chttp_session_work *work) {
  return work->record && work->record->used && work->record->generation == work->generation &&
      memcmp(work->record->id, work->original_id, sizeof(work->original_id)) == 0;
}
int chttp_session_work_size(const chttp_session *session, size_t *out) {
  const chttp_session_context *context = chttp_session_context_const_get(session);
  if (!out || !context || !context->server) return SALTS_EINVAL;
  if (context->staged || context->dirty || context->created || context->invalidated ||
      !context->server->config.session_capacity) return SALTS_ENOTSUP;
  const chttp_server_config *c = &context->server->config;
  size_t entries, keys, values;
  if (c->max_session_key_bytes == SIZE_MAX || c->max_session_value_bytes == SIZE_MAX ||
      !chttp_session_multiply(c->session_entry_capacity, sizeof(chttp_session_entry), &entries) ||
      !chttp_session_multiply(c->session_entry_capacity, c->max_session_key_bytes + 1u, &keys) ||
      !chttp_session_multiply(c->session_entry_capacity, c->max_session_value_bytes + 1u, &values))
    return SALTS_ERANGE;
  size_t bytes = sizeof(chttp_session_work);
  if (entries > SIZE_MAX - bytes) return SALTS_ERANGE;
  bytes += entries;
  if (keys > SIZE_MAX - bytes) return SALTS_ERANGE;
  bytes += keys;
  if (values > SIZE_MAX - bytes) return SALTS_ERANGE;
  bytes += values;
  const size_t alignment = _Alignof(max_align_t);
  if (bytes > SIZE_MAX - (alignment - 1u)) return SALTS_ERANGE;
  *out = (bytes + alignment - 1u) / alignment * alignment;
  return SALTS_OK;
}
int chttp_session_work_prepare(void *storage, size_t capacity, const chttp_session *session,
    void (*committed)(void *, int), void *user, chttp_session_work **out) {
  if (!out || !storage || !committed || (uintptr_t)storage % _Alignof(max_align_t))
    return SALTS_EINVAL;
  *out = NULL;
  size_t bytes;
  int status = chttp_session_work_size(session, &bytes);
  if (status != SALTS_OK) return status;
  if (capacity < bytes) return SALTS_ENOBUFS;
  const chttp_session_context *origin = session->impl;
  chttp_server_impl *server = origin->server;
  chttp_session_work *work = storage;
  memset(storage, 0, bytes);
  work->origin = origin; work->committed = committed; work->user = user;
  work->context.server = server; work->context.staged = true;
  work->context.presented = origin->presented;
  work->view.impl = &work->context;
  unsigned char *next = (unsigned char *)(work + 1);
  work->context.snapshot_entries = (chttp_session_entry *)next;
  next += server->config.session_entry_capacity * sizeof(chttp_session_entry);
  work->context.snapshot_keys = (char *)next;
  next += server->config.session_entry_capacity * (server->config.max_session_key_bytes + 1u);
  work->context.snapshot_values = (char *)next;
  for (size_t i = 0; i < server->config.session_entry_capacity; ++i) {
    work->context.snapshot_entries[i].key = work->context.snapshot_keys +
        i * (server->config.max_session_key_bytes + 1u);
    work->context.snapshot_entries[i].value = work->context.snapshot_values +
        i * (server->config.max_session_value_bytes + 1u);
  }
  sync_mutex_lock(&server->session_mutex);
  if (origin->record) {
    if (!chttp_session_record_matches(origin)) status = SALTS_ENOENT;
    else if (origin->record->worker_pins == SIZE_MAX) status = SALTS_ERANGE;
    else {
      work->record = origin->record; work->generation = origin->record_generation;
      memcpy(work->original_id, origin->id, sizeof(work->original_id));
      ++work->record->worker_pins; work->pinned = true;
      chttp_session_snapshot_from_record(&work->context, work->record);
      work->context.staged_exists = true;
    }
  }
  sync_mutex_unlock(&server->session_mutex);
  if (status == SALTS_OK) *out = work;
  return status;
}
chttp_session *chttp_session_work_view(chttp_session_work *work) {
  return work ? &work->view : NULL;
}
const void *chttp_session_work_key(const chttp_session_work *work) {
  return work ? work->record : NULL;
}
int chttp_session_work_begin(chttp_session_work *work) {
  if (!work) return SALTS_OK;
  if (work->begun || work->context.sealed) return SALTS_EALREADY;
  chttp_server_impl *server = work->context.server;
  int status = SALTS_OK;
  sync_mutex_lock(&server->session_mutex);
  if (work->owner_done) status = SALTS_ECANCELED;
  else if (work->record && !session_work_matches(work)) status = SALTS_ENOENT;
  else {
    if (work->record) chttp_session_snapshot_from_record(&work->context, work->record);
    work->context.staged_exists = work->record != NULL;
    work->begun = true;
  }
  sync_mutex_unlock(&server->session_mutex);
  return status;
}
void chttp_session_work_seal(chttp_session_work *work, bool commit) {
  if (!work) return;
  work->commit_allowed = commit;
  work->context.sealed = true;
}
void chttp_session_work_release(chttp_session_work *work) {
  if (!work) return;
  chttp_server_impl *server = work->context.server;
  sync_mutex_lock(&server->session_mutex);
  /* Explicit logout/fail-closed invalidation is irreversible. A dead transport
   * or failed cookie publication cannot preserve the original authentication.
   * This reconciles only the admitted generation; never clear a newer identity.
   * Framework retirement owns this store-only cleanup, after business last use. */
  if (work->context.sealed && work->context.invalidated && session_work_matches(work))
    chttp_session_record_clear(server, work->record);
  if (work->pinned) { --work->record->worker_pins; work->pinned = false; }
  sync_mutex_unlock(&server->session_mutex);
  /* The invocation owns storage, including all snapshot buffers. */
}
int chttp_session_work_attach(chttp_server_request_state *state, chttp_session_work *work) {
  if (!work || !state || work->origin != &state->session_context || state->session_work)
    return SALTS_EINVAL;
  state->session_work = work;
  return SALTS_OK;
}
static void session_work_owner_done(chttp_session_work *work, int status) {
  chttp_server_impl *server = work->context.server;
  sync_mutex_lock(&server->session_mutex);
  const bool notify = !work->owner_done;
  if (notify) { work->owner_done = true; work->owner_status = status; }
  sync_mutex_unlock(&server->session_mutex);
  if (notify) work->committed(work->user, status);
}
void chttp_session_work_detach(chttp_server_request_state *state, int status) {
  if (!state || !state->session_work) return;
  chttp_session_work *work = state->session_work;
  state->session_work = NULL;
  session_work_owner_done(work, status);
}
static int session_work_commit(chttp_session_work *work, chttp_server_response *response) {
  chttp_session_context *context = &work->context;
  chttp_server_impl *server = context->server;
  if (!context->sealed) return SALTS_EPROTO;
  if (!work->commit_allowed && !context->invalidated) return SALTS_OK;
  if (!work->begun) return SALTS_EPROTO;
  char id[CHTTP_SESSION_ID_TEXT_BYTES + 1u] = {0};
  int status = SALTS_OK;
  sync_mutex_lock(&server->session_mutex);
  chttp_session_record *record = work->record;
  if (record && !session_work_matches(work)) status = SALTS_ENOENT;
  if (status == SALTS_OK && !context->invalidated && context->staged_exists) {
    if (!record) {
      chttp_session_expire_locked(server, monotonic_ms());
      for (size_t i = 0; i < server->config.session_capacity; ++i)
        if (!server->sessions[i].used && !server->sessions[i].worker_pins) {
          record = &server->sessions[i]; break;
        }
      if (!record) status = SALTS_ENOBUFS;
    }
    if (status == SALTS_OK) {
      if (!work->record || context->staged_regenerate)
        status = chttp_session_generate_id_locked(server, id);
      else memcpy(id, record->id, sizeof(id));
    }
  }
  /* Reserve bounded header storage before changing the authoritative record. */
  if (status == SALTS_OK && (context->invalidated || context->staged_exists)) {
    uint32_t max_age = server->config.session_idle_timeout_ms / 1000u;
    if (server->config.session_idle_timeout_ms % 1000u) ++max_age;
    if (!max_age) max_age = 1u;
    status = chttp_session_set_cookie(server, response, context->invalidated ? "" : id,
        context->invalidated ? 0u : max_age);
  }
  if (status == SALTS_OK) {
    if (context->invalidated) { if (record) chttp_session_record_clear(server, record); }
    else if (context->staged_exists) {
      if (!work->record || context->staged_regenerate) {
        record->generation = chttp_session_generation_next(record->generation);
        memcpy(record->id, id, sizeof(id));
      }
      if (context->dirty || context->created) chttp_session_record_from_snapshot(context, record);
      record->used = true;
      record->expires_at_ms = chttp_session_expiry(server, monotonic_ms());
    }
  }
  sync_mutex_unlock(&server->session_mutex);
  return status;
}
int chttp_session_request_finish_deferred(chttp_server_request_state *state,
    chttp_server_response *response) {
  if (!state->session_work) return chttp_session_request_finish(state);
  chttp_session_work *work = state->session_work;
  sync_mutex_lock(&state->server->session_mutex);
  const bool settled = work->owner_done;
  sync_mutex_unlock(&state->server->session_mutex);
  if (settled) return SALTS_EALREADY;
  const int status = session_work_commit(work, response);
  session_work_owner_done(work, status);
  return status;
}
