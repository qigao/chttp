#include "chttp_server_runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { CHTTP_CORS_MAX_AGE_BYTES = 11 };
static const char cors_origin[] = "Origin";
static const char cors_method[] = "Access-Control-Request-Method";
static const char cors_headers[] = "Access-Control-Request-Headers";

static bool cors_token_char(unsigned char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= 'a' && ch <= 'z') ||
         (ch != 0 && strchr("!#$%&'*+-.^_`|~", ch) != NULL);
}

/* Lists borrow slices from the bounded request headers or immutable policy. */
static int cors_next_token(const char **cursor, const char **token, size_t *size) {
  const char *scan = *cursor;
  while (*scan == ' ' || *scan == '\t' || *scan == ',') ++scan;
  if (*scan == '\0') return 0;
  *token = scan;
  while (cors_token_char((unsigned char)*scan)) ++scan;
  *size = (size_t)(scan - *token);
  if (*size == 0) return -1;
  while (*scan == ' ' || *scan == '\t') ++scan;
  if (*scan != '\0' && *scan != ',') return -1;
  *cursor = scan;
  return 1;
}

static bool cors_equal(const char *a, const char *b, size_t size, bool sensitive) {
  for (size_t index = 0; index < size; ++index) {
    unsigned char left = (unsigned char)a[index], right = (unsigned char)b[index];
    if (!sensitive) {
      if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
      if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
    }
    if (left != right) return false;
  }
  return true;
}

static bool cors_list_contains(const char *list, const char *token, size_t size,
                               bool sensitive) {
  const char *candidate;
  size_t length;
  if (list == NULL) return false;
  while (cors_next_token(&list, &candidate, &length) > 0)
    if (length == size && cors_equal(candidate, token, size, sensitive)) return true;
  return false;
}

static bool cors_policy_list_valid(const char *list) {
  const char *token;
  size_t size, count = 0;
  int status;
  if (list == NULL) return true;
  while ((status = cors_next_token(&list, &token, &size)) > 0) {
    if (size == 1 && *token == '*') return false;
    ++count;
  }
  return status == 0 && count != 0;
}

static size_t cors_trim(const char **value) {
  while (**value == ' ' || **value == '\t') ++*value;
  size_t size = strlen(*value);
  while (size != 0 && ((*value)[size - 1] == ' ' || (*value)[size - 1] == '\t')) --size;
  return size;
}

static bool cors_origin_valid(const char *origin, size_t size) {
  if (size == 0) return false;
  for (size_t index = 0; index < size; ++index) {
    const unsigned char ch = (unsigned char)origin[index];
    if (ch <= ' ' || ch >= 127 || ch == ',') return false;
  }
  return true;
}

static const char *cors_header_at(const chttp_server_request_view *request, size_t index,
                                  const char *name) {
  const chttp_server_request_view field = {.headers = &request->headers[index], .header_count = 1};
  return chttp_server_request_header(&field, name);
}

static int cors_single_header(const chttp_server_request_view *request, const char *name,
                               const char **value) {
  *value = NULL;
  for (size_t index = 0; index < request->header_count; ++index) {
    const char *candidate = cors_header_at(request, index, name);
    if (candidate == NULL) continue;
    if (*value != NULL) return SALTS_EINVAL;
    *value = candidate;
  }
  return SALTS_OK;
}

static unsigned int cors_preflight_status(const chttp_server_cors_options *policy,
                                           const chttp_server_request_view *request,
                                           const char *method) {
  const size_t method_size = cors_trim(&method);
  const char *end = method;
  while (cors_token_char((unsigned char)*end)) ++end;
  if (end == method || (size_t)(end - method) != method_size) return 400u;
  if (!cors_list_contains(policy->methods, method, (size_t)(end - method), true)) return 403u;
  for (size_t index = 0; index < request->header_count; ++index) {
    const char *list = cors_header_at(request, index, cors_headers);
    const char *token;
    size_t size;
    int status;
    if (list == NULL) continue;
    while ((status = cors_next_token(&list, &token, &size)) > 0)
      if (!cors_list_contains(policy->allowed_headers, token, size, false)) return 403u;
    if (status < 0) return 400u;
  }
  return 204u;
}

