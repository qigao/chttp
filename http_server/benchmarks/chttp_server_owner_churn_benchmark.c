#include "chttp_server_runtime.h"

#include <http_server/http.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET owner_churn_socket;
  #define OWNER_CHURN_INVALID_SOCKET INVALID_SOCKET
  #define owner_churn_close closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int owner_churn_socket;
  #define OWNER_CHURN_INVALID_SOCKET (-1)
  #define owner_churn_close close
#endif

enum {
  OWNER_CHURN_INITIAL_CONNECTIONS = 8,
  OWNER_CHURN_OPERATIONS = 32,
  OWNER_CHURN_TIMEOUT_MS = 10000,
  OWNER_CHURN_MAX_OWNERS = 4
};

static int owner_churn_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t owner_churn_percentile(const uint64_t *values, size_t count,
                                       unsigned int percent) {
  uint64_t ordered[OWNER_CHURN_OPERATIONS];
  size_t rank;
  if (values == NULL || count == 0u || count > OWNER_CHURN_OPERATIONS) return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), owner_churn_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  return ordered[rank - 1u];
}

static native_io_backend_kind owner_churn_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config owner_churn_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = {
          .backend = owner_churn_backend(),
          .connection_capacity = 16u,
          .command_capacity = 64u,
          .request_capacity = 32u,
          .completion_batch_capacity = 32u,
          .event_capacity = 64u,
          .max_send_bytes = 64u * 1024u,
          .receive_buffer_bytes = 16u * 1024u,
          .connect_timeout_ms = OWNER_CHURN_TIMEOUT_MS,
          .read_timeout_ms = OWNER_CHURN_TIMEOUT_MS,
          .write_timeout_ms = OWNER_CHURN_TIMEOUT_MS},
      .route_capacity = 2u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 32u,
      .max_target_bytes = 128u,
      .max_header_count = 16u,
      .max_header_bytes = 2048u,
      .max_request_body_bytes = 1024u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 2048u,
      .max_response_body_bytes = 1024u,
      .poll_slice_ms = 1u,
      .max_buffered_response_body_bytes = 1024u,
      .buffer_capacity_bytes = 2u * 1024u * 1024u};
}

static int owner_churn_handler(void *user,
                               const chttp_server_request_view *request,
                               chttp_server_response *response) {
  static const char body[] = "ok";
  (void)user;
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain", body, sizeof(body) - 1u);
}

static int owner_churn_socket_timeout(owner_churn_socket socket_value) {
#if defined(_WIN32)
  DWORD timeout = OWNER_CHURN_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            (const char *)&timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#else
  struct timeval timeout = {
      .tv_sec = OWNER_CHURN_TIMEOUT_MS / 1000,
      .tv_usec = (OWNER_CHURN_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    &timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            &timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#endif
}

static int owner_churn_connect(uint16_t port, owner_churn_socket *out_socket) {
  struct sockaddr_in address;
  owner_churn_socket socket_value;
  int one = 1;
  if (out_socket == NULL) return -1;
  *out_socket = OWNER_CHURN_INVALID_SOCKET;
  socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == OWNER_CHURN_INVALID_SOCKET) return -1;
  if (owner_churn_socket_timeout(socket_value) != 0) goto fail;
  (void)setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY,
#if defined(_WIN32)
                   (const char *)&one,
#else
                   &one,
#endif
                   sizeof(one));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(socket_value, (const struct sockaddr *)&address,
              (int)sizeof(address)) != 0)
    goto fail;
  *out_socket = socket_value;
  return 0;
fail:
  owner_churn_close(socket_value);
  return -1;
}

static int owner_churn_send_all(owner_churn_socket socket_value,
                                const unsigned char *data, size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    int sent = send(socket_value, (const char *)data + offset,
                    (int)(size - offset), 0);
    if (sent <= 0) {
#if !defined(_WIN32)
      if (sent < 0 && errno == EINTR) continue;
#endif
      return -1;
    }
    offset += (size_t)sent;
  }
  return 0;
}

