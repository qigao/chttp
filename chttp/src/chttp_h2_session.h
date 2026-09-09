#ifndef CHTTP_H2_SESSION_H
#define CHTTP_H2_SESSION_H

#include "chttp_file_sink.h"
#include "chttp_file_transfer.h"
#include "chttp_h2_proto.h"
#include "chttp_internal.h"
#include "chttp_tls.h"

#include <stdbool.h>

typedef enum chttp_h2_session_state {
  CHTTP_H2_SESSION_FREE = 0,
  CHTTP_H2_SESSION_CONNECTING,
  CHTTP_H2_SESSION_ACTIVE,
  CHTTP_H2_SESSION_DRAINING,
  CHTTP_H2_SESSION_CLOSING,
  CHTTP_H2_SESSION_TERMINAL
} chttp_h2_session_state;

typedef struct chttp_h2_session chttp_h2_session;

typedef struct chttp_h2_request_state {
  chttp_h2_session *session;
  void *request_user;
  chttp_response_view response;
  chttp_header *headers;
  char *header_storage;
  unsigned char *response_body;
  unsigned char *request_body;
  chttp_body_source body_source;
  chttp_body_sink body_sink;
  chttp_file_transfer *file_transfer;
  chttp_file_sink_transfer *file_sink_transfer;
  size_t header_storage_capacity;
  size_t header_storage_used;
  size_t header_list_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_response_body_bytes;
  size_t max_informational_responses;
  size_t max_request_body_bytes;
  size_t stream_chunk_bytes;
  size_t source_transferred;
  size_t informational_responses;
  size_t content_length;
  size_t registry_index;
  int32_t stream_id;
  chttp_method method;
  unsigned int current_status;
  int failure_status;
  const char *failure_stage;
  bool header_block_open;
  bool regular_header_seen;
  bool status_seen;
  bool content_length_seen;
  bool final_headers_seen;
  bool trailers;
  bool completed;
  bool terminal_pending;
  bool source_enabled;
  bool sink_enabled;
  bool sink_write_pending;
  size_t sink_flow_bytes;
  int terminal_status;
  int terminal_native_status;
  const char *terminal_stage;
} chttp_h2_request_state;

typedef void (*chttp_h2_session_complete_fn)(void *user, void *request_user,
                                             const chttp_response_view *response, int status,
                                             int native_status, const char *stage);

typedef struct chttp_h2_session_callbacks {
  void *user;
  chttp_h2_session_complete_fn on_complete;
} chttp_h2_session_callbacks;

struct chttp_h2_session {
  cnet_client *network;
  cnet_connection connection;
  chttp_h2_proto *protocol;
  chttp_h2_request_state **requests;
  unsigned char *pending_output;
  char *connection_uri;
  char *authority;
  chttp_tls_profile_impl *tls_profile;
  chttp_limits limits;
  chttp_h2_proto_config protocol_config;
  chttp_h2_session_callbacks callbacks;
  size_t request_capacity;
  size_t active_requests;
  size_t pending_output_size;
  chttp_h2_session_state state;
  bool tls;
  bool receive_armed;
  bool send_active;
  bool close_after_flush;
  bool close_admitted;
  bool close_pending;
  bool defer_completions;
};

int chttp_h2_protocol_config(const chttp_client_config *config, chttp_h2_proto_config *out_config);
int chttp_h2_request_prepare(chttp_h2_request_state *request, const chttp_request_options *options,
                             const chttp_limits *limits, void *request_user);
void chttp_h2_request_destroy(chttp_h2_request_state *request);

int chttp_h2_session_open(chttp_h2_session *session, cnet_client *network,
                          const chttp_request_options *options, chttp_tls_profile_impl *tls_profile,
                          const chttp_h2_proto_config *protocol_config, const chttp_limits *limits,
                          const chttp_h2_session_callbacks *callbacks);
bool chttp_h2_session_matches(const chttp_h2_session *session, const chttp_request_options *options,
                              const chttp_tls_profile_impl *tls_profile);
bool chttp_h2_session_terminal(const chttp_h2_session *session);
int chttp_h2_session_submit(chttp_h2_session *session, chttp_h2_request_state *request,
                            const chttp_request_options *options);
int chttp_h2_session_cancel(chttp_h2_session *session, chttp_h2_request_state *request);
int chttp_h2_session_progress(chttp_h2_session *session);
int chttp_h2_session_resume_file_source(chttp_h2_request_state *request);
int chttp_h2_session_resume_file_sink(chttp_h2_request_state *request);
int chttp_h2_session_begin_stop(chttp_h2_session *session);
bool chttp_h2_session_stop_ready(const chttp_h2_session *session);
void chttp_h2_session_destroy(chttp_h2_session *session);

#endif /* CHTTP_H2_SESSION_H */