static int cors_middleware(void *user, const chttp_server_request_view *request,
                            chttp_server_response *response, chttp_server_next *next) {
  const chttp_server_cors_options *policy = (const chttp_server_cors_options *)user;
  const char *origin, *method = NULL, *allowed = NULL;
  int origin_status = cors_single_header(request, cors_origin, &origin);
  const size_t origin_size = origin != NULL ? cors_trim(&origin) : 0;
  int method_status = SALTS_OK;
  int status = chttp_server_response_append_vary(response, cors_origin);
  if (status != SALTS_OK) return status;
  if (origin_status != SALTS_OK || (origin != NULL && !cors_origin_valid(origin, origin_size)))
    return chttp_server_reply(response, 400u, NULL, NULL, 0u);
  if (origin == NULL) return chttp_server_next_call(next);
  if (request->method == CHTTP_METHOD_OPTIONS)
    method_status = cors_single_header(request, cors_method, &method);
  const bool preflight = method != NULL || method_status != SALTS_OK;
  if (preflight) {
    status = chttp_server_response_append_vary(response,
        "Access-Control-Request-Method, Access-Control-Request-Headers");
    if (status != SALTS_OK) return status;
  }
  if (method_status != SALTS_OK) return chttp_server_reply(response, 400u, NULL, NULL, 0u);
  for (size_t index = 0; index < policy->origin_count; ++index)
    if (strcmp(policy->origins[index], "*") == 0 ||
        (strlen(policy->origins[index]) == origin_size &&
         memcmp(policy->origins[index], origin, origin_size) == 0)) {
      allowed = policy->origins[index];
      break;
    }
  if (allowed == NULL) return chttp_server_reply(response, 403u, NULL, NULL, 0u);
  if (preflight) {
    const unsigned int result = cors_preflight_status(policy, request, method);
    if (result != 204u) return chttp_server_reply(response, result, NULL, NULL, 0u);
  }
  status = chttp_server_response_set_header(response, "Access-Control-Allow-Origin", allowed);
  if (status == SALTS_OK && policy->allow_credentials)
    status = chttp_server_response_set_header(response, "Access-Control-Allow-Credentials", "true");
  if (status != SALTS_OK) return status;
  if (!preflight) {
    if (policy->exposed_headers != NULL)
      status = chttp_server_response_set_header(response, "Access-Control-Expose-Headers",
                                                policy->exposed_headers);
    return status == SALTS_OK ? chttp_server_next_call(next) : status;
  }
  status = chttp_server_response_set_header(response, "Access-Control-Allow-Methods", policy->methods);
  if (status == SALTS_OK && policy->allowed_headers != NULL)
    status = chttp_server_response_set_header(response, "Access-Control-Allow-Headers",
                                              policy->allowed_headers);
  if (status == SALTS_OK) {
    char age[CHTTP_CORS_MAX_AGE_BYTES];
    const int length = snprintf(age, sizeof(age), "%" PRIu32, policy->max_age_seconds);
    if (length < 0 || (size_t)length >= sizeof(age)) return SALTS_ERANGE;
    status = chttp_server_response_set_header(response, "Access-Control-Max-Age", age);
  }
  return status == SALTS_OK ? chttp_server_reply(response, 204u, NULL, NULL, 0u) : status;
}

int chttp_server_use_cors(chttp_server *server, const chttp_server_cors_options *options) {
  if (options == NULL || options->origins == NULL || options->origin_count == 0 ||
      options->methods == NULL || !cors_policy_list_valid(options->methods) ||
      !cors_policy_list_valid(options->allowed_headers) ||
      !cors_policy_list_valid(options->exposed_headers)) return SALTS_EINVAL;
  for (size_t index = 0; index < options->origin_count; ++index) {
    const char *origin = options->origins[index];
    if (origin == NULL || !cors_origin_valid(origin, strlen(origin)) ||
        (strchr(origin, '*') != NULL &&
         (strcmp(origin, "*") != 0 || options->origin_count != 1 || options->allow_credentials)))
      return SALTS_EINVAL;
  }
  return chttp_server_use(server, cors_middleware, (void *)options);
}
