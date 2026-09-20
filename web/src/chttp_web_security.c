#include <chttp_web/web.h>

#include <salts/error_codes.h>

#include <string.h>

static const char CHTTP_WEB_REFERENCE_CSP[] =
    "default-src 'self'; "
    "base-uri 'self'; "
    "object-src 'none'; "
    "frame-ancestors 'none'; "
    "form-action 'self'; "
    "script-src 'self'; "
    "style-src 'self'; "
    "img-src 'self' data:; "
    "connect-src 'self'";

static int chttp_web_security_value_valid(const char *value) {
  const unsigned char *cursor;
  if (value == NULL) return 1;
  if (value[0] == '\0') return 0;
  cursor = (const unsigned char *)value;
  while (*cursor != 0u) {
    if (*cursor == (unsigned char)'\r' ||
        *cursor == (unsigned char)'\n')
      return 0;
    ++cursor;
  }
  return 1;
}

static int chttp_web_security_policy_valid(
    const chttp_web_security_policy *policy) {
  return policy != NULL &&
         policy->size >= sizeof(*policy) &&
         chttp_web_security_value_valid(
             policy->content_security_policy) &&
         chttp_web_security_value_valid(policy->referrer_policy) &&
         chttp_web_security_value_valid(policy->frame_options) &&
         chttp_web_security_value_valid(
             policy->strict_transport_security) &&
         chttp_web_security_value_valid(policy->cache_control);
}

chttp_web_security_policy chttp_web_security_reference_policy(void) {
  chttp_web_security_policy policy =
      (chttp_web_security_policy)CHTTP_WEB_SECURITY_POLICY_INIT;
  policy.content_security_policy = CHTTP_WEB_REFERENCE_CSP;
  policy.referrer_policy = "no-referrer";
  policy.frame_options = "DENY";
  policy.nosniff = true;
  return policy;
}

chttp_web_security_policy chttp_web_security_authenticated_policy(void) {
  chttp_web_security_policy policy =
      chttp_web_security_reference_policy();
  policy.cache_control = "private, no-store";
  return policy;
}

static int chttp_web_security_set(
    chttp_server_response *response,
    const char *name,
    const char *value) {
  if (value == NULL) return SALTS_OK;
  return chttp_server_response_set_header(response, name, value);
}

static int chttp_web_security_middleware(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response,
    chttp_server_next *next) {
  const chttp_web_security_policy *policy =
      (const chttp_web_security_policy *)user;
  int status;
  (void)request;

  if (!chttp_web_security_policy_valid(policy) ||
      response == NULL || next == NULL)
    return SALTS_EINVAL;

  status = chttp_web_security_set(
      response, "Content-Security-Policy",
      policy->content_security_policy);
  if (status == SALTS_OK && policy->nosniff)
    status = chttp_server_response_set_header(
        response, "X-Content-Type-Options", "nosniff");
  if (status == SALTS_OK)
    status = chttp_web_security_set(
        response, "Referrer-Policy", policy->referrer_policy);
  if (status == SALTS_OK)
    status = chttp_web_security_set(
        response, "X-Frame-Options", policy->frame_options);
  if (status == SALTS_OK)
    status = chttp_web_security_set(
        response, "Strict-Transport-Security",
        policy->strict_transport_security);
  if (status == SALTS_OK)
    status = chttp_web_security_set(
        response, "Cache-Control", policy->cache_control);
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

int chttp_web_security_use(
    chttp_server *server,
    const chttp_web_security_policy *policy) {
  if (server == NULL || !chttp_web_security_policy_valid(policy))
    return SALTS_EINVAL;
  return chttp_server_use(
      server, chttp_web_security_middleware, (void *)policy);
}
