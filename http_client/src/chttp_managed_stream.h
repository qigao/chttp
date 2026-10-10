#ifndef CHTTP_MANAGED_STREAM_H
#define CHTTP_MANAGED_STREAM_H

#include <cnet/manager.h>

/* One dedicated WS physical stream. The host keeps its protocol storage and
 * observer alive through retire; Manager owns transport admission/terminal
 * truth and close retries. No worker, protocol pool or recovery loop is added. */
typedef struct chttp_managed_stream {
  cnet_manager manager;
  cnet_managed_connection managed;
} chttp_managed_stream;

static inline int chttp_managed_stream_init(chttp_managed_stream *stream,
                                             cnet_client *client) {
  const cnet_manager_config config = {
      sizeof(config), CNET_MANAGER_VERSION, client, 1u, 1u};
  return cnet_manager_init(&stream->manager, &config);
}

/* Called only after protocol callbacks/storage settle, or synchronous connect
 * rejection. Never releases the extra context hold of a live connection. */
static inline int chttp_managed_stream_retire(chttp_managed_stream *stream) {
  cnet_manager_entry entry;
  size_t work = 0u;
  int status;
  if (stream->managed.slot == 0u) return SALTS_OK;
  status = cnet_manager_lookup(&stream->manager, stream->managed, &entry);
  if (status != SALTS_OK) return status;
  if (entry.state != CNET_MANAGER_RETIRED) return SALTS_EBUSY;
  if (entry.context_held) {
    status = cnet_manager_release_context(&stream->manager, stream->managed);
    if (status != SALTS_OK) return status;
  }
  status = cnet_manager_advance(&stream->manager, 1u, &work);
  if (status != SALTS_OK) return status;
  status = cnet_manager_lookup(&stream->manager, stream->managed, &entry);
  if (status != SALTS_ENOENT) return status == SALTS_OK ? SALTS_EBUSY : status;
  stream->managed = (cnet_managed_connection){0};
  return SALTS_OK;
}

static inline int chttp_managed_stream_connect(chttp_managed_stream *stream,
    const cnet_connect_options *options, cnet_connection *out) {
  const cnet_manager_attachment attachment = {
      .observer = options->observer, .hold_context = true};
  if (stream->managed.slot != 0u) return SALTS_EALREADY;
  int status = cnet_manager_reserve(&stream->manager, &attachment, &stream->managed);
  if (status != SALTS_OK) return status;
  status = cnet_manager_connect(&stream->manager, stream->managed, options, out);
  if (status != SALTS_OK) {
    const int cleanup = chttp_managed_stream_retire(stream);
    if (cleanup != SALTS_OK) return cleanup;
  }
  return status;
}

static inline int chttp_managed_stream_progress(chttp_managed_stream *stream) {
  size_t work = 0u;
  return cnet_manager_advance(&stream->manager, 1u, &work);
}

static inline int chttp_managed_stream_close(chttp_managed_stream *stream) {
  int status = cnet_manager_request_close(&stream->manager);
  if (status != SALTS_OK) return status;
  return chttp_managed_stream_progress(stream);
}

/* Client must still be initialized. A failed destroy retains this adapter and
 * its observer/context so the same Owner can continue progress and retry. */
static inline int chttp_managed_stream_destroy(chttp_managed_stream *stream) {
  if (stream->manager.impl == NULL) return SALTS_OK;
  const int status = chttp_managed_stream_retire(stream);
  return status == SALTS_OK ? cnet_manager_destroy(&stream->manager) : status;
}

#endif
