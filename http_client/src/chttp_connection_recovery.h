#ifndef CHTTP_CONNECTION_RECOVERY_H
#define CHTTP_CONNECTION_RECOVERY_H

#include <http_client/http.h>
#include <cnet/managed_dial.h>
#include <cnet/client_pool.h>

/* One address-stable, owner-local physical session. Protocol state remains
 * owned by H1/H2. The admission hook may reset it only after old logical work
 * and retained writes have settled; CNet independently requires real recycle. */
typedef struct chttp_connection_recovery {
  cnet_managed_dial dial;
  cnet_manager *manager;
  cnet_client_pool *pool;
  cnet_pool_key key;
  cnet_pool_connection *pooled;
  cnet_managed_connection *managed;
  cnet_connection *connection;
  cnet_observer observer;
  int (*reset)(void *user);
  bool (*settled)(void *user);
  void (*fail)(void *user, int status, int native_status);
  void *user;
  cnet_reconnect_ticket ticket;
  bool configured;
  bool stopping;
  bool permanent;
  bool terminal;
  bool ready;
  bool failed_after_ready;
  bool pool_bound;
  bool failure_delivered;
  bool in_callback;
  bool defer_stop;
  bool tls;
  bool transport_connected;
  int last_status;
  int last_native_status;
} chttp_connection_recovery;

int chttp_connection_recovery_validate(const chttp_connection_recovery_options *options);
int chttp_connection_recovery_init(chttp_connection_recovery *recovery,
    const chttp_connection_recovery_options *options, cnet_client *client,
    cnet_manager *manager, cnet_client_pool *pool, const cnet_pool_key *key,
    cnet_pool_connection *pooled, cnet_managed_connection *managed, cnet_connection *connection,
    uint64_t deadline_at_ms, const cnet_connect_options *connect, int (*reset)(void *), bool (*settled)(void *),
    void (*fail)(void *, int, int), void *user);
bool chttp_connection_recovery_opening_retry(chttp_connection_recovery *recovery);
int chttp_connection_recovery_ready(chttp_connection_recovery *recovery);
int chttp_connection_recovery_bind(chttp_connection_recovery *recovery, size_t capacity);
int chttp_connection_recovery_progress(chttp_connection_recovery *recovery);
int chttp_connection_recovery_seal(chttp_connection_recovery *recovery);
uint32_t chttp_connection_recovery_wait(chttp_connection_recovery *recovery, uint32_t wait_ms);

#endif
