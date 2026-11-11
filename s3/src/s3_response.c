#include "s3_internal.h"

#include <xml_parser/xml_parser.h>

#include <stdlib.h>
#include <string.h>

void s3_response_destroy(s3_response *response) {
  if (response == NULL) return;
  chttp_response_destroy(&response->http);
  s3_service_error_destroy(&response->service_error);
  *response = (s3_response){0};
}

void s3_service_error_destroy(s3_service_error *error) {
  if (error == NULL) return;
  free(error->code);
  free(error->message);
  free(error->request_id);
  free(error->host_id);
  *error = (s3_service_error){0};
}

static int s3_xml_name_equal(xml_node node, const char *expected) {
  const xml_string_view name = xml_node_local_name(node);
  const size_t size = strlen(expected);
  return name.data != NULL && name.size == size && memcmp(name.data, expected, size) == 0;
}

static int s3_service_error_assign(s3_service_error *error, xml_node node) {
  char **destination = NULL;
  if (s3_xml_name_equal(node, "Code")) destination = &error->code;
  if (s3_xml_name_equal(node, "Message")) destination = &error->message;
  if (s3_xml_name_equal(node, "RequestId")) destination = &error->request_id;
  if (s3_xml_name_equal(node, "HostId")) destination = &error->host_id;
  if (destination == NULL || *destination != NULL) return SALTS_OK;
  return s3_xml_text_dup(node, destination);
}

static int s3_xml_status_map(xml_status status) {
  if (status == SALTS_XML_OK) return SALTS_OK;
  if (status == SALTS_XML_ALLOCATION_FAILED) return SALTS_ENOMEM;
  if (status == SALTS_XML_LIMIT_EXCEEDED) return SALTS_EMSGSIZE;
  if (status == SALTS_XML_INVALID_ARGUMENT) return SALTS_EINVAL;
  return SALTS_EPROTO;
}

int s3_service_error_parse(const void *body, size_t body_size, size_t max_xml_bytes,
                           size_t max_xml_nodes, s3_service_error *out_error) {
  xml_document document = {0};
  xml_diagnostic diagnostic = {0};
  xml_limits limits = xml_default_limits();
  xml_node root;
  size_t index;
  int status = SALTS_OK;
  xml_status xml_status;

  if (out_error == NULL || out_error->code != NULL || out_error->message != NULL ||
      out_error->request_id != NULL || out_error->host_id != NULL ||
      (body == NULL && body_size != 0u) || max_xml_bytes == 0u || max_xml_nodes == 0u)
    return SALTS_EINVAL;
  if (body_size == 0u) return SALTS_OK;
  if (body_size > max_xml_bytes) return SALTS_EMSGSIZE;
  limits.max_input_bytes = max_xml_bytes;
  limits.max_nodes = max_xml_nodes;
  limits.max_attributes = max_xml_nodes;
  limits.max_depth = max_xml_nodes;
  limits.max_retained_string_bytes = max_xml_bytes;
  xml_status = xml_parse(&document, (const char *)body, body_size, &limits, &diagnostic);
  if (xml_status != SALTS_XML_OK) return s3_xml_status_map(xml_status);
  root = xml_document_root(&document);
  if (!s3_xml_name_equal(root, "Error")) {
    xml_document_destroy(&document);
    return SALTS_OK;
  }
  for (index = 0u; index < xml_node_child_count(root) && status == SALTS_OK; ++index) {
    const xml_node child = xml_node_child_at(root, index);
    if (xml_node_type(child) == SALTS_XML_ELEMENT)
      status = s3_service_error_assign(out_error, child);
  }
  xml_document_destroy(&document);
  if (status != SALTS_OK) {
    s3_service_error_destroy(out_error);
  }
  return status;
}

int s3_response_parse_service_error(s3_response *response, size_t max_xml_bytes,
                                    size_t max_xml_nodes) {
  if (response == NULL) return SALTS_EINVAL;
  return s3_service_error_parse(response->http.body, response->http.body_size, max_xml_bytes,
                                max_xml_nodes, &response->service_error);
}
