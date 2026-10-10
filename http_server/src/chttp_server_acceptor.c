#include "chttp_server_runtime.h"

/* Listener-side Acceptor: descriptor ownership transfers only on successful
 * handoff publication. Protocol state is initialized by the receiving Owner. */
static int chttp_server_owner_admission_enqueue(
    chttp_server_owner_lane *owner, cnet_handoff_ticket ticket,
    cnet_accepted_stream *accepted) {
  if (chttp_server_owner_runtime_state_get(owner) !=
      CHTTP_SERVER_OWNER_RUNTIME_READY)
    return SALTS_ESHUTDOWN;
  return cnet_handoff_publish(&owner->handoff, ticket, accepted);
}

int chttp_server_acceptor_cancel_pending(
    chttp_server_owner_lane *owner) {
  cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
  cnet_handoff_ticket ticket;
  int first_status = SALTS_OK;
  int status = cnet_handoff_seal(&owner->handoff);
  if (status != SALTS_OK) return status;
  while ((status = cnet_handoff_take(&owner->handoff, &ticket, &accepted)) == SALTS_OK) {
    const int close_status = cnet_accepted_stream_close(&accepted);
    const int release_status = cnet_handoff_release(&owner->handoff, ticket);
    if (first_status == SALTS_OK && close_status != SALTS_OK) first_status = close_status;
    if (first_status == SALTS_OK && release_status != SALTS_OK) first_status = release_status;
  }
  return first_status != SALTS_OK ? first_status : status == SALTS_ENOENT ? SALTS_OK : status;
}

void chttp_server_stats_rejected_connection(chttp_server_impl *server) {
  cmeta_mutex_lock(&server->mutex);
  ++server->stats.rejected_connections;
  cmeta_mutex_unlock(&server->mutex);
}

static int chttp_server_admission_owner(
    chttp_server_impl *server, cnet_handoff_ticket *ticket,
    chttp_server_owner_lane **out_owner) {
  cnet_owner_placement_input selection;
  cnet_owner_placement_kind kind;
  size_t index;
  size_t attempt;
  int status;
  if (ticket == NULL || out_owner == NULL) return SALTS_EINVAL;
  *out_owner = NULL;
  if (server == NULL || server->owner_count == 0u ||
      server->acceptor.placement_hints == NULL)
    return SALTS_EINVAL;
  kind = server->owner_placement_options.kind;

  /*
   * The listener is a separate control thread, so even data-owner0 uses the
   * bounded cross-thread handoff. CNet placement is pure and advisory; actual
   * capacity is committed only by the generation-checked handoff reservation.
   * The scratch array is allocated when the stopped Owner topology is set.
   */
  for (index = 0u; index < server->owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(server, index);
    const bool eligible =
        owner != NULL &&
        chttp_server_owner_runtime_state_get(owner) ==
            CHTTP_SERVER_OWNER_RUNTIME_READY;
    uint64_t pressure = 0u;
    if (eligible && kind == CNET_OWNER_PLACE_LOWEST_PRESSURE) {
      cnet_handoff_snapshot snapshot = {0};
      size_t held;
      const size_t capacity = owner->connection_count;
      status = cnet_handoff_get_snapshot(&owner->handoff, &snapshot);
      if (status != SALTS_OK) return status;
      if (capacity == 0u || snapshot.reserved > capacity ||
          snapshot.queued > capacity - snapshot.reserved ||
          snapshot.taken > capacity - snapshot.reserved - snapshot.queued)
        return SALTS_EPROTO;
      held = snapshot.reserved + snapshot.queued + snapshot.taken;
      /* Normalize differing partition capacities without overflowing when
       * capacity is large. Owner selection never reserves a connection. */
      pressure = (UINT64_MAX / (uint64_t)capacity) * (uint64_t)held;
    }
    server->acceptor.placement_hints[index] = (cnet_owner_placement_hint){
        .eligible = eligible, .pressure = pressure};
  }
  selection = (cnet_owner_placement_input){
      .size = sizeof(selection),
      .version = CNET_OWNER_PLACEMENT_VERSION,
      .kind = kind,
      .owners = server->acceptor.placement_hints,
      .owner_count = server->owner_count,
      .sequence = server->acceptor.admission_cursor,
      .explicit_owner = server->owner_placement_options.explicit_owner};
  for (attempt = 0u; attempt < server->owner_count; ++attempt) {
    chttp_server_owner_lane *owner;
    index = SIZE_MAX;
    status = cnet_owner_placement_choose(&selection, &index);
    if (status != SALTS_OK) return status;
    owner = chttp_server_owner_at(server, index);
    if (owner == NULL) return SALTS_EPROTO;
    status = cnet_handoff_reserve(&owner->handoff, ticket);
    if (status == SALTS_ENOBUFS || status == SALTS_ESHUTDOWN) {
      /* A pinned Owner never spills on FULL. Other policies can retry the
       * remaining eligible Owners after a real reservation failure. */
      if (kind == CNET_OWNER_PLACE_EXPLICIT) return SALTS_ENOBUFS;
      server->acceptor.placement_hints[index].eligible = false;
      continue;
    }
    if (status != SALTS_OK) return status;
    server->acceptor.admission_cursor = (index + 1u) % server->owner_count;
    *out_owner = owner;
    return SALTS_OK;
  }
  return SALTS_ENOBUFS;
}

