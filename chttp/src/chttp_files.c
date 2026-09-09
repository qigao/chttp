#include <chttp/chttp.h>

#include "chttp_client_internal.h"
#include "chttp_server_runtime.h"

#include <salts_fs.h>
#include <tstr.h>
#include <salts_uuid.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_FILE_CHUNK_DEFAULT_BYTES = 64u * 1024u };

static int chttp_file_error(chttp_response *response, chttp_error *error, int native_status,
                            const char *stage) {
  if (response != NULL) chttp_response_destroy(response);
  if (error != NULL)
    *error = (chttp_error){.status = SALTS_EIO, .native_status = native_status, .stage = stage};
  return SALTS_EIO;
}

static int chttp_file_async_source_unexpected(void *user, void *buffer, size_t capacity,
                                              size_t *out_size) {
  (void)user;
  (void)buffer;
  (void)capacity;
  if (out_size != NULL) *out_size = 0u;
  return SALTS_EPROTO;
}

static int chttp_file_async_sink_unexpected(void *user, const void *data, size_t size) {
  (void)user;
  (void)data;
  (void)size;
  return SALTS_EPROTO;
}

static int chttp_file_upload(chttp_client *client, const chttp_options *options, const char *path,
                             chttp_progress_fn progress, void *progress_user,
                             chttp_response *out_response, chttp_error *out_error,
                             chttp_method method) {
  salts_fs_stat_t file_stat = {0};
  cflow_io_file_runtime *runtime = NULL;
  chttp_file_transfer transfer = {0};
  chttp_body_source source;
  chttp_options request_options;
  int native_status;
  int cleanup_status;
  int transfer_status;
  int status;
  if (out_response == NULL || out_error == NULL) return SALTS_EINVAL;
  *out_response = (chttp_response){0};
  *out_error = (chttp_error){0};
  if (client == NULL || options == NULL || path == NULL ||
      (method != CHTTP_METHOD_POST && method != CHTTP_METHOD_PUT) || options->body != NULL ||
      options->body_size != 0u || options->body_source != NULL)
    return SALTS_EINVAL;
  native_status = salts_fs_stat(path, &file_stat);
  if (native_status != SALTS_OK)
    return chttp_file_error(out_response, out_error, native_status, "file-stat");
  if (!file_stat.is_file) {
    *out_error = (chttp_error){.status = SALTS_EISDIR, .stage = "file-stat"};
    return SALTS_EISDIR;
  }
  if (file_stat.size > SIZE_MAX) {
    *out_error = (chttp_error){.status = SALTS_EFBIG, .stage = "file-stat"};
    return SALTS_EFBIG;
  }
  status = chttp_client_file_runtime(client, &runtime);
  if (status != SALTS_OK) {
    *out_error = (chttp_error){.status = status, .stage = "file-runtime"};
    return status;
  }
  status = chttp_file_transfer_open_read(&transfer, runtime, path, (size_t)file_stat.size,
                                         CHTTP_FILE_CHUNK_DEFAULT_BYTES, progress, progress_user);
  if (status != SALTS_OK) return chttp_file_error(out_response, out_error, status, "file-open");
  source = (chttp_body_source){.read = chttp_file_async_source_unexpected,
                               .user = &transfer,
                               .content_length = (size_t)file_stat.size,
                               .content_length_known = 1};
  request_options = *options;
  request_options.body_source = &source;
  status = chttp_client_perform_file(client, method, &request_options, &transfer, out_response,
                                     out_error);
  transfer_status = chttp_file_transfer_status(&transfer, &native_status);
  cleanup_status = chttp_file_transfer_drain_destroy(&transfer, runtime);
  if (status != SALTS_OK) {
    if (transfer_status != SALTS_OK)
      *out_error = (chttp_error){
          .status = transfer_status, .native_status = native_status, .stage = "file-read"};
    return status;
  }
  if (cleanup_status != SALTS_OK)
    return chttp_file_error(out_response, out_error, cleanup_status, "file-close");
  return SALTS_OK;
}

int chttp_post_file(chttp_client *client, const chttp_options *options, const char *path,
                    chttp_progress_fn progress, void *progress_user, chttp_response *out_response,
                    chttp_error *out_error) {
  return chttp_file_upload(client, options, path, progress, progress_user, out_response, out_error,
                           CHTTP_METHOD_POST);
}

