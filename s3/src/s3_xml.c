#include "s3_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int s3_xml_status_map(salts_xml_status status) {
  if (status == SALTS_XML_OK) return SALTS_OK;
  if (status == SALTS_XML_ALLOCATION_FAILED) return SALTS_ENOMEM;
  if (status == SALTS_XML_LIMIT_EXCEEDED) return SALTS_EMSGSIZE;
  if (status == SALTS_XML_INVALID_ARGUMENT) return SALTS_EINVAL;
  return SALTS_EPROTO;
}

int s3_xml_node_name_equal(salts_xml_node node, const char *expected) {
  const salts_xml_string_view name = salts_xml_node_local_name(node);
  const size_t size = expected != NULL ? strlen(expected) : 0u;
  return expected != NULL && name.data != NULL && name.size == size &&
         memcmp(name.data, expected, size) == 0;
}

salts_xml_node s3_xml_child(salts_xml_node parent, const char *name) {
  size_t index;
  for (index = 0u; index < salts_xml_node_child_count(parent); ++index) {
    const salts_xml_node child = salts_xml_node_child_at(parent, index);
    if (salts_xml_node_type(child) == SALTS_XML_ELEMENT && s3_xml_node_name_equal(child, name))
      return child;
  }
  return (salts_xml_node){0};
}

static int s3_xml_codepoint_valid(uint32_t value) {
  return value == 0x09u || value == 0x0au || value == 0x0du ||
         (value >= 0x20u && value <= 0xd7ffu) || (value >= 0xe000u && value <= 0xfffdu) ||
         (value >= 0x10000u && value <= 0x10ffffu);
}

static size_t s3_xml_utf8_write(uint32_t value, char *output) {
  if (value <= 0x7fu) {
    output[0] = (char)value;
    return 1u;
  }
  if (value <= 0x7ffu) {
    output[0] = (char)(0xc0u | (value >> 6u));
    output[1] = (char)(0x80u | (value & 0x3fu));
    return 2u;
  }
  if (value <= 0xffffu) {
    output[0] = (char)(0xe0u | (value >> 12u));
    output[1] = (char)(0x80u | ((value >> 6u) & 0x3fu));
    output[2] = (char)(0x80u | (value & 0x3fu));
    return 3u;
  }
  output[0] = (char)(0xf0u | (value >> 18u));
  output[1] = (char)(0x80u | ((value >> 12u) & 0x3fu));
  output[2] = (char)(0x80u | ((value >> 6u) & 0x3fu));
  output[3] = (char)(0x80u | (value & 0x3fu));
  return 4u;
}

static int s3_xml_numeric_entity(const char *entity, size_t size, uint32_t *out_value) {
  uint32_t value = 0u;
  uint32_t base = 10u;
  size_t index = 1u;
  if (size < 2u || entity[0] != '#' || out_value == NULL) return SALTS_EPROTO;
  if (entity[index] == 'x' || entity[index] == 'X') {
    base = 16u;
    ++index;
  }
  if (index == size) return SALTS_EPROTO;
  for (; index < size; ++index) {
    uint32_t digit;
    if (entity[index] >= '0' && entity[index] <= '9') digit = (uint32_t)(entity[index] - '0');
    else if (base == 16u && entity[index] >= 'a' && entity[index] <= 'f')
      digit = (uint32_t)(entity[index] - 'a') + 10u;
    else if (base == 16u && entity[index] >= 'A' && entity[index] <= 'F')
      digit = (uint32_t)(entity[index] - 'A') + 10u;
    else return SALTS_EPROTO;
    if (value > (UINT32_MAX - digit) / base) return SALTS_EPROTO;
    value = value * base + digit;
  }
  if (!s3_xml_codepoint_valid(value)) return SALTS_EPROTO;
  *out_value = value;
  return SALTS_OK;
}

static int s3_xml_entity_value(const char *entity, size_t size, uint32_t *out_value) {
  if (size == 2u && memcmp(entity, "lt", 2u) == 0) *out_value = '<';
  else if (size == 2u && memcmp(entity, "gt", 2u) == 0) *out_value = '>';
  else if (size == 3u && memcmp(entity, "amp", 3u) == 0) *out_value = '&';
  else if (size == 4u && memcmp(entity, "quot", 4u) == 0) *out_value = '"';
  else if (size == 4u && memcmp(entity, "apos", 4u) == 0) *out_value = '\'';
  else if (size != 0u && entity[0] == '#') return s3_xml_numeric_entity(entity, size, out_value);
  else return SALTS_EPROTO;
  return SALTS_OK;
}

int s3_xml_text_dup(salts_xml_node node, char **out_text) {
  char *raw;
  char *decoded;
  size_t input = 0u;
  size_t output = 0u;
  size_t size;
  if (node.impl == NULL || out_text == NULL || *out_text != NULL) return SALTS_EINVAL;
  raw = salts_xml_node_text_dup(node);
  if (raw == NULL) return SALTS_ENOMEM;
  size = strlen(raw);
  decoded = (char *)malloc(size + 1u);
  if (decoded == NULL) {
    free(raw);
    return SALTS_ENOMEM;
  }
  while (input < size) {
    if (raw[input] != '&') {
      decoded[output++] = raw[input++];
    } else {
      const char *end = (const char *)memchr(raw + input + 1u, ';', size - input - 1u);
      uint32_t value;
      int status;
      if (end == NULL) {
        free(decoded);
        free(raw);
        return SALTS_EPROTO;
      }
      status = s3_xml_entity_value(raw + input + 1u, (size_t)(end - raw - input - 1u), &value);
      if (status != SALTS_OK) {
        free(decoded);
        free(raw);
        return status;
      }
      output += s3_xml_utf8_write(value, decoded + output);
      input = (size_t)(end - raw) + 1u;
    }
  }
  decoded[output] = '\0';
  free(raw);
  *out_text = decoded;
  return SALTS_OK;
}

int s3_xml_parse_root(const void *body, size_t body_size, size_t max_xml_bytes,
                      size_t max_xml_nodes, const char *expected_root,
                      salts_xml_document *out_document, salts_xml_node *out_root) {
  salts_xml_limits limits = salts_xml_default_limits();
  salts_xml_diagnostic diagnostic = {0};
  salts_xml_status xml_status;
  if ((body == NULL && body_size != 0u) || body_size == 0u || max_xml_bytes == 0u ||
      max_xml_nodes == 0u || expected_root == NULL || out_document == NULL ||
      out_document->impl != NULL || out_root == NULL)
    return SALTS_EINVAL;
  *out_root = (salts_xml_node){0};
  if (body_size > max_xml_bytes) return SALTS_EMSGSIZE;
  limits.max_input_bytes = max_xml_bytes;
  limits.max_nodes = max_xml_nodes;
  limits.max_attributes = max_xml_nodes;
  limits.max_depth = max_xml_nodes;
  limits.max_retained_string_bytes = max_xml_bytes;
  xml_status = salts_xml_parse(out_document, (const char *)body, body_size, &limits, &diagnostic);
  if (xml_status != SALTS_XML_OK) return s3_xml_status_map(xml_status);
  *out_root = salts_xml_document_root(out_document);
  if (!s3_xml_node_name_equal(*out_root, expected_root)) {
    salts_xml_document_destroy(out_document);
    *out_root = (salts_xml_node){0};
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}
