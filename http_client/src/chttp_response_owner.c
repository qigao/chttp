#include "chttp_response_owner.h"
#include <http_client/response_scope.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *chttp_response_copy_text(const char *text) {
  const size_t size = strlen(text);
  char *copy;
  if (size == SIZE_MAX) return NULL;
  copy = (char *)malloc(size + 1u);
  if (copy != NULL) memcpy(copy, text, size + 1u);
  return copy;
}

void chttp_response_destroy(chttp_response *response) {
  size_t index;
  if (response == NULL) return;
  free(response->reason);
  for (index = 0u; index < response->header_count; ++index) {
    free((void *)response->headers[index].name);
    free((void *)response->headers[index].value);
  }
  free(response->headers);
  free(response->body);
  *response = (chttp_response){0};
}

static int chttp_response_copy_body(
    const chttp_response_view *source, chttp_response *temporary,
    chttp_response *out) {
  size_t index;
  temporary->http_major = source->http_major;
  temporary->http_minor = source->http_minor;
  temporary->status_code = source->status_code;
  temporary->protocol_keep_alive = source->protocol_keep_alive;
  /* A streaming sink reports transferred bytes without a buffered body. */
  temporary->body_size = source->body_size;
  if (source->reason != NULL) {
    temporary->reason = chttp_response_copy_text(source->reason);
    if (temporary->reason == NULL) return SALTS_ENOMEM;
  }
  if (source->header_count != 0u) {
    temporary->headers = (chttp_header *)calloc(
        source->header_count, sizeof(*temporary->headers));
    if (temporary->headers == NULL) return SALTS_ENOMEM;
    for (index = 0u; index < source->header_count; ++index) {
      /* A published header slot owns even a partially copied pair. */
      chttp_header *header = &temporary->headers[index];
      ++temporary->header_count;
      header->name = chttp_response_copy_text(source->headers[index].name);
      if (header->name == NULL) return SALTS_ENOMEM;
      header->value = chttp_response_copy_text(source->headers[index].value);
      if (header->value == NULL) return SALTS_ENOMEM;
    }
  }
  if (source->body_size != 0u && source->body != NULL) {
    temporary->body = malloc(source->body_size);
    if (temporary->body == NULL) return SALTS_ENOMEM;
    memcpy(temporary->body, source->body, source->body_size);
  }
  chttp_response_move(out, temporary);
  return SALTS_OK;
}

int chttp_response_copy(const chttp_response_view *source, chttp_response *out) {
  size_t index;
  int status;
  if (source == NULL || out == NULL) return SALTS_EINVAL;
  if ((source->header_count != 0u && source->headers == NULL) ||
      source->header_count > SIZE_MAX / sizeof(*out->headers))
    return SALTS_EPROTO;
  for (index = 0u; index < source->header_count; ++index)
    if (source->headers[index].name == NULL || source->headers[index].value == NULL)
      return SALTS_EPROTO;

  /* Native status is preserved; scope owns rollback until the final move. */
  cmeta_scope(status, cmeta_autos((chttp_response, temporary)),
      cmeta_body(chttp_response_copy_body(source, &temporary, out)));
  return status;
}
