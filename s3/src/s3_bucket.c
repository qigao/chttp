#include "s3_internal.h"

#include <s3/s3_bucket.h>

#include <stdlib.h>
#include <string.h>

static const char s3_create_bucket_xml_prefix[] =
    "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
    "<LocationConstraint>";
static const char s3_create_bucket_xml_suffix[] =
    "</LocationConstraint></CreateBucketConfiguration>";

static int s3_bucket_request(s3_client *client, s3_method method, const char *bucket,
                             s3_response *out_response, s3_error *out_error) {
  const s3_request_options options = {.size = sizeof(options), .method = method, .bucket = bucket};
  return s3_request(client, &options, out_response, out_error);
}

int s3_create_bucket(s3_client *client, const char *bucket, s3_response *out_response,
                     s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_text_builder body = {0};
  chttp_header header;
  s3_request_options options;
  size_t required = sizeof(s3_create_bucket_xml_prefix) - 1u;
  int status;
  if (impl == NULL || out_response == NULL || out_error == NULL) return SALTS_EINVAL;
  *out_error = (s3_error){0};
  if (strcmp(impl->base.region, "us-east-1") == 0)
    return s3_bucket_request(client, S3_METHOD_PUT, bucket, out_response, out_error);
  status = s3_checked_add(required, strlen(impl->base.region), &required);
  if (status == SALTS_OK)
    status = s3_checked_add(required, sizeof(s3_create_bucket_xml_suffix) - 1u, &required);
  if (status == SALTS_OK) status = s3_text_builder_init(&body, required);
  if (status == SALTS_OK)
    status = s3_text_builder_append(&body, s3_create_bucket_xml_prefix,
                                    sizeof(s3_create_bucket_xml_prefix) - 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&body, impl->base.region);
  if (status == SALTS_OK)
    status = s3_text_builder_append(&body, s3_create_bucket_xml_suffix,
                                    sizeof(s3_create_bucket_xml_suffix) - 1u);
  header = (chttp_header){"Content-Type", "application/xml"};
  options = (s3_request_options){.size = sizeof(options),
                                 .method = S3_METHOD_PUT,
                                 .bucket = bucket,
                                 .headers = &header,
                                 .header_count = 1u,
                                 .body = body.text,
                                 .body_size = body.text != NULL ? tstr_len(body.text) : 0u};
  if (status == SALTS_OK) status = s3_request(client, &options, out_response, out_error);
  if (status != SALTS_OK && out_error->status == SALTS_OK)
    *out_error = (s3_error){.status = status, .stage = "s3-create-bucket"};
  s3_text_builder_destroy(&body);
  return status;
}

int s3_head_bucket(s3_client *client, const char *bucket, s3_response *out_response,
                   s3_error *out_error) {
  return s3_bucket_request(client, S3_METHOD_HEAD, bucket, out_response, out_error);
}

int s3_delete_bucket(s3_client *client, const char *bucket, s3_response *out_response,
                     s3_error *out_error) {
  return s3_bucket_request(client, S3_METHOD_DELETE, bucket, out_response, out_error);
}

void s3_bucket_list_destroy(s3_bucket_list *list) {
  size_t index;
  if (list == NULL) return;
  for (index = 0u; index < list->count; ++index) {
    free(list->items[index].name);
    free(list->items[index].creation_date);
  }
  free(list->items);
  *list = (s3_bucket_list){0};
}

static int s3_bucket_info_parse(salts_xml_node node, s3_bucket_info *out) {
  const salts_xml_node name = s3_xml_child(node, "Name");
  const salts_xml_node creation_date = s3_xml_child(node, "CreationDate");
  if (name.impl == NULL || creation_date.impl == NULL || out == NULL) return SALTS_EPROTO;
  {
    int status = s3_xml_text_dup(name, &out->name);
    if (status == SALTS_OK) status = s3_xml_text_dup(creation_date, &out->creation_date);
    return status;
  }
}

static int s3_bucket_list_parse(const s3_client_impl *client, const s3_response *response,
                                s3_bucket_list *out) {
  salts_xml_document document = {0};
  salts_xml_node root = {0};
  salts_xml_node buckets;
  size_t count = 0u;
  size_t index;
  size_t output_index = 0u;
  int status = s3_xml_parse_root(
      response->http.body, response->http.body_size, client->base.config.max_xml_bytes,
      client->base.config.max_xml_nodes, "ListAllMyBucketsResult", &document, &root);
  if (status != SALTS_OK) return status;
  buckets = s3_xml_child(root, "Buckets");
  if (buckets.impl == NULL) {
    status = SALTS_EPROTO;
    goto done;
  }
  for (index = 0u; index < salts_xml_node_child_count(buckets); ++index) {
    const salts_xml_node child = salts_xml_node_child_at(buckets, index);
    if (salts_xml_node_type(child) == SALTS_XML_ELEMENT && s3_xml_node_name_equal(child, "Bucket"))
      ++count;
  }
  if (count > client->base.config.max_list_entries ||
      (count != 0u && count > SIZE_MAX / sizeof(*out->items))) {
    status = SALTS_ENOBUFS;
    goto done;
  }
  if (count != 0u) {
    out->items = (s3_bucket_info *)calloc(count, sizeof(*out->items));
    if (out->items == NULL) {
      status = SALTS_ENOMEM;
      goto done;
    }
  }
  out->count = count;
  for (index = 0u; index < salts_xml_node_child_count(buckets) && status == SALTS_OK; ++index) {
    const salts_xml_node child = salts_xml_node_child_at(buckets, index);
    if (salts_xml_node_type(child) == SALTS_XML_ELEMENT && s3_xml_node_name_equal(child, "Bucket"))
      status = s3_bucket_info_parse(child, &out->items[output_index++]);
  }

done:
  salts_xml_document_destroy(&document);
  if (status != SALTS_OK) s3_bucket_list_destroy(out);
  return status;
}

int s3_list_buckets(s3_client *client, s3_bucket_list *out_list, s3_response *out_response,
                    s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  int status;
  if (impl == NULL || out_list == NULL || out_list->items != NULL || out_response == NULL ||
      out_error == NULL)
    return SALTS_EINVAL;
  *out_list = (s3_bucket_list){0};
  status = s3_bucket_request(client, S3_METHOD_GET, NULL, out_response, out_error);
  if (status != SALTS_OK) return status;
  status = s3_bucket_list_parse(impl, out_response, out_list);
  if (status != SALTS_OK)
    *out_error = (s3_error){.status = status, .stage = "s3-list-buckets-parse"};
  return status;
}