int chttp_put_file(chttp_client *client, const chttp_options *options, const char *path,
                   chttp_progress_fn progress, void *progress_user, chttp_response *out_response,
                   chttp_error *out_error) {
  return chttp_file_upload(client, options, path, progress, progress_user, out_response, out_error,
                           CHTTP_METHOD_PUT);
}

static void chttp_server_file_cleanup(void *user, int status) {
  chttp_file_transfer *transfer = (chttp_file_transfer *)user;
  (void)status;
  if (transfer == NULL) return;
  chttp_file_transfer_set_ready(transfer, NULL, NULL);
  transfer->owner_release_requested = true;
  (void)chttp_file_transfer_close(transfer);
}

int chttp_server_response_file(chttp_server_response *response, unsigned int status_code,
                               const char *content_type, const char *path) {
  salts_fs_stat_t file_stat = {0};
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
  status = salts_fs_stat(path, &file_stat);
  if (status != SALTS_OK) return SALTS_EIO;
  if (!file_stat.is_file) return SALTS_EISDIR;
  if (file_stat.size > SIZE_MAX) return SALTS_EFBIG;
  status = chttp_server_file_runtime_ensure(builder->server, &runtime);
  if (status != SALTS_OK) return status;
  transfer = (chttp_file_transfer *)calloc(1u, sizeof(*transfer));
  if (transfer == NULL) return SALTS_ENOMEM;
  status = chttp_file_transfer_open_read(transfer, runtime, path, (size_t)file_stat.size,
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
                               .content_length = (size_t)file_stat.size,
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

static int chttp_file_temp_path(const char *output_path, tstr *out_path) {
  static const char prefix[] = ".chttp-";
  static const char suffix[] = ".part";
  salts_uuid_t uuid;
  char uuid_text[SALTS_UUID_STRING_SIZE];
  size_t output_size;
  size_t total_size;
  tstr path;
  int status;
  if (output_path == NULL || out_path == NULL) return SALTS_EINVAL;
  *out_path = NULL;
  status = salts_uuid_v4_generate(&uuid);
  if (status != SALTS_OK) return status;
  status = salts_uuid_format(&uuid, uuid_text, sizeof(uuid_text));
  if (status != SALTS_OK) return status;
  output_size = strlen(output_path);
  if (output_size >
      SIZE_MAX - (sizeof(prefix) - 1u) - SALTS_UUID_STRING_LENGTH - (sizeof(suffix) - 1u))
    return SALTS_EFBIG;
  total_size = output_size + sizeof(prefix) - 1u + SALTS_UUID_STRING_LENGTH + sizeof(suffix) - 1u;
  path = tstr_new_len(NULL, total_size);
  if (path == NULL) return SALTS_ENOMEM;
  memcpy(path, output_path, output_size);
  memcpy(path + output_size, prefix, sizeof(prefix) - 1u);
  memcpy(path + output_size + sizeof(prefix) - 1u, uuid_text, SALTS_UUID_STRING_LENGTH);
  memcpy(path + total_size - (sizeof(suffix) - 1u), suffix, sizeof(suffix) - 1u);
  path[total_size] = '\0';
  *out_path = path;
  return SALTS_OK;
}

int chttp_download_file(chttp_client *client, const chttp_options *options, const char *output_path,
                        chttp_progress_fn progress, void *progress_user,
                        chttp_response *out_response, chttp_error *out_error) {
  cflow_io_file_runtime *runtime = NULL;
  chttp_file_sink_transfer transfer = {0};
  chttp_body_sink sink = {.write = chttp_file_async_sink_unexpected, .user = &transfer};
  chttp_options request_options;
  tstr temporary_path = NULL;
  size_t sink_capacity = 0u;
  int cleanup_status;
  int transfer_status;
  int status;
  int native_status;
  bool async_flush_supported;
  if (out_response == NULL || out_error == NULL) return SALTS_EINVAL;
  *out_response = (chttp_response){0};
  *out_error = (chttp_error){0};
  if (client == NULL || options == NULL || output_path == NULL || output_path[0] == '\0' ||
      options->body != NULL || options->body_size != 0u || options->body_source != NULL ||
      options->body_sink != NULL)
    return SALTS_EINVAL;
  status = chttp_file_temp_path(output_path, &temporary_path);
  if (status != SALTS_OK) {
    *out_error = (chttp_error){.status = status, .stage = "file-temp-path"};
    return status;
  }
  status = chttp_client_file_runtime(client, &runtime);
  if (status == SALTS_OK) status = chttp_client_file_sink_capacity(client, &sink_capacity);
  if (status != SALTS_OK) {
    (void)salts_fs_unlink(temporary_path);
    tstr_free(temporary_path);
    *out_error = (chttp_error){.status = status, .stage = "file-runtime"};
    return status;
  }
  status = chttp_file_sink_transfer_open(&transfer, runtime, temporary_path, sink_capacity,
                                         progress, progress_user);
  if (status != SALTS_OK) {
    (void)salts_fs_unlink(temporary_path);
    tstr_free(temporary_path);
    return chttp_file_error(out_response, out_error, status, "file-open");
  }
  request_options = *options;
  request_options.body_sink = &sink;
  status = chttp_client_perform_file_download(client, &request_options, &transfer, out_response,
                                              out_error);
  transfer_status = chttp_file_sink_transfer_status(&transfer, &native_status);
  if (status != SALTS_OK) {
    cleanup_status = chttp_file_sink_transfer_drain_destroy(&transfer, runtime);
    (void)salts_fs_unlink(temporary_path);
    tstr_free(temporary_path);
    if (transfer_status != SALTS_OK)
      *out_error = (chttp_error){
          .status = transfer_status, .native_status = native_status, .stage = "file-write"};
    else if (cleanup_status != SALTS_OK)
      *out_error = (chttp_error){.status = cleanup_status, .stage = "file-close"};
    return status;
  }
  if (progress != NULL)
    progress(progress_user, chttp_file_sink_transfer_transferred(&transfer),
             out_response->body_size);
  if (out_response->status_code < 200u || out_response->status_code >= 300u) {
    cleanup_status = chttp_file_sink_transfer_drain_destroy(&transfer, runtime);
    (void)salts_fs_unlink(temporary_path);
    tstr_free(temporary_path);
    if (cleanup_status != SALTS_OK)
      return chttp_file_error(out_response, out_error, cleanup_status, "file-close");
    return SALTS_OK;
  }
  async_flush_supported =
      cflow_io_file_operation_supported(&transfer.file, CFLOW_IO_NATIVE_FILE_FLUSH);
  if (async_flush_supported) {
    status = chttp_file_sink_transfer_flush_drain(&transfer, runtime);
    if (status != SALTS_OK) {
      (void)chttp_file_sink_transfer_drain_destroy(&transfer, runtime);
      (void)salts_fs_unlink(temporary_path);
      tstr_free(temporary_path);
      return chttp_file_error(out_response, out_error, status, "file-sync");
    }
  }
  cleanup_status = chttp_file_sink_transfer_drain_destroy(&transfer, runtime);
  if (cleanup_status != SALTS_OK) {
    (void)salts_fs_unlink(temporary_path);
    tstr_free(temporary_path);
    return chttp_file_error(out_response, out_error, cleanup_status, "file-close");
  }
  if (!async_flush_supported) {
    salts_file_t sync_file = salts_fs_open(temporary_path, SALTS_FS_O_WRONLY, 0);
    if (sync_file == SALTS_INVALID_FILE) {
      (void)salts_fs_unlink(temporary_path);
      tstr_free(temporary_path);
      return chttp_file_error(out_response, out_error, 0, "file-sync-open");
    }
    native_status = salts_fs_fsync(sync_file);
    cleanup_status = salts_fs_close(sync_file);
    if (native_status == SALTS_OK) native_status = cleanup_status;
    if (native_status != SALTS_OK) {
      (void)salts_fs_unlink(temporary_path);
      tstr_free(temporary_path);
      return chttp_file_error(out_response, out_error, native_status, "file-sync");
    }
  }
  native_status = salts_fs_rename(temporary_path, output_path);
  if (native_status != SALTS_OK) {
    (void)salts_fs_unlink(temporary_path);
    tstr_free(temporary_path);
    return chttp_file_error(out_response, out_error, native_status, "file-commit");
  }
  tstr_free(temporary_path);
  return SALTS_OK;
}