static int chttp_server_listener_progress(chttp_server_impl *server) {
  size_t attempts;
  if (server == NULL || !server->acceptor.initialized) return SALTS_EINVAL;
  for (attempts = 0u;
       attempts < server->config.network.connection_capacity;
       ++attempts) {
    cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
    cnet_handoff_ticket ticket = {0};
    chttp_server_owner_lane *owner;
    cnet_client *network;
    int status;
    if (chttp_server_should_stop(server)) return SALTS_OK;
    status = cnet_listener_accept_detached(&server->acceptor.listener, &accepted);
    if (status == SALTS_ETIMEDOUT) return SALTS_OK;
    if (status == SALTS_ENOBUFS) {
      chttp_server_stats_rejected_connection(server);
      return SALTS_OK;
    }
    if (status != SALTS_OK) return status;

    status = chttp_server_admission_owner(server, &ticket, &owner);
    if (status != SALTS_OK) {
      (void)cnet_accepted_stream_close(&accepted);
      if (status != SALTS_ENOBUFS) return status;
      chttp_server_stats_rejected_connection(server);
      continue;
    }
    status = chttp_server_owner_admission_enqueue(owner, ticket, &accepted);
    if (status != SALTS_OK) {
      (void)cnet_accepted_stream_close(&accepted);
      (void)cnet_handoff_release(&owner->handoff, ticket);
      if (status != SALTS_ESHUTDOWN)
        chttp_server_stats_rejected_connection(server);
      if (status == SALTS_ENOBUFS) continue;
      if (status == SALTS_ESHUTDOWN && chttp_server_should_stop(server))
        return SALTS_OK;
      return status;
    }
    network = chttp_server_owner_network(owner);
    if (network == NULL) return SALTS_EPROTO;
    {
      status = cnet_client_wake(network);
      /* Publication already transferred the descriptor and credit. On wake
       * failure stop admission and let the owner cancel its inbox after the
       * listener_done barrier; never close/release the published item here. */
      if (status != SALTS_OK) return status;
    }
  }
  return SALTS_OK;
}

int chttp_server_acceptor_destroy(chttp_server_impl *server) {
  int first_status = SALTS_OK;
  int status;
  if (server == NULL) return SALTS_EINVAL;
  if (!server->acceptor.initialized) return SALTS_OK;

  status = cnet_listener_close(&server->acceptor.listener);
  if (status != SALTS_OK && status != SALTS_EALREADY)
    first_status = status;

  status = cnet_listener_destroy(&server->acceptor.listener);
  if (first_status == SALTS_OK && status != SALTS_OK)
    first_status = status;
  if (status == SALTS_OK) server->acceptor.initialized = false;
  return first_status;
}

int chttp_server_acceptor_open(
    chttp_server_impl *server, uint16_t *out_port) {
  cnet_listener_config listener_config;
  int status;
  if (server == NULL || out_port == NULL) return SALTS_EINVAL;
  *out_port = 0u;
  listener_config = (cnet_listener_config){
      .backend = server->config.network.backend,
      .host = server->host,
      .port = server->config.port,
      .backlog = server->config.backlog};

  status = cnet_listener_init_ex(
      &server->acceptor.listener, &listener_config, &server->socket_options.listener);
  if (status != SALTS_OK) return status;
  server->acceptor.initialized = true;
  return cnet_listener_port(&server->acceptor.listener, out_port);
}

int chttp_server_acceptor_poll(chttp_server_impl *server, uint32_t timeout_ms) {
  int ready = 0;
  int status = cnet_listener_wait(&server->acceptor.listener, timeout_ms, &ready);
  if (status != SALTS_OK || !ready) return status;
  return chttp_server_listener_progress(server);
}

int chttp_server_acceptor_close(chttp_server_impl *server) {
  return cnet_listener_close(&server->acceptor.listener);
}
