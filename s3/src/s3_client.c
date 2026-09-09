#include "s3_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void s3_error_set(s3_error *error, int status, int native_status, const char *stage) {
  if (error == NULL) return;
  *error = (s3_error){.status = status, .native_status = native_status, .stage = stage};
}

static int s3_client_config_normalize(s3_client_config *config) {
  size_t region_index;
  if (config == NULL || config->size != sizeof(*config) || config->connection_uri == NULL ||
      config->connection_uri[0] == '\0' || config->authority == NULL ||
      config->authority[0] == '\0' || config->region == NULL || config->region[0] == '\0' ||
      config->credentials.fetch == NULL ||
      (config->addressing_style != S3_ADDRESSING_PATH &&
       config->addressing_style != S3_ADDRESSING_VIRTUAL_HOSTED) ||
      (config->protocol != CHTTP_HTTP_1_1 && config->protocol != CHTTP_HTTP_2))
    return SALTS_EINVAL;
  if (strlen(config->region) > S3_MAX_REGION_BYTES) return SALTS_EINVAL;
  for (region_index = 0u; config->region[region_index] != '\0'; ++region_index) {
    const unsigned char value = (unsigned char)config->region[region_index];
    if (!((value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
          (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
          (value >= (unsigned char)'0' && value <= (unsigned char)'9') || value == '-'))
      return SALTS_EINVAL;
  }
  if (config->max_bucket_name_bytes == 0u)
    config->max_bucket_name_bytes = S3_DEFAULT_MAX_BUCKET_NAME_BYTES;
  if (config->max_object_key_bytes == 0u)
    config->max_object_key_bytes = S3_DEFAULT_MAX_OBJECT_KEY_BYTES;
  if (config->max_target_bytes == 0u) config->max_target_bytes = S3_DEFAULT_MAX_TARGET_BYTES;
  if (config->max_query_count == 0u) config->max_query_count = S3_DEFAULT_MAX_QUERY_COUNT;
  if (config->max_header_count == 0u) config->max_header_count = S3_DEFAULT_MAX_HEADER_COUNT;
  if (config->max_header_bytes == 0u) config->max_header_bytes = S3_DEFAULT_MAX_HEADER_BYTES;
  if (config->max_xml_bytes == 0u) config->max_xml_bytes = S3_DEFAULT_MAX_XML_BYTES;
  if (config->max_xml_nodes == 0u) config->max_xml_nodes = S3_DEFAULT_MAX_XML_NODES;
  if (config->max_list_entries == 0u) config->max_list_entries = S3_DEFAULT_MAX_LIST_ENTRIES;
  if (config->max_multipart_parts == 0u) config->max_multipart_parts = S3_MULTIPART_MAX_PARTS;
  if (config->max_multipart_part_bytes == 0u)
    config->max_multipart_part_bytes = S3_MULTIPART_DEFAULT_MAX_BUFFER_BYTES;
  if (config->max_bucket_name_bytes > S3_DEFAULT_MAX_BUCKET_NAME_BYTES ||
      config->max_header_count < 3u || config->max_target_bytes == 0u ||
      config->max_header_bytes == 0u || config->max_xml_bytes == 0u ||
      config->max_xml_nodes == 0u || config->max_list_entries == 0u ||
      config->max_multipart_parts == 0u || config->max_multipart_parts > S3_MULTIPART_MAX_PARTS ||
      config->max_multipart_part_bytes < S3_MULTIPART_MIN_PART_BYTES ||
      config->max_multipart_part_bytes > (size_t)INT_MAX ||
      (uint64_t)config->max_multipart_part_bytes > S3_MULTIPART_MAX_PART_BYTES)
    return SALTS_EINVAL;
  return SALTS_OK;
}

int s3_client_base_init(s3_client_base *base, const s3_client_config *config) {
  s3_client_config normalized;
  if (base == NULL || config == NULL) return SALTS_EINVAL;
  *base = (s3_client_base){0};
  normalized = *config;
  if (s3_client_config_normalize(&normalized) != SALTS_OK) return SALTS_EINVAL;
  base->connection_uri = tstr_dup(normalized.connection_uri);
  base->authority = tstr_dup(normalized.authority);
  base->region = tstr_dup(normalized.region);
  if (base->connection_uri == NULL || base->authority == NULL || base->region == NULL) {
    s3_client_base_destroy(base);
    return SALTS_ENOMEM;
  }
  normalized.connection_uri = base->connection_uri;
  normalized.authority = base->authority;
  normalized.region = base->region;
  base->config = normalized;
  return SALTS_OK;
}

void s3_client_base_destroy(s3_client_base *base) {
  if (base == NULL) return;
  tstr_free(base->connection_uri);
  tstr_free(base->authority);
  tstr_free(base->region);
  *base = (s3_client_base){0};
}

int s3_client_init(s3_client *client, chttp_client *http_client, const s3_client_config *config) {
  s3_client_impl *impl;
  int status;
  if (client == NULL || http_client == NULL || http_client->impl == NULL || config == NULL)
    return SALTS_EINVAL;
  if (client->impl != NULL) return SALTS_EALREADY;
  impl = (s3_client_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  status = s3_client_base_init(&impl->base, config);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  impl->http_client = http_client;
  client->impl = impl;
  return SALTS_OK;
}

static int s3_http_call(s3_client_impl *client, const s3_request_options *request,
                        const s3_request_plan *plan, chttp_response *out_response,
                        chttp_error *out_error) {
  const chttp_options options = {.connection_uri = client->base.connection_uri,
                                 .authority = plan->authority,
                                 .target = plan->target,
                                 .headers = plan->headers,
                                 .header_count = plan->header_count,
                                 .body = request->body,
                                 .body_size = request->body_size,
                                 .body_source = request->body_source,
                                 .body_sink = request->body_sink,
                                 .timeout_ms = client->base.config.timeout_ms,
                                 .tls = client->base.config.tls,
                                 .protocol = client->base.config.protocol};
  switch (plan->method) {
  case CHTTP_METHOD_GET:
    return chttp_get(client->http_client, &options, out_response, out_error);
  case CHTTP_METHOD_HEAD:
    return chttp_head(client->http_client, &options, out_response, out_error);
  case CHTTP_METHOD_POST:
    return chttp_post(client->http_client, &options, out_response, out_error);
  case CHTTP_METHOD_PUT:
    return chttp_put(client->http_client, &options, out_response, out_error);
  case CHTTP_METHOD_DELETE:
    return chttp_delete(client->http_client, &options, out_response, out_error);
  default:
    return SALTS_EINVAL;
  }
}

int s3_response_is_empty(const s3_response *response) {
  return response != NULL && response->http.reason == NULL && response->http.headers == NULL &&
         response->http.body == NULL && response->service_error.code == NULL &&
         response->service_error.message == NULL && response->service_error.request_id == NULL &&
         response->service_error.host_id == NULL;
}

static int s3_response_adopt(s3_client_impl *client, chttp_response *http_response,
                             s3_response *out_response, s3_error *out_error) {
  int status = SALTS_OK;
  out_response->http = *http_response;
  *http_response = (chttp_response){0};
  if (out_response->http.status_code < 200u || out_response->http.status_code >= 300u) {
    const int parse_status =
        out_response->http.body == NULL
            ? SALTS_OK
            : s3_response_parse_service_error(out_response, client->base.config.max_xml_bytes,
                                              client->base.config.max_xml_nodes);
    status = parse_status == SALTS_OK ? SALTS_EPROTO : parse_status;
    s3_error_set(out_error, status, 0,
                 parse_status == SALTS_OK ? "s3-service" : "s3-service-parse");
  }
  return status;
}

int s3_request(s3_client *client, const s3_request_options *options, s3_response *out_response,
               s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_request_plan plan = {0};
  chttp_response http_response = {0};
  chttp_error http_error = {0};
  int status;
  if (out_response == NULL || out_error == NULL || !s3_response_is_empty(out_response))
    return SALTS_EINVAL;
  *out_response = (s3_response){0};
  *out_error = (s3_error){0};
  if (impl == NULL || options == NULL) {
    s3_error_set(out_error, SALTS_EINVAL, 0, "s3-request");
    return SALTS_EINVAL;
  }
  if (impl->operation_active) {
    s3_error_set(out_error, SALTS_EBUSY, 0, "s3-request");
    return SALTS_EBUSY;
  }
  impl->operation_active = 1;
  status = s3_request_plan_build(&impl->base, options, &plan);
  if (status != SALTS_OK) {
    s3_error_set(out_error, status, 0, "s3-request-plan");
    goto done;
  }
  status = s3_http_call(impl, options, &plan, &http_response, &http_error);
  if (status != SALTS_OK) {
    s3_error_set(out_error, status, http_error.native_status,
                 http_error.stage != NULL ? http_error.stage : "http");
    goto done;
  }
  status = s3_response_adopt(impl, &http_response, out_response, out_error);

done:
  chttp_response_destroy(&http_response);
  s3_request_plan_destroy(&plan);
  impl->operation_active = 0;
  return status;
}

static int s3_request_file(s3_client *client, const s3_request_options *options, const char *path,
                           int upload, chttp_progress_fn progress, void *progress_user,
                           s3_response *out_response, s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_request_plan plan = {0};
  chttp_response http_response = {0};
  chttp_error http_error = {0};
  chttp_options http_options;
  int status;
  if (out_response == NULL || out_error == NULL || !s3_response_is_empty(out_response))
    return SALTS_EINVAL;
  *out_response = (s3_response){0};
  *out_error = (s3_error){0};
  if (impl == NULL || options == NULL || path == NULL || path[0] == '\0' ||
      (upload && options->method != S3_METHOD_PUT) ||
      (!upload && options->method != S3_METHOD_GET)) {
    s3_error_set(out_error, SALTS_EINVAL, 0, upload ? "s3-put-file" : "s3-get-file");
    return SALTS_EINVAL;
  }
  if (impl->operation_active) {
    s3_error_set(out_error, SALTS_EBUSY, 0, upload ? "s3-put-file" : "s3-get-file");
    return SALTS_EBUSY;
  }
  impl->operation_active = 1;
  status = s3_request_plan_build(&impl->base, options, &plan);
  if (status != SALTS_OK) {
    s3_error_set(out_error, status, 0, "s3-request-plan");
    goto done;
  }
  http_options = (chttp_options){.connection_uri = impl->base.connection_uri,
                                 .authority = plan.authority,
                                 .target = plan.target,
                                 .headers = plan.headers,
                                 .header_count = plan.header_count,
                                 .timeout_ms = impl->base.config.timeout_ms,
                                 .tls = impl->base.config.tls,
                                 .protocol = impl->base.config.protocol};
  status = upload ? chttp_put_file(impl->http_client, &http_options, path, progress, progress_user,
                                   &http_response, &http_error)
                  : chttp_download_file(impl->http_client, &http_options, path, progress,
                                        progress_user, &http_response, &http_error);
  if (status != SALTS_OK) {
    s3_error_set(out_error, status, http_error.native_status,
                 http_error.stage != NULL ? http_error.stage : "http-file");
    goto done;
  }
  status = s3_response_adopt(impl, &http_response, out_response, out_error);

done:
  chttp_response_destroy(&http_response);
  s3_request_plan_destroy(&plan);
  impl->operation_active = 0;
  return status;
}

int s3_request_put_file(s3_client *client, const s3_request_options *options, const char *path,
                        chttp_progress_fn progress, void *progress_user, s3_response *out_response,
                        s3_error *out_error) {
  return s3_request_file(client, options, path, 1, progress, progress_user, out_response,
                         out_error);
}

int s3_request_get_file(s3_client *client, const s3_request_options *options,
                        const char *output_path, chttp_progress_fn progress, void *progress_user,
                        s3_response *out_response, s3_error *out_error) {
  return s3_request_file(client, options, output_path, 0, progress, progress_user, out_response,
                         out_error);
}

int s3_client_destroy(s3_client *client) {
  s3_client_impl *impl;
  if (client == NULL) return SALTS_EINVAL;
  impl = (s3_client_impl *)client->impl;
  if (impl == NULL) return SALTS_OK;
  if (impl->operation_active) return SALTS_EBUSY;
  s3_client_base_destroy(&impl->base);
  free(impl);
  client->impl = NULL;
  return SALTS_OK;
}
