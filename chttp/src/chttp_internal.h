#ifndef CHTTP_INTERNAL_H
#define CHTTP_INTERNAL_H

#include <chttp/chttp.h>
#include <llhttp.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct chttp_limits {
  size_t max_start_line_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_request_body_bytes;
  size_t max_response_body_bytes;
  size_t max_informational_responses;
  size_t max_request_bytes;
  size_t stream_chunk_bytes;
} chttp_limits;

typedef struct chttp_file_sink_transfer chttp_file_sink_transfer;

typedef struct chttp_response_parser {
  llhttp_t parser;
  llhttp_settings_t settings;
  chttp_response_view response;
  chttp_header *headers;
  char *header_storage;
  char *reason_storage;
  unsigned char *body_storage;
  chttp_body_sink body_sink;
  chttp_file_sink_transfer *file_sink_transfer;
  size_t header_storage_capacity;
  size_t header_storage_used;
  size_t header_wire_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_response_body_bytes;
  size_t max_reason_bytes;
  size_t max_informational_responses;
  size_t informational_responses;
  size_t field_offset;
  size_t value_offset;
  size_t reason_size;
  int failure_status;
  int parser_status;
  const char *failure_stage;
  chttp_method request_method;
  bool field_open;
  bool value_open;
  bool reason_terminated;
  bool complete;
  bool body_sink_enabled;
} chttp_response_parser;

int chttp_request_build(const chttp_request_options *options, const chttp_limits *limits,
                        unsigned char **out_data, size_t *out_size);

int chttp_response_parser_init(chttp_response_parser *parser, chttp_method method,
                               const chttp_limits *limits);
int chttp_response_parser_init_with_sink(chttp_response_parser *parser, chttp_method method,
                                         const chttp_limits *limits, const chttp_body_sink *sink);
void chttp_response_parser_destroy(chttp_response_parser *parser);
int chttp_response_parser_execute(chttp_response_parser *parser, const void *data, size_t size);
int chttp_response_parser_finish(chttp_response_parser *parser);

#endif /* CHTTP_INTERNAL_H */
