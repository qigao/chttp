#ifndef CHTTP_CLIENT_INTERNAL_H
#define CHTTP_CLIENT_INTERNAL_H

#include <stdint.h>

#include "chttp_file_sink.h"
#include "chttp_file_transfer.h"

int chttp_async_client_file_runtime(chttp_async_client *client,
                                    cflow_io_file_runtime **out_runtime);
int chttp_async_client_file_sink_capacity(chttp_async_client *client, size_t *out_capacity);

/* Benchmark/internal control only: affects future H2 sessions and must be set
 * before any H2 session leaves FREE. Zero selects the RFC default 65,535. */
int chttp_async_client_set_h2_receive_window_policy(
    chttp_async_client *client, uint32_t stream_window,
    uint32_t connection_window);
int chttp_async_client_submit_file(chttp_async_client *client, const chttp_request_options *options,
                                   chttp_file_transfer *transfer, chttp_request *out_request);
int chttp_async_client_submit_file_download(chttp_async_client *client,
                                            const chttp_request_options *options,
                                            chttp_file_sink_transfer *transfer,
                                            chttp_request *out_request);

int chttp_client_file_runtime(chttp_client *client, cflow_io_file_runtime **out_runtime);
int chttp_client_file_sink_capacity(chttp_client *client, size_t *out_capacity);
int chttp_client_perform_file(chttp_client *client, chttp_method method,
                              const chttp_options *options, chttp_file_transfer *transfer,
                              chttp_response *out_response, chttp_error *out_error);
int chttp_client_perform_file_download(chttp_client *client, const chttp_options *options,
                                       chttp_file_sink_transfer *transfer,
                                       chttp_response *out_response, chttp_error *out_error);

#endif /* CHTTP_CLIENT_INTERNAL_H */
