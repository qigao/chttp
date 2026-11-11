#include "chttp_retry.h"
#include <stdlib.h>
#include <string.h>

static int retry_copy_string(tstr *out, const char *source, size_t limit, size_t *bytes) {
  size_t size = 0u;
  if (source == NULL) return SALTS_EINVAL;
  while (size < limit && source[size] != '\0') ++size;
  if (source[size] != '\0') return SALTS_EMSGSIZE;
  *out = tstr_dup_len(source, size);
  if (*out == NULL) return SALTS_ENOMEM;
  if (bytes != NULL) *bytes = size;
  return SALTS_OK;
}

void chttp_retry_request_destroy(chttp_retry_request *r) {
  if (r->headers != NULL) {
    for (size_t i = 0u; i < r->options.header_count; ++i) {
      tstr_free((tstr)r->headers[i].name);
      tstr_free((tstr)r->headers[i].value);
    }
  }
  free(r->headers);
  free(r->body);
  tstr_free(r->uri); tstr_free(r->authority); tstr_free(r->target);
  *r = (chttp_retry_request){0};
}

int chttp_retry_request_copy(chttp_retry_request *r, const chttp_options *o,
    const chttp_client_config *config) {
  size_t header_bytes = 0u;
  int status;
  *r = (chttp_retry_request){.options = *o};
  if (o->header_count > config->max_header_count ||
      o->body_size > config->max_request_body_bytes) return SALTS_EMSGSIZE;
  if ((o->header_count != 0u && o->headers == NULL) ||
      (o->body_size != 0u && o->body == NULL)) return SALTS_EINVAL;
  status = retry_copy_string(&r->uri, o->connection_uri, config->max_header_bytes, NULL);
  if (status == SALTS_OK)
    status = retry_copy_string(&r->authority, o->authority, config->max_header_bytes, NULL);
  if (status == SALTS_OK)
    status = retry_copy_string(&r->target, o->target, config->max_start_line_bytes, NULL);
  if (status != SALTS_OK) return status;
  if (o->header_count > SIZE_MAX / sizeof(*r->headers)) return SALTS_ERANGE;
  if (o->header_count != 0u) {
    r->headers = calloc(o->header_count, sizeof(*r->headers));
    if (r->headers == NULL) return SALTS_ENOMEM;
  }
  for (size_t i = 0u; i < o->header_count; ++i) {
    tstr name = NULL, value = NULL;
    size_t size = 0u;
    status = retry_copy_string(&name, o->headers[i].name,
        config->max_header_bytes - header_bytes, &size);
    r->headers[i].name = name;
    if (status != SALTS_OK) return status;
    header_bytes += size;
    status = retry_copy_string(&value, o->headers[i].value,
        config->max_header_bytes - header_bytes, &size);
    r->headers[i].value = value;
    if (status != SALTS_OK) return status;
    header_bytes += size;
  }
  if (o->body_size != 0u) {
    r->body = malloc(o->body_size);
    if (r->body == NULL) return SALTS_ENOMEM;
    memcpy(r->body, o->body, o->body_size);
  }
  r->options.connection_uri = r->uri;
  r->options.authority = r->authority;
  r->options.target = r->target;
  r->options.headers = r->headers;
  r->options.body = r->body;
  return SALTS_OK;
}
