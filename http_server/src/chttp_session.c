#include "chttp_server_runtime.h"

#include <salts/clock.h>
#include <salts/random.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CHTTP_SESSION_ID_RANDOM_BYTES = 16,
  CHTTP_SESSION_ID_TEXT_BYTES = 32,
  CHTTP_SESSION_ID_ATTEMPTS = 128,
  CHTTP_SESSION_COOKIE_BYTES = 256,
  CHTTP_SESSION_STORE_MAX_SHARDS = 8,
  CHTTP_SESSION_EXPIRE_BUDGET = 4
};

static bool chttp_session_multiply(size_t left, size_t right, size_t *out) {
  if (out == NULL || (right != 0u && left > SIZE_MAX / right)) return false;
  *out = left * right;
  return true;
}

static uint32_t chttp_session_next_generation(uint32_t generation) {
  ++generation;
  return generation == 0u ? 1u : generation;
}

static void chttp_session_entries_clear(
    const chttp_server_impl *server, chttp_session_entry *entries) {
  size_t index;
  if (server == NULL || entries == NULL) return;
  for (index = 0u; index < server->config.session_entry_capacity; ++index) {
    entries[index].used = false;
    entries[index].key[0] = '\0';
    entries[index].value[0] = '\0';
  }
}

static void chttp_session_record_clear(
    const chttp_server_impl *server, chttp_session_record *record) {
  if (server == NULL || record == NULL) return;
  chttp_session_entries_clear(server, record->entries);
  record->id[0] = '\0';
  record->expires_at_ms = 0u;
  record->used = false;
  record->generation = chttp_session_next_generation(record->generation);
}

