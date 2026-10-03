#include "chttp_server_runtime.h"

#include <salts/clock.h>
#include <salts/random.h>

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
  salts_mutex_init(&server->session_mutex);
  server->session_sync_initialized = true;
  return SALTS_OK;
}

void chttp_session_store_destroy(chttp_server_impl *server) {
  if (server == NULL) return;
  if (server->session_sync_initialized) {
    salts_mutex_destroy(&server->session_mutex);
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
    if (record->used && record->expires_at_ms <= now_ms)
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
  now_ms = salts_monotonic_ms();

  salts_mutex_lock(&server->session_mutex);
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
  salts_mutex_unlock(&server->session_mutex);
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
    int status = salts_platform_secure_random(random, sizeof(random));
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

  now_ms = salts_monotonic_ms();
  salts_mutex_lock(&context->server->session_mutex);
  chttp_session_expire_locked(context->server, now_ms);
  for (record_index = 0u;
       record_index < context->server->config.session_capacity;
       ++record_index) {
    if (!context->server->sessions[record_index].used) {
      record = &context->server->sessions[record_index];
      break;
    }
  }
  if (record == NULL) {
    salts_mutex_unlock(&context->server->session_mutex);
    return SALTS_ENOBUFS;
  }

  status = chttp_session_generate_id_locked(context->server, id);
  if (status != SALTS_OK) {
    salts_mutex_unlock(&context->server->session_mutex);
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
  salts_mutex_unlock(&context->server->session_mutex);
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
  if (context == NULL || key == NULL || context->record == NULL ||
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
  if (context->record == NULL) {
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
  if (context->record == NULL) return SALTS_ENOENT;
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
  if (context->record == NULL) return SALTS_OK;
  chttp_session_snapshot_clear(context);
  context->dirty = true;
  return SALTS_OK;
}

int chttp_session_regenerate(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  char id[CHTTP_SESSION_ID_TEXT_BYTES + 1u];
  int status;

  if (context == NULL || context->invalidated) return SALTS_EINVAL;
  if (context->record == NULL) return chttp_session_create(context);

  salts_mutex_lock(&context->server->session_mutex);
  if (!chttp_session_record_matches(context)) {
    salts_mutex_unlock(&context->server->session_mutex);
    return SALTS_ENOENT;
  }
  status = chttp_session_generate_id_locked(context->server, id);
  if (status == SALTS_OK) {
    memcpy(context->record->id, id, sizeof(id));
    context->record->expires_at_ms =
        chttp_session_expiry(context->server, salts_monotonic_ms());
    context->record->generation =
        chttp_session_generation_next(context->record->generation);
    context->record_generation = context->record->generation;
    memcpy(context->id, id, sizeof(context->id));
    context->created = true;
    context->presented = false;
  }
  salts_mutex_unlock(&context->server->session_mutex);
  return status;
}

int chttp_session_invalidate(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  if (context == NULL) return SALTS_EINVAL;
  if (context->record != NULL) {
    salts_mutex_lock(&context->server->session_mutex);
    if (chttp_session_record_matches(context))
      chttp_session_record_clear(context->server, context->record);
    salts_mutex_unlock(&context->server->session_mutex);
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

static int chttp_session_set_cookie(chttp_server_request_state *state,
                                    const char *id,
                                    uint32_t max_age_seconds) {
  chttp_server_impl *server = state->server;
  char cookie[CHTTP_SESSION_COOKIE_BYTES];
  int cookie_size =
      snprintf(cookie, sizeof(cookie),
               "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%u%s",
               server->session_cookie_name, id,
               (unsigned int)max_age_seconds,
               server->config.session_cookie_secure ? "; Secure" : "");
  if (cookie_size < 0 || (size_t)cookie_size >= sizeof(cookie))
    return SALTS_EMSGSIZE;
  return chttp_server_response_set_header(&state->response, "Set-Cookie",
                                          cookie);
}

static void chttp_session_discard_created(chttp_session_context *context) {
  if (context == NULL || context->server == NULL || !context->created ||
      context->record == NULL)
    return;
  salts_mutex_lock(&context->server->session_mutex);
  if (chttp_session_record_matches(context))
    chttp_session_record_clear(context->server, context->record);
  salts_mutex_unlock(&context->server->session_mutex);
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
    return chttp_session_set_cookie(state, "", 0u);
  if (context->record == NULL) return SALTS_OK;

  salts_mutex_lock(&context->server->session_mutex);
  if (!chttp_session_record_matches(context)) {
    stale = 1;
    id[0] = '\0';
  } else {
    if (context->dirty)
      chttp_session_record_from_snapshot(context, context->record);
    context->record->expires_at_ms =
        chttp_session_expiry(context->server, salts_monotonic_ms());
    memcpy(id, context->record->id, sizeof(id));
  }
  salts_mutex_unlock(&context->server->session_mutex);

  if (stale) {
    context->record = NULL;
    context->record_generation = 0u;
    context->id[0] = '\0';
    context->invalidated = true;
    context->created = false;
    context->dirty = false;
    return chttp_session_set_cookie(state, "", 0u);
  }

  max_age_seconds =
      state->server->config.session_idle_timeout_ms / 1000u;
  if (state->server->config.session_idle_timeout_ms % 1000u != 0u)
    ++max_age_seconds;
  if (max_age_seconds == 0u) max_age_seconds = 1u;
  status = chttp_session_set_cookie(state, id, max_age_seconds);
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