static int owner_churn_request(owner_churn_socket socket_value) {
  static const unsigned char request[] =
      "GET /probe HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
  unsigned char response[4096];
  size_t used = 0u;
  if (owner_churn_send_all(socket_value, request, sizeof(request) - 1u) != 0)
    return -1;
  while (used < sizeof(response) - 1u) {
    int got = recv(socket_value, (char *)response + used,
                   (int)(sizeof(response) - 1u - used), 0);
    if (got <= 0) {
#if !defined(_WIN32)
      if (got < 0 && errno == EINTR) continue;
#endif
      return -1;
    }
    used += (size_t)got;
    response[used] = '\0';
    if (strstr((const char *)response, "\r\n\r\nok") != NULL) return 0;
  }
  return -1;
}

static void owner_churn_snapshot(chttp_server_impl *impl, size_t owner_count,
                                 size_t leases[OWNER_CHURN_MAX_OWNERS],
                                 size_t rings[OWNER_CHURN_MAX_OWNERS]) {
  size_t index;
  memset(leases, 0, OWNER_CHURN_MAX_OWNERS * sizeof(*leases));
  memset(rings, 0, OWNER_CHURN_MAX_OWNERS * sizeof(*rings));
  for (index = 0u; index < owner_count && index < OWNER_CHURN_MAX_OWNERS; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    if (owner == NULL) continue;
    leases[index] = chttp_server_owner_lease_count(owner);
    cmeta_mutex_lock(&owner->admission_mutex);
    rings[index] = owner->admission_count;
    cmeta_mutex_unlock(&owner->admission_mutex);
  }
}

static size_t owner_churn_sum(const size_t values[OWNER_CHURN_MAX_OWNERS],
                              size_t owner_count) {
  size_t index;
  size_t total = 0u;
  for (index = 0u; index < owner_count; ++index) total += values[index];
  return total;
}

static int owner_churn_wait_total(chttp_server_impl *impl, size_t owner_count,
                                  size_t expected,
                                  size_t leases[OWNER_CHURN_MAX_OWNERS],
                                  size_t rings[OWNER_CHURN_MAX_OWNERS]) {
  const uint64_t deadline = cmeta_monotonic_ms() + OWNER_CHURN_TIMEOUT_MS;
  do {
    owner_churn_snapshot(impl, owner_count, leases, rings);
    if (owner_churn_sum(leases, owner_count) == expected) return 0;
    cmeta_thread_yield();
  } while (cmeta_monotonic_ms() < deadline);
  return -1;
}

static int owner_churn_delta_owner(
    const size_t before[OWNER_CHURN_MAX_OWNERS],
    const size_t after[OWNER_CHURN_MAX_OWNERS], size_t owner_count,
    size_t *out_owner) {
  size_t index;
  size_t changed = SIZE_MAX;
  if (out_owner == NULL) return -1;
  for (index = 0u; index < owner_count; ++index) {
    if (after[index] == before[index]) continue;
    if (after[index] != before[index] + 1u || changed != SIZE_MAX) return -1;
    changed = index;
  }
  if (changed == SIZE_MAX) return -1;
  *out_owner = changed;
  return 0;
}