static uint64_t chttp_session_hash(const char *id) {
  uint64_t hash = UINT64_C(1469598103934665603);
  size_t index;
  if (id == NULL) return 0u;
  for (index = 0u; index < CHTTP_SESSION_ID_TEXT_BYTES && id[index] != '\0'; ++index) {
    hash ^= (uint64_t)(unsigned char)id[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static chttp_session_shard *chttp_session_shard_for_id(
    const chttp_server_impl *server, const char *id) {
  size_t index;
  if (server == NULL || server->session_shards == NULL ||
      server->session_shard_count == 0u || id == NULL)
    return NULL;
  index = (size_t)(chttp_session_hash(id) % server->session_shard_count);
  return &server->session_shards[index];
}

static size_t chttp_session_shard_end(const chttp_session_shard *shard) {
  return shard == NULL ? 0u : shard->record_begin + shard->record_count;
}

static uint64_t chttp_session_expiry(
    const chttp_server_impl *server, uint64_t now_ms) {
  if ((uint64_t)server->config.session_idle_timeout_ms > UINT64_MAX - now_ms)
    return UINT64_MAX;
  return now_ms + (uint64_t)server->config.session_idle_timeout_ms;
}

static void chttp_session_shard_expire_locked(
    chttp_server_impl *server, chttp_session_shard *shard,
    uint64_t now_ms) {
  size_t budget;
  size_t step;
  if (server == NULL || shard == NULL || shard->record_count == 0u) return;
  budget = shard->record_count < CHTTP_SESSION_EXPIRE_BUDGET
               ? shard->record_count
               : CHTTP_SESSION_EXPIRE_BUDGET;
  for (step = 0u; step < budget; ++step) {
    const size_t offset = shard->expiry_cursor++ % shard->record_count;
    chttp_session_record *record =
        &server->sessions[shard->record_begin + offset];
    if (record->used && record->expires_at_ms <= now_ms)
      chttp_session_record_clear(server, record);
  }
}

static chttp_session_record *chttp_session_record_find_locked(
    chttp_server_impl *server, const chttp_session_shard *shard,
    const char *id) {
  size_t index;
  size_t end;
  if (server == NULL || shard == NULL || id == NULL) return NULL;
  end = chttp_session_shard_end(shard);
  for (index = shard->record_begin; index < end; ++index) {
    chttp_session_record *record = &server->sessions[index];
    if (record->used &&
        memcmp(record->id, id, CHTTP_SESSION_ID_TEXT_BYTES) == 0 &&
        record->id[CHTTP_SESSION_ID_TEXT_BYTES] == '\0')
      return record;
  }
  return NULL;
}

static chttp_session_record *chttp_session_record_free_locked(
    chttp_server_impl *server, const chttp_session_shard *shard) {
  size_t index;
  size_t end;
  if (server == NULL || shard == NULL) return NULL;
  end = chttp_session_shard_end(shard);
  for (index = shard->record_begin; index < end; ++index)
    if (!server->sessions[index].used) return &server->sessions[index];
  return NULL;
}

static void chttp_session_record_prepare(
    const chttp_server_impl *server, chttp_session_record *record,
    const char *id, uint64_t expires_at_ms) {
  if (server == NULL || record == NULL || id == NULL) return;
  chttp_session_entries_clear(server, record->entries);
  if (record->generation == 0u) record->generation = 1u;
  memcpy(record->id, id, CHTTP_SESSION_ID_TEXT_BYTES + 1u);
  record->expires_at_ms = expires_at_ms;
  record->used = true;
}

static void chttp_session_record_copy_entries(
    const chttp_server_impl *server,
    chttp_session_record *destination,
    const chttp_session_record *source) {
  size_t index;
  if (server == NULL || destination == NULL || source == NULL) return;
  chttp_session_entries_clear(server, destination->entries);
  for (index = 0u; index < server->config.session_entry_capacity; ++index) {
    const chttp_session_entry *src = &source->entries[index];
    chttp_session_entry *dst = &destination->entries[index];
    if (!src->used) continue;
    memcpy(dst->key, src->key, strlen(src->key) + 1u);
    memcpy(dst->value, src->value, strlen(src->value) + 1u);
    dst->used = true;
  }
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
  if (context == NULL || context->server == NULL || record == NULL ||
      context->snapshot_entries == NULL)
    return;
  chttp_session_snapshot_clear(context);
  for (index = 0u; index < context->server->config.session_entry_capacity; ++index) {
    const chttp_session_entry *src = &record->entries[index];
    chttp_session_entry *dst = &context->snapshot_entries[index];
    if (!src->used) continue;
    memcpy(dst->key, src->key, strlen(src->key) + 1u);
    memcpy(dst->value, src->value, strlen(src->value) + 1u);
    dst->used = true;
  }
}

static bool chttp_session_context_record_valid_locked(
    const chttp_session_context *context) {
  return context != NULL && context->record != NULL &&
         context->shard != NULL && context->record->used &&
         context->record->generation == context->record_generation &&
         memcmp(context->record->id, context->id,
                CHTTP_SESSION_ID_TEXT_BYTES + 1u) == 0;
}

static void chttp_session_context_bind_locked(
    chttp_session_context *context, chttp_session_shard *shard,
    chttp_session_record *record) {
  if (context == NULL || shard == NULL || record == NULL) return;
  context->shard = shard;
  context->record = record;
  context->record_generation = record->generation;
  memcpy(context->id, record->id, sizeof(context->id));
  chttp_session_snapshot_from_record(context, record);
}

int chttp_session_context_init(
    chttp_session_context *context, chttp_server_impl *server) {
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
      !chttp_session_multiply(
          server->config.session_entry_capacity, key_stride, &key_bytes) ||
      !chttp_session_multiply(
          server->config.session_entry_capacity, value_stride, &value_bytes) ||
      server->config.session_entry_capacity >
          SIZE_MAX / sizeof(*context->snapshot_entries))
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
  *context = (chttp_session_context){
      .server = server,
      .snapshot_entries = entries,
      .snapshot_keys = keys,
      .snapshot_values = values};
  chttp_session_snapshot_clear(context);
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
  size_t shard_index;
  size_t shard_count;

  if (server == NULL) return SALTS_EINVAL;
  if (server->config.session_capacity == 0u) return SALTS_OK;

  key_stride = server->config.max_session_key_bytes + 1u;
  value_stride = server->config.max_session_value_bytes + 1u;
  if (key_stride == 0u || value_stride == 0u ||
      !chttp_session_multiply(server->config.session_capacity,
                              server->config.session_entry_capacity,
                              &entry_count) ||
      !chttp_session_multiply(entry_count, key_stride, &key_bytes) ||
      !chttp_session_multiply(entry_count, value_stride, &value_bytes) ||
      server->config.session_capacity > SIZE_MAX / sizeof(*server->sessions) ||
      entry_count > SIZE_MAX / sizeof(*server->session_entries))
    return SALTS_ERANGE;

  shard_count = server->config.session_capacity < CHTTP_SESSION_STORE_MAX_SHARDS
                    ? server->config.session_capacity
                    : CHTTP_SESSION_STORE_MAX_SHARDS;

  server->sessions = (chttp_session_record *)calloc(
      server->config.session_capacity, sizeof(*server->sessions));
  server->session_entries =
      (chttp_session_entry *)calloc(entry_count, sizeof(*server->session_entries));
  server->session_keys = (char *)calloc(key_bytes, 1u);
  server->session_values = (char *)calloc(value_bytes, 1u);
  server->session_shards =
      (chttp_session_shard *)calloc(shard_count, sizeof(*server->session_shards));
  server->session_shard_count = shard_count;

  if (server->sessions == NULL || server->session_entries == NULL ||
      server->session_keys == NULL || server->session_values == NULL ||
      server->session_shards == NULL) {
    chttp_session_store_destroy(server);
    return SALTS_ENOMEM;
  }

  for (record_index = 0u;
       record_index < server->config.session_capacity; ++record_index) {
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

  for (shard_index = 0u; shard_index < shard_count; ++shard_index) {
    chttp_session_shard *shard = &server->session_shards[shard_index];
    const size_t begin =
        server->config.session_capacity * shard_index / shard_count;
    const size_t end =
        server->config.session_capacity * (shard_index + 1u) / shard_count;
    shard->record_begin = begin;
    shard->record_count = end - begin;
    salts_mutex_init(&shard->mutex);
    if (shard->mutex == NULL) {
      chttp_session_store_destroy(server);
      return SALTS_ENOMEM;
    }
    shard->initialized = true;
  }

  return SALTS_OK;
}

void chttp_session_store_destroy(chttp_server_impl *server) {
  size_t shard_index;
  if (server == NULL) return;
  if (server->session_shards != NULL) {
    for (shard_index = 0u;
         shard_index < server->session_shard_count; ++shard_index)
      if (server->session_shards[shard_index].initialized)
        salts_mutex_destroy(&server->session_shards[shard_index].mutex);
  }
  free(server->session_shards);
  free(server->session_values);
  free(server->session_keys);
  free(server->session_entries);
  free(server->sessions);
  server->session_shards = NULL;
  server->session_shard_count = 0u;
  server->session_values = NULL;
  server->session_keys = NULL;
  server->session_entries = NULL;
  server->sessions = NULL;
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

void chttp_session_request_begin(
    chttp_server_request_state *state,
    const chttp_server_request_view *request) {
  chttp_server_impl *server;
  chttp_session_context *context;
  chttp_session_shard *shard;
  chttp_session_record *record;
  const char *id;
  size_t id_size = 0u;
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

  memcpy(context->id, id, CHTTP_SESSION_ID_TEXT_BYTES);
  context->id[CHTTP_SESSION_ID_TEXT_BYTES] = '\0';
  shard = chttp_session_shard_for_id(server, context->id);
  if (shard == NULL) return;

  now_ms = salts_monotonic_ms();
  salts_mutex_lock(&shard->mutex);
  chttp_session_shard_expire_locked(server, shard, now_ms);
  record = chttp_session_record_find_locked(server, shard, context->id);
  if (record != NULL && record->expires_at_ms <= now_ms) {
    chttp_session_record_clear(server, record);
    record = NULL;
  }
  if (record != NULL) {
    record->expires_at_ms = chttp_session_expiry(server, now_ms);
    chttp_session_context_bind_locked(context, shard, record);
    context->presented = true;
  }
  salts_mutex_unlock(&shard->mutex);
}

static int chttp_session_random_id(
    char out_id[CHTTP_SESSION_ID_TEXT_BYTES + 1u]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char random[CHTTP_SESSION_ID_RANDOM_BYTES];
  size_t index;
  int status;

  if (out_id == NULL) return SALTS_EINVAL;
  status = salts_platform_secure_random(random, sizeof(random));
  if (status != SALTS_OK) return status;
  for (index = 0u; index < sizeof(random); ++index) {
    out_id[index * 2u] = hex[random[index] >> 4u];
    out_id[index * 2u + 1u] = hex[random[index] & 0x0fu];
  }
  out_id[CHTTP_SESSION_ID_TEXT_BYTES] = '\0';
  return SALTS_OK;
}

static int chttp_session_create(chttp_session_context *context) {
  char id[CHTTP_SESSION_ID_TEXT_BYTES + 1u];
  size_t attempt;
  uint64_t now_ms;
  int status;

  if (context == NULL || context->server == NULL || context->invalidated)
    return SALTS_EINVAL;

  now_ms = salts_monotonic_ms();
  for (attempt = 0u; attempt < CHTTP_SESSION_ID_ATTEMPTS; ++attempt) {
    chttp_session_shard *shard;
    chttp_session_record *record;

    status = chttp_session_random_id(id);
    if (status != SALTS_OK) return status;
    shard = chttp_session_shard_for_id(context->server, id);
    if (shard == NULL) return SALTS_EINVAL;

    salts_mutex_lock(&shard->mutex);
    chttp_session_shard_expire_locked(context->server, shard, now_ms);
    if (chttp_session_record_find_locked(context->server, shard, id) != NULL) {
      salts_mutex_unlock(&shard->mutex);
      continue;
    }
    record = chttp_session_record_free_locked(context->server, shard);
    if (record == NULL) {
      salts_mutex_unlock(&shard->mutex);
      continue;
    }

    chttp_session_record_prepare(
        context->server, record, id,
        chttp_session_expiry(context->server, now_ms));
    chttp_session_context_bind_locked(context, shard, record);
    context->created = true;
    salts_mutex_unlock(&shard->mutex);
    return SALTS_OK;
  }

  return SALTS_ENOBUFS;
}

static chttp_session_context *chttp_session_context_get(
    chttp_session *session) {
  return session == NULL
             ? NULL
             : (chttp_session_context *)session->impl;
}

static const chttp_session_context *chttp_session_context_const_get(
    const chttp_session *session) {
  return session == NULL
             ? NULL
             : (const chttp_session_context *)session->impl;
}

static chttp_session_entry *chttp_session_snapshot_entry(
    chttp_session_context *context, const char *key,
    chttp_session_entry **out_free) {
  size_t index;
  if (out_free != NULL) *out_free = NULL;
  if (context == NULL || context->server == NULL ||
      context->snapshot_entries == NULL || key == NULL)
    return NULL;
  for (index = 0u;
       index < context->server->config.session_entry_capacity; ++index) {
    chttp_session_entry *entry = &context->snapshot_entries[index];
    if (entry->used && strcmp(entry->key, key) == 0) return entry;
    if (!entry->used && out_free != NULL && *out_free == NULL)
      *out_free = entry;
  }
  return NULL;
}

const char *chttp_session_get(
    const chttp_session *session, const char *key) {
  const chttp_session_context *context =
      chttp_session_context_const_get(session);
  size_t index;
  if (context == NULL || key == NULL || context->invalidated ||
      context->snapshot_entries == NULL)
    return NULL;
  for (index = 0u;
       index < context->server->config.session_entry_capacity; ++index) {
    const chttp_session_entry *entry = &context->snapshot_entries[index];
    if (entry->used && strcmp(entry->key, key) == 0) return entry->value;
  }
  return NULL;
}

int chttp_session_set(
    chttp_session *session, const char *key, const char *value) {
  chttp_session_context *context = chttp_session_context_get(session);
  chttp_session_entry *snapshot_entry;
  chttp_session_entry *snapshot_free = NULL;
  chttp_session_entry *record_free = NULL;
  size_t key_size;
  size_t value_size;
  size_t index;
  int status;

  if (context == NULL || key == NULL || value == NULL ||
      key[0] == '\0' || context->invalidated)
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

  salts_mutex_lock(&context->shard->mutex);
  if (!chttp_session_context_record_valid_locked(context)) {
    salts_mutex_unlock(&context->shard->mutex);
    return SALTS_ENOENT;
  }

  for (index = 0u;
       index < context->server->config.session_entry_capacity; ++index) {
    chttp_session_entry *entry = &context->record->entries[index];
    if (entry->used && strcmp(entry->key, key) == 0) {
      memcpy(entry->value, value, value_size + 1u);
      context->record->expires_at_ms =
          chttp_session_expiry(context->server, salts_monotonic_ms());
      salts_mutex_unlock(&context->shard->mutex);

      snapshot_entry =
          chttp_session_snapshot_entry(context, key, &snapshot_free);
      if (snapshot_entry == NULL) snapshot_entry = snapshot_free;
      if (snapshot_entry == NULL) return SALTS_EPROTO;
      if (!snapshot_entry->used) {
        memcpy(snapshot_entry->key, key, key_size + 1u);
        snapshot_entry->used = true;
      }
      memcpy(snapshot_entry->value, value, value_size + 1u);
      return SALTS_OK;
    }
    if (!entry->used && record_free == NULL) record_free = entry;
  }

  if (record_free == NULL) {
    salts_mutex_unlock(&context->shard->mutex);
    return SALTS_ENOBUFS;
  }

  memcpy(record_free->key, key, key_size + 1u);
  memcpy(record_free->value, value, value_size + 1u);
  record_free->used = true;
  context->record->expires_at_ms =
      chttp_session_expiry(context->server, salts_monotonic_ms());
  salts_mutex_unlock(&context->shard->mutex);

  snapshot_entry = chttp_session_snapshot_entry(context, key, &snapshot_free);
  if (snapshot_entry == NULL) snapshot_entry = snapshot_free;
  if (snapshot_entry == NULL) return SALTS_EPROTO;
  memcpy(snapshot_entry->key, key, key_size + 1u);
  memcpy(snapshot_entry->value, value, value_size + 1u);
  snapshot_entry->used = true;
  return SALTS_OK;
}

int chttp_session_remove(chttp_session *session, const char *key) {
  chttp_session_context *context = chttp_session_context_get(session);
  size_t index;
  if (context == NULL || key == NULL || key[0] == '\0' ||
      context->invalidated)
    return SALTS_EINVAL;
  if (context->record == NULL || context->shard == NULL)
    return SALTS_ENOENT;

  salts_mutex_lock(&context->shard->mutex);
  if (!chttp_session_context_record_valid_locked(context)) {
    salts_mutex_unlock(&context->shard->mutex);
    return SALTS_ENOENT;
  }
  for (index = 0u;
       index < context->server->config.session_entry_capacity; ++index) {
    chttp_session_entry *entry = &context->record->entries[index];
    if (entry->used && strcmp(entry->key, key) == 0) {
      entry->used = false;
      entry->key[0] = '\0';
      entry->value[0] = '\0';
      context->record->expires_at_ms =
          chttp_session_expiry(context->server, salts_monotonic_ms());
      salts_mutex_unlock(&context->shard->mutex);

      for (index = 0u;
           index < context->server->config.session_entry_capacity; ++index) {
        chttp_session_entry *snapshot = &context->snapshot_entries[index];
        if (snapshot->used && strcmp(snapshot->key, key) == 0) {
          snapshot->used = false;
          snapshot->key[0] = '\0';
          snapshot->value[0] = '\0';
          break;
        }
      }
      return SALTS_OK;
    }
  }
  salts_mutex_unlock(&context->shard->mutex);
  return SALTS_ENOENT;
}

int chttp_session_clear(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  if (context == NULL || context->invalidated) return SALTS_EINVAL;
  if (context->record == NULL || context->shard == NULL) {
    chttp_session_snapshot_clear(context);
    return SALTS_OK;
  }

  salts_mutex_lock(&context->shard->mutex);
  if (!chttp_session_context_record_valid_locked(context)) {
    salts_mutex_unlock(&context->shard->mutex);
    return SALTS_ENOENT;
  }
  chttp_session_entries_clear(context->server, context->record->entries);
  context->record->expires_at_ms =
      chttp_session_expiry(context->server, salts_monotonic_ms());
  salts_mutex_unlock(&context->shard->mutex);
  chttp_session_snapshot_clear(context);
  return SALTS_OK;
}

static void chttp_session_lock_pair(
    chttp_server_impl *server,
    size_t first_index, size_t second_index) {
  if (first_index == second_index) {
    salts_mutex_lock(&server->session_shards[first_index].mutex);
    return;
  }
  if (first_index < second_index) {
    salts_mutex_lock(&server->session_shards[first_index].mutex);
    salts_mutex_lock(&server->session_shards[second_index].mutex);
  } else {
    salts_mutex_lock(&server->session_shards[second_index].mutex);
    salts_mutex_lock(&server->session_shards[first_index].mutex);
  }
}

static void chttp_session_unlock_pair(
    chttp_server_impl *server,
    size_t first_index, size_t second_index) {
  if (first_index == second_index) {
    salts_mutex_unlock(&server->session_shards[first_index].mutex);
    return;
  }
  if (first_index < second_index) {
    salts_mutex_unlock(&server->session_shards[second_index].mutex);
    salts_mutex_unlock(&server->session_shards[first_index].mutex);
  } else {
    salts_mutex_unlock(&server->session_shards[first_index].mutex);
    salts_mutex_unlock(&server->session_shards[second_index].mutex);
  }
}

int chttp_session_regenerate(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  char id[CHTTP_SESSION_ID_TEXT_BYTES + 1u];
  size_t attempt;
  int status;

  if (context == NULL || context->invalidated) return SALTS_EINVAL;
  if (context->record == NULL) return chttp_session_create(context);

  for (attempt = 0u; attempt < CHTTP_SESSION_ID_ATTEMPTS; ++attempt) {
    chttp_server_impl *server = context->server;
    chttp_session_shard *source_shard = context->shard;
    chttp_session_shard *target_shard;
    chttp_session_record *target_record;
    size_t source_index;
    size_t target_index;
    uint64_t now_ms = salts_monotonic_ms();

    status = chttp_session_random_id(id);
    if (status != SALTS_OK) return status;
    target_shard = chttp_session_shard_for_id(server, id);
    if (source_shard == NULL || target_shard == NULL) return SALTS_EINVAL;
    source_index = (size_t)(source_shard - server->session_shards);
    target_index = (size_t)(target_shard - server->session_shards);

    chttp_session_lock_pair(server, source_index, target_index);
    if (!chttp_session_context_record_valid_locked(context)) {
      chttp_session_unlock_pair(server, source_index, target_index);
      return SALTS_ENOENT;
    }

    if (source_shard != target_shard)
      chttp_session_shard_expire_locked(server, target_shard, now_ms);
    if (chttp_session_record_find_locked(server, target_shard, id) != NULL) {
      chttp_session_unlock_pair(server, source_index, target_index);
      continue;
    }

    if (source_shard == target_shard) {
      context->record->generation =
          chttp_session_next_generation(context->record->generation);
      memcpy(context->record->id, id, sizeof(id));
      context->record->expires_at_ms = chttp_session_expiry(server, now_ms);
      context->record_generation = context->record->generation;
      memcpy(context->id, id, sizeof(context->id));
      context->created = true;
      context->presented = false;
      chttp_session_unlock_pair(server, source_index, target_index);
      return SALTS_OK;
    }

    target_record = chttp_session_record_free_locked(server, target_shard);
    if (target_record == NULL) {
      chttp_session_unlock_pair(server, source_index, target_index);
      continue;
    }

    chttp_session_record_prepare(
        server, target_record, id, chttp_session_expiry(server, now_ms));
    chttp_session_record_copy_entries(server, target_record, context->record);
    chttp_session_record_clear(server, context->record);
    context->shard = target_shard;
    context->record = target_record;
    context->record_generation = target_record->generation;
    memcpy(context->id, id, sizeof(context->id));
    context->created = true;
    context->presented = false;
    chttp_session_unlock_pair(server, source_index, target_index);
    return SALTS_OK;
  }

  return SALTS_ENOBUFS;
}

int chttp_session_invalidate(chttp_session *session) {
  chttp_session_context *context = chttp_session_context_get(session);
  if (context == NULL) return SALTS_EINVAL;
  if (context->record != NULL && context->shard != NULL) {
    salts_mutex_lock(&context->shard->mutex);
    if (chttp_session_context_record_valid_locked(context))
      chttp_session_record_clear(context->server, context->record);
    salts_mutex_unlock(&context->shard->mutex);
  }
  context->record = NULL;
  context->shard = NULL;
  context->record_generation = 0u;
  context->id[0] = '\0';
  context->invalidated = true;
  return SALTS_OK;
}

static int chttp_session_set_cookie(
    chttp_server_request_state *state, const char *id,
    uint32_t max_age_seconds) {
  chttp_server_impl *server = state->server;
  char cookie[CHTTP_SESSION_COOKIE_BYTES];
  int cookie_size = snprintf(
      cookie, sizeof(cookie),
      "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%u%s",
      server->session_cookie_name, id, (unsigned int)max_age_seconds,
      server->config.session_cookie_secure ? "; Secure" : "");
  if (cookie_size < 0 || (size_t)cookie_size >= sizeof(cookie))
    return SALTS_EMSGSIZE;
  return chttp_server_response_set_header(
      &state->response, "Set-Cookie", cookie);
}

static bool chttp_session_context_refresh(
    chttp_session_context *context) {
  bool valid;
  if (context == NULL || context->record == NULL || context->shard == NULL)
    return false;
  salts_mutex_lock(&context->shard->mutex);
  valid = chttp_session_context_record_valid_locked(context);
  if (valid)
    context->record->expires_at_ms =
        chttp_session_expiry(context->server, salts_monotonic_ms());
  salts_mutex_unlock(&context->shard->mutex);
  return valid;
}

static void chttp_session_created_discard(
    chttp_session_context *context) {
  if (context == NULL || !context->created ||
      context->record == NULL || context->shard == NULL)
    return;
  salts_mutex_lock(&context->shard->mutex);
  if (chttp_session_context_record_valid_locked(context))
    chttp_session_record_clear(context->server, context->record);
  salts_mutex_unlock(&context->shard->mutex);
  context->record = NULL;
  context->shard = NULL;
  context->record_generation = 0u;
  context->id[0] = '\0';
}

int chttp_session_request_finish(chttp_server_request_state *state) {
  chttp_session_context *context;
  uint32_t max_age_seconds;
  int status;

  if (state == NULL || state->server == NULL ||
      state->server->config.session_capacity == 0u)
    return SALTS_OK;

  context = &state->session_context;
  if (context->invalidated)
    return chttp_session_set_cookie(state, "", 0u);
  if (context->record == NULL) return SALTS_OK;

  if (!chttp_session_context_refresh(context)) {
    context->record = NULL;
    context->shard = NULL;
    context->record_generation = 0u;
    context->id[0] = '\0';
    return context->presented || context->created
               ? chttp_session_set_cookie(state, "", 0u)
               : SALTS_OK;
  }

  max_age_seconds = state->server->config.session_idle_timeout_ms / 1000u;
  if (state->server->config.session_idle_timeout_ms % 1000u != 0u)
    ++max_age_seconds;
  if (max_age_seconds == 0u) max_age_seconds = 1u;

  status = chttp_session_set_cookie(state, context->id, max_age_seconds);
  if (status != SALTS_OK && context->created)
    chttp_session_created_discard(context);
  return status;
}

void chttp_session_request_abort(chttp_server_request_state *state) {
  chttp_session_context *context;
  if (state == NULL || state->server == NULL ||
      state->server->config.session_capacity == 0u)
    return;
  context = &state->session_context;
  if (context->created) chttp_session_created_discard(context);
  context->record = NULL;
  context->shard = NULL;
  context->record_generation = 0u;
  context->id[0] = '\0';
}
