#include "chttp_server_runtime.h"

#include <salts_fs.h>
#include <stdint.h>
#include <stdlib.h>

static int chttp_file_async_source_unexpected(void *user, void *buffer, size_t capacity,
                                              size_t *out_size) {
  (void)user;
  (void)buffer;
  (void)capacity;
  if (out_size != NULL) *out_size = 0u;
  return SALTS_EPROTO;
}

static void chttp_server_file_cleanup(void *user, int status) {
  chttp_file_transfer *transfer = (chttp_file_transfer *)user;
  (void)status;
  if (transfer == NULL) return;
  chttp_file_transfer_set_ready(transfer, NULL, NULL);
  transfer->owner_release_requested = true;
  (void)chttp_file_transfer_close(transfer);
}

int chttp_server_send_file(chttp_server_response *response, unsigned int status_code,
    const char *content_type, const char *path, uint64_t offset, size_t length, bool bounded) {
  chttp_server_response_builder *builder;
  cflow_io_file_runtime *runtime = NULL;
  chttp_file_transfer *transfer;
  chttp_body_source source;
  int cleanup_status;
  int status;
  if (response == NULL || response->impl == NULL || path == NULL || path[0] == '\0')
    return SALTS_EINVAL;
  builder = (chttp_server_response_builder *)response->impl;
  if (builder->server == NULL) return SALTS_EINVAL;
  status = chttp_server_file_runtime_ensure(builder->server, &runtime);
  if (status != SALTS_OK) return status;
  transfer = (chttp_file_transfer *)calloc(1u, sizeof(*transfer));
  if (transfer == NULL) return SALTS_ENOMEM;
  status = bounded
      ? chttp_file_transfer_open_read_range(transfer, runtime, path, offset, length,
                                            builder->server->config.stream_chunk_bytes)
      : chttp_file_transfer_open_read(transfer, runtime, path, length,
                                       builder->server->config.stream_chunk_bytes, NULL, NULL);
  if (status != SALTS_OK) {
    free(transfer);
    return status;
  }
  status = chttp_server_file_transfer_register(builder->server, transfer);
  if (status != SALTS_OK) {
    cleanup_status = chttp_file_transfer_drain_destroy(transfer, runtime);
    free(transfer);
    return cleanup_status == SALTS_OK ? status : cleanup_status;
  }
  source = (chttp_body_source){.read = chttp_file_async_source_unexpected,
                               .user = transfer,
                               .content_length = length,
                               .content_length_known = 1};
  status = chttp_server_response_source_owned(response, status_code, content_type, &source,
                                              chttp_server_file_cleanup, transfer);
  if (status != SALTS_OK) {
    chttp_server_file_cleanup(transfer, status);
    return status;
  }
  builder->file_transfer = transfer;
  return status;
}

int chttp_server_response_file(chttp_server_response *response, unsigned int status_code,
                               const char *content_type, const char *path) {
  salts_fs_stat_t file_stat = {0};
  if (response == NULL || response->impl == NULL || path == NULL || path[0] == '\0')
    return SALTS_EINVAL;
  const int status = salts_fs_stat(path, &file_stat);
  if (status != SALTS_OK) return SALTS_EIO;
  if (!file_stat.is_file) return SALTS_EISDIR;
  if (file_stat.size > SIZE_MAX) return SALTS_EFBIG;
  return chttp_server_send_file(response, status_code, content_type, path, 0,
                                (size_t)file_stat.size, false);
}