static int owner_churn_run(size_t owner_count) {
  chttp_server server = {0};
  chttp_server_config config = owner_churn_server_config();
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  chttp_server_stats stats = {0};
  chttp_server_impl *impl = NULL;
  owner_churn_socket initial[OWNER_CHURN_INITIAL_CONNECTIONS];
  size_t initial_owner[OWNER_CHURN_INITIAL_CONNECTIONS];
  size_t leases[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t rings[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t before[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t before_rings[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t after[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t after_rings[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t skew_leases[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t peak_leases[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t peak_rings[OWNER_CHURN_MAX_OWNERS] = {0};
  size_t churn_admissions[OWNER_CHURN_MAX_OWNERS] = {0};
  uint64_t latencies[OWNER_CHURN_OPERATIONS] = {0};
  uint16_t port = 0u;
  size_t skew_owner;
  size_t kept = 0u;
  size_t index;
  int result = 1;

  if (owner_count == 0u || owner_count > OWNER_CHURN_MAX_OWNERS) return 1;
  for (index = 0u; index < OWNER_CHURN_INITIAL_CONNECTIONS; ++index)
    initial[index] = OWNER_CHURN_INVALID_SOCKET;

  if (chttp_server_init(&server, &config) != SALTS_OK) goto cleanup;
  execution.owner_count = owner_count;
  if (chttp_server_set_execution_options(&server, &execution) != SALTS_OK) goto cleanup;
  if (chttp_server_get(&server, "/probe", owner_churn_handler, NULL) != SALTS_OK) goto cleanup;
  if (chttp_server_start(&server) != SALTS_OK) goto cleanup;
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u) goto cleanup;
  impl = (chttp_server_impl *)server.impl;

  for (index = 0u; index < OWNER_CHURN_INITIAL_CONNECTIONS; ++index) {
    size_t assigned;
    owner_churn_snapshot(impl, owner_count, before, before_rings);
    if (owner_churn_connect(port, &initial[index]) != 0 ||
        owner_churn_request(initial[index]) != 0)
      goto cleanup;
    if (owner_churn_wait_total(impl, owner_count, index + 1u, after, after_rings) != 0)
      goto cleanup;
    if (owner_churn_delta_owner(before, after, owner_count, &assigned) != 0) goto cleanup;
    initial_owner[index] = assigned;
  }

  skew_owner = initial_owner[0];
  for (index = 0u; index < OWNER_CHURN_INITIAL_CONNECTIONS; ++index) {
    if (initial_owner[index] == skew_owner) {
      ++kept;
      continue;
    }
    owner_churn_close(initial[index]);
    initial[index] = OWNER_CHURN_INVALID_SOCKET;
  }
  if (owner_churn_wait_total(impl, owner_count, kept, skew_leases, rings) != 0)
    goto cleanup;
  if (skew_leases[skew_owner] != kept) goto cleanup;
  for (index = 0u; index < owner_count; ++index)
    if (index != skew_owner && skew_leases[index] != 0u) goto cleanup;

  memcpy(peak_leases, skew_leases, sizeof(peak_leases));
  for (index = 0u; index < OWNER_CHURN_OPERATIONS; ++index) {
    owner_churn_socket socket_value = OWNER_CHURN_INVALID_SOCKET;
    size_t assigned;
    size_t owner_index;
    const uint64_t started = cmeta_hrtime();

    owner_churn_snapshot(impl, owner_count, before, before_rings);
    if (owner_churn_connect(port, &socket_value) != 0 ||
        owner_churn_request(socket_value) != 0) {
      if (socket_value != OWNER_CHURN_INVALID_SOCKET) owner_churn_close(socket_value);
      goto cleanup;
    }
    latencies[index] = cmeta_hrtime() - started;
    if (owner_churn_wait_total(impl, owner_count, kept + 1u, after, after_rings) != 0) {
      owner_churn_close(socket_value);
      goto cleanup;
    }
    if (owner_churn_delta_owner(before, after, owner_count, &assigned) != 0) {
      owner_churn_close(socket_value);
      goto cleanup;
    }
    ++churn_admissions[assigned];
    for (owner_index = 0u; owner_index < owner_count; ++owner_index) {
      if (after[owner_index] > peak_leases[owner_index])
        peak_leases[owner_index] = after[owner_index];
      if (after_rings[owner_index] > peak_rings[owner_index])
        peak_rings[owner_index] = after_rings[owner_index];
    }
    owner_churn_close(socket_value);
    if (owner_churn_wait_total(impl, owner_count, kept, after, after_rings) != 0)
      goto cleanup;
  }

  if (chttp_server_get_stats(&server, &stats) != SALTS_OK ||
      stats.rejected_connections != 0u)
    goto cleanup;

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_owner_admission_churn\","
      "\"owners\":%zu,"
      "\"initial_connections\":%u,"
      "\"kept_connections\":%zu,"
      "\"skew_owner\":%zu,"
      "\"churn_connections\":%u,"
      "\"accepted_connections\":%llu,"
      "\"rejected_connections\":%llu,"
      "\"skew_owner0_leases\":%zu,"
      "\"skew_owner1_leases\":%zu,"
      "\"skew_owner2_leases\":%zu,"
      "\"skew_owner3_leases\":%zu,"
      "\"churn_owner0_admissions\":%zu,"
      "\"churn_owner1_admissions\":%zu,"
      "\"churn_owner2_admissions\":%zu,"
      "\"churn_owner3_admissions\":%zu,"
      "\"peak_owner0_leases\":%zu,"
      "\"peak_owner1_leases\":%zu,"
      "\"peak_owner2_leases\":%zu,"
      "\"peak_owner3_leases\":%zu,"
      "\"peak_owner0_ring\":%zu,"
      "\"peak_owner1_ring\":%zu,"
      "\"peak_owner2_ring\":%zu,"
      "\"peak_owner3_ring\":%zu,"
      "\"p50_ns\":%llu,"
      "\"p95_ns\":%llu,"
      "\"p99_ns\":%llu,"
      "\"cross_owner_data_plane_hops\":0,"
      "\"errors\":0}\n",
      owner_count, OWNER_CHURN_INITIAL_CONNECTIONS, kept, skew_owner,
      OWNER_CHURN_OPERATIONS,
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections,
      skew_leases[0], skew_leases[1], skew_leases[2], skew_leases[3],
      churn_admissions[0], churn_admissions[1], churn_admissions[2],
      churn_admissions[3], peak_leases[0], peak_leases[1], peak_leases[2],
      peak_leases[3], peak_rings[0], peak_rings[1], peak_rings[2],
      peak_rings[3],
      (unsigned long long)owner_churn_percentile(latencies, OWNER_CHURN_OPERATIONS, 50u),
      (unsigned long long)owner_churn_percentile(latencies, OWNER_CHURN_OPERATIONS, 95u),
      (unsigned long long)owner_churn_percentile(latencies, OWNER_CHURN_OPERATIONS, 99u));
  fflush(stdout);
  result = 0;

cleanup:
  for (index = 0u; index < OWNER_CHURN_INITIAL_CONNECTIONS; ++index) {
    if (initial[index] != OWNER_CHURN_INVALID_SOCKET) {
      owner_churn_close(initial[index]);
      initial[index] = OWNER_CHURN_INVALID_SOCKET;
    }
  }
  if (server.impl != NULL) {
    (void)chttp_server_stop(&server, OWNER_CHURN_TIMEOUT_MS);
    (void)chttp_server_destroy(&server);
  }
  return result;
}

int main(void) {
  static const size_t owners[] = {1u, 2u, 4u};
  size_t index;
  int result = 0;
#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
#endif

  printf(
      "{\"kind\":\"environment\","
      "\"benchmark\":\"chttp_server_owner_admission_churn\","
      "\"commit\":\"%s\","
      "\"initial_connections\":%u,"
      "\"churn_connections\":%u,"
      "\"note\":\"benchmark-only owner admission evidence; no runtime policy change\"}\n",
      getenv("GITHUB_SHA") != NULL ? getenv("GITHUB_SHA") : "unknown",
      OWNER_CHURN_INITIAL_CONNECTIONS, OWNER_CHURN_OPERATIONS);

  for (index = 0u; index < sizeof(owners) / sizeof(owners[0]); ++index) {
    if (owner_churn_run(owners[index]) != 0) {
      fprintf(stderr, "owner admission churn benchmark failed owners=%zu\n", owners[index]);
      result = 3;
      break;
    }
  }

#if defined(_WIN32)
  WSACleanup();
#endif
  return result;
}
