#include "s3_internal.h"

#include <s3/s3_bucket_config.h>

#include <string.h>

typedef struct s3_bucket_subresource {
  const char *query_name;
  const char *root_name;
  const char *stage;
} s3_bucket_subresource;

static const s3_bucket_subresource s3_lifecycle = {"lifecycle", "LifecycleConfiguration",
                                                   "s3-bucket-lifecycle"};
static const s3_bucket_subresource s3_notification = {"notification", "NotificationConfiguration",
                                                      "s3-bucket-notification"};
static const s3_bucket_subresource s3_replication = {"replication", "ReplicationConfiguration",
                                                     "s3-bucket-replication"};

static int s3_bucket_config_error(s3_error *error, int status, const char *stage) {
  if (error != NULL) *error = (s3_error){.status = status, .stage = stage};
  return status;
}

static int s3_bucket_config_request(s3_client *client, const char *bucket,
                                    const s3_bucket_subresource *subresource, s3_method method,
                                    const void *xml, size_t xml_size, s3_response *out_response,
                                    s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_query_param query;
  chttp_header header = {"Content-Type", "application/xml"};
  s3_request_options request;
  salts_xml_document document = {0};
  salts_xml_node root = {0};
  int status = SALTS_OK;
  if (out_error == NULL) return SALTS_EINVAL;
  *out_error = (s3_error){0};
  if (impl == NULL || subresource == NULL ||
      (method != S3_METHOD_GET && method != S3_METHOD_PUT && method != S3_METHOD_DELETE) ||
      (method == S3_METHOD_PUT &&
       (xml == NULL || xml_size == 0u || memchr(xml, '\0', xml_size) != NULL)) ||
      (method != S3_METHOD_PUT && (xml != NULL || xml_size != 0u)))
    return s3_bucket_config_error(out_error, SALTS_EINVAL,
                                  subresource != NULL ? subresource->stage : "s3-bucket-config");
  if (method == S3_METHOD_PUT) {
    status = s3_xml_parse_root(xml, xml_size, impl->base.config.max_xml_bytes,
                               impl->base.config.max_xml_nodes, subresource->root_name, &document,
                               &root);
    salts_xml_document_destroy(&document);
    if (status != SALTS_OK) return s3_bucket_config_error(out_error, status, subresource->stage);
  }
  query = (s3_query_param){subresource->query_name, ""};
  request = (s3_request_options){.size = sizeof(request),
                                 .method = method,
                                 .bucket = bucket,
                                 .query = &query,
                                 .query_count = 1u,
                                 .headers = method == S3_METHOD_PUT ? &header : NULL,
                                 .header_count = method == S3_METHOD_PUT ? 1u : 0u,
                                 .body = xml,
                                 .body_size = xml_size};
  status = s3_request(client, &request, out_response, out_error);
  if (status != SALTS_OK || method != S3_METHOD_GET) return status;
  status = s3_xml_parse_root(out_response->http.body, out_response->http.body_size,
                             impl->base.config.max_xml_bytes, impl->base.config.max_xml_nodes,
                             subresource->root_name, &document, &root);
  salts_xml_document_destroy(&document);
  if (status != SALTS_OK) s3_bucket_config_error(out_error, status, subresource->stage);
  return status;
}

int s3_get_bucket_lifecycle(s3_client *client, const char *bucket, s3_response *out_response,
                            s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_lifecycle, S3_METHOD_GET, NULL, 0u,
                                  out_response, out_error);
}

int s3_put_bucket_lifecycle(s3_client *client, const char *bucket, const void *xml, size_t xml_size,
                            s3_response *out_response, s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_lifecycle, S3_METHOD_PUT, xml, xml_size,
                                  out_response, out_error);
}

int s3_delete_bucket_lifecycle(s3_client *client, const char *bucket, s3_response *out_response,
                               s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_lifecycle, S3_METHOD_DELETE, NULL, 0u,
                                  out_response, out_error);
}

int s3_get_bucket_notification(s3_client *client, const char *bucket, s3_response *out_response,
                               s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_notification, S3_METHOD_GET, NULL, 0u,
                                  out_response, out_error);
}

int s3_put_bucket_notification(s3_client *client, const char *bucket, const void *xml,
                               size_t xml_size, s3_response *out_response, s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_notification, S3_METHOD_PUT, xml, xml_size,
                                  out_response, out_error);
}

int s3_get_bucket_replication(s3_client *client, const char *bucket, s3_response *out_response,
                              s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_replication, S3_METHOD_GET, NULL, 0u,
                                  out_response, out_error);
}

int s3_put_bucket_replication(s3_client *client, const char *bucket, const void *xml,
                              size_t xml_size, s3_response *out_response, s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_replication, S3_METHOD_PUT, xml, xml_size,
                                  out_response, out_error);
}

int s3_delete_bucket_replication(s3_client *client, const char *bucket, s3_response *out_response,
                                 s3_error *out_error) {
  return s3_bucket_config_request(client, bucket, &s3_replication, S3_METHOD_DELETE, NULL, 0u,
                                  out_response, out_error);
}
