#ifndef CHTTP_LOGICAL_RETRY_H
#define CHTTP_LOGICAL_RETRY_H
#include "chttp_file_sink.h"
#include "chttp_file_transfer.h"
#include <http_client/http.h>
typedef struct chttp_logical_runtime chttp_logical_runtime;
int chttp_logical_runtime_init(chttp_logical_runtime **, chttp_async_client *,
                               const chttp_client_config *);
int chttp_logical_runtime_progress(chttp_logical_runtime *);
uint32_t chttp_logical_runtime_wait(chttp_logical_runtime *, uint32_t);
void chttp_logical_runtime_seal(chttp_logical_runtime *);
bool chttp_logical_runtime_empty(const chttp_logical_runtime *);
void chttp_logical_runtime_destroy(chttp_logical_runtime *);
int chttp_async_client_logical_runtime(chttp_async_client *, chttp_logical_runtime **);
bool chttp_async_request_pending(chttp_async_client *, chttp_request);
void chttp_async_client_logical_complete(chttp_async_client *, chttp_logical_complete_fn, void *,
                                         chttp_logical_request, const chttp_response_view *,
                                         const chttp_error *);
int chttp_async_client_replay_hook(chttp_async_client *, int (*hook)(void *), void *);
int chttp_async_client_submit_logical_attempt(chttp_async_client *, const chttp_request_options *,
                                              chttp_file_transfer *, chttp_file_sink_transfer *,
                                              uint64_t deadline, uint32_t idle_ms, chttp_request *);
#endif
