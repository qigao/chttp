#include <chttp_web/web.h>

#include <salts/error_codes.h>

#include <stdio.h>
#include <string.h>

#define CHTTP_WEB_PRINCIPAL_SUBJECT_KEY "__chttp_web_principal_subject_v1"
#define CHTTP_WEB_PRINCIPAL_ROLE_KEY "__chttp_web_principal_role_v1"
#define CHTTP_WEB_PRINCIPAL_DISPLAY_KEY "__chttp_web_principal_display_v1"

static chttp_web_status chttp_web_auth_fail(
    chttp_web_error *error,
    chttp_web_status status,
    int native_status,
    const char *message) {
  if (error != NULL) {
    error->status = status;
    error->native_status = native_status;
    error->offset = 0u;
    error->template_name[0] = '\0';
    if (message != NULL)
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
    else
      error->message[0] = '\0';
  }
  return status;
}

static chttp_web_status chttp_web_auth_session_status(
    chttp_web_error *error,
    int status,
    const char *message) {
  if (status == SALTS_OK)
    return chttp_web_auth_fail(error, CHTTP_WEB_OK, 0, NULL);
  if (status == SALTS_ENOBUFS || status == SALTS_EMSGSIZE)
    return chttp_web_auth_fail(
        error, CHTTP_WEB_CAPACITY, status, message);
  return chttp_web_auth_fail(
      error, CHTTP_WEB_SERVER, status, message);
}

static chttp_web_string_view chttp_web_auth_string(const char *value) {
  if (value == NULL) return (chttp_web_string_view){NULL, 0u};
  return (chttp_web_string_view){value, strlen(value)};
}

static int chttp_web_auth_remove_optional(
    chttp_session *session,
    const char *key) {
  const int status = chttp_session_remove(session, key);
  return status == SALTS_ENOENT ? SALTS_OK : status;
}

static chttp_web_status chttp_web_auth_fail_closed(
    chttp_session *session,
    chttp_web_error *error,
    int status,
    const char *message) {
  const int invalidate_status = chttp_session_invalidate(session);
  if (invalidate_status != SALTS_OK)
    return chttp_web_auth_fail(
        error,
        CHTTP_WEB_SERVER,
        invalidate_status,
        "unable to invalidate partially authenticated Session");
  return chttp_web_auth_session_status(error, status, message);
}

chttp_web_status chttp_web_principal_get(
    const chttp_server_request_view *request,
    chttp_web_principal *out_principal,
    chttp_web_error *error) {
  const char *subject;
  const char *role;
  const char *display_name;

  if (request == NULL || out_principal == NULL ||
      out_principal->size < sizeof(*out_principal))
    return chttp_web_auth_fail(
        error,
        CHTTP_WEB_INVALID_ARGUMENT,
        0,
        "principal request/output size is invalid");

  *out_principal = (chttp_web_principal)CHTTP_WEB_PRINCIPAL_INIT;
  if (request->session == NULL)
    return chttp_web_auth_fail(error, CHTTP_WEB_OK, 0, NULL);

  subject =
      chttp_session_get(request->session, CHTTP_WEB_PRINCIPAL_SUBJECT_KEY);
  if (subject == NULL || subject[0] == '\0')
    return chttp_web_auth_fail(error, CHTTP_WEB_OK, 0, NULL);

  role = chttp_session_get(request->session, CHTTP_WEB_PRINCIPAL_ROLE_KEY);
  display_name =
      chttp_session_get(request->session, CHTTP_WEB_PRINCIPAL_DISPLAY_KEY);

  out_principal->authenticated = true;
  out_principal->subject = chttp_web_auth_string(subject);
  out_principal->role = chttp_web_auth_string(role);
  out_principal->display_name = chttp_web_auth_string(display_name);
  return chttp_web_auth_fail(error, CHTTP_WEB_OK, 0, NULL);
}

static int chttp_web_auth_hex_value(unsigned char value) {
  if (value >= (unsigned char)'0' && value <= (unsigned char)'9')
    return (int)(value - (unsigned char)'0');
  if (value >= (unsigned char)'a' && value <= (unsigned char)'f')
    return (int)(value - (unsigned char)'a') + 10;
  if (value >= (unsigned char)'A' && value <= (unsigned char)'F')
    return (int)(value - (unsigned char)'A') + 10;
  return -1;
}

static chttp_web_status chttp_web_auth_target_fail(
    chttp_web_error *error,
    chttp_web_status status,
    int native_status,
    size_t offset,
    const char *message) {
  (void)chttp_web_auth_fail(error, status, native_status, message);
  if (error != NULL) error->offset = offset;
  return status;
}

chttp_web_status chttp_web_local_target_validate(
    const char *target,
    size_t target_size,
    size_t max_target_bytes,
    chttp_web_error *error) {
  size_t i;

  if (target == NULL || max_target_bytes == 0u)
    return chttp_web_auth_target_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL, 0u,
        "local target arguments are invalid");
  if (target_size == 0u || target[0] != '/')
    return chttp_web_auth_target_fail(
        error, CHTTP_WEB_AUTH, SALTS_EPERM, 0u,
        "browser return target must be origin-form");
  if (target_size > max_target_bytes ||
      target_size > CHTTP_WEB_LOCAL_TARGET_HARD_MAX)
    return chttp_web_auth_target_fail(
        error, CHTTP_WEB_CAPACITY, SALTS_EMSGSIZE, target_size,
        "browser return target exceeds its byte bound");
  if (target_size > 1u && target[1] == '/')
    return chttp_web_auth_target_fail(
        error, CHTTP_WEB_AUTH, SALTS_EPERM, 1u,
        "scheme-relative browser return targets are forbidden");

  for (i = 0u; i < target_size; ++i) {
    const unsigned char value = (unsigned char)target[i];
    if (value < 0x20u || value == 0x7fu || value == (unsigned char)'\\' ||
        value == (unsigned char)'#')
      return chttp_web_auth_target_fail(
          error, CHTTP_WEB_AUTH, SALTS_EPERM, i,
          "browser return target contains a forbidden byte");

    if (value == (unsigned char)'%') {
      int high;
      int low;
      unsigned char decoded;
      if (i + 2u >= target_size)
        return chttp_web_auth_target_fail(
            error, CHTTP_WEB_AUTH, SALTS_EPERM, i,
            "browser return target has a truncated percent escape");
      high = chttp_web_auth_hex_value((unsigned char)target[i + 1u]);
      low = chttp_web_auth_hex_value((unsigned char)target[i + 2u]);
      if (high < 0 || low < 0)
        return chttp_web_auth_target_fail(
            error, CHTTP_WEB_AUTH, SALTS_EPERM, i,
            "browser return target has an invalid percent escape");
      decoded = (unsigned char)((high << 4) | low);
      if (decoded < 0x20u || decoded == 0x7fu ||
          decoded == (unsigned char)'\\' ||
          decoded == (unsigned char)'#' ||
          (i == 1u && decoded == (unsigned char)'/'))
        return chttp_web_auth_target_fail(
            error, CHTTP_WEB_AUTH, SALTS_EPERM, i,
            "browser return target percent-encodes a forbidden byte");
      i += 2u;
    }
  }

  return chttp_web_auth_fail(error, CHTTP_WEB_OK, 0, NULL);
}

static int chttp_web_auth_bounded_length(
    const char *value,
    size_t limit,
    size_t *out_size) {
  size_t size;
  if (value == NULL || out_size == NULL) return SALTS_EINVAL;
  for (size = 0u; size <= limit; ++size) {
    if (value[size] == '\0') {
      *out_size = size;
      return SALTS_OK;
    }
  }
  return SALTS_EMSGSIZE;
}

static int chttp_web_auth_unreserved(unsigned char value) {
  return (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
         (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
         (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
         value == (unsigned char)'-' || value == (unsigned char)'.' ||
         value == (unsigned char)'_' || value == (unsigned char)'~';
}

enum {
  CHTTP_WEB_LOGIN_DESTINATION_HARD_MAX =
      CHTTP_WEB_LOGIN_PATH_HARD_MAX +
      3 * CHTTP_WEB_LOCAL_TARGET_HARD_MAX + 16
};

static int chttp_web_auth_policy_valid(
    const chttp_web_auth_policy *policy) {
  size_t login_size = 0u;
  chttp_web_status status;

  if (policy == NULL || policy->size < sizeof(*policy) ||
      chttp_web_auth_bounded_length(
          policy->login_path, CHTTP_WEB_LOGIN_PATH_HARD_MAX,
          &login_size) != SALTS_OK ||
      login_size == 0u)
    return 0;

  status = chttp_web_local_target_validate(
      policy->login_path, login_size, CHTTP_WEB_LOGIN_PATH_HARD_MAX, NULL);
  if (status != CHTTP_WEB_OK) return 0;

  if (policy->max_return_target_bytes > CHTTP_WEB_LOCAL_TARGET_HARD_MAX)
    return 0;
  if (policy->include_return_target &&
      policy->max_return_target_bytes == 0u)
    return 0;
  return 1;
}

static chttp_web_status chttp_web_auth_login_destination(
    const chttp_web_auth_policy *policy,
    const chttp_server_request_view *request,
    char *storage,
    size_t storage_capacity,
    chttp_web_error *error) {
  static const char parameter[] = "return_to=";
  static const char hex[] = "0123456789ABCDEF";
  size_t login_size = 0u;
  size_t target_size = 0u;
  size_t output_size;
  size_t i;
  chttp_web_status status;

  if (policy == NULL || request == NULL || storage == NULL ||
      storage_capacity == 0u ||
      !policy->include_return_target ||
      request->target == NULL)
    return chttp_web_auth_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL,
        "login destination arguments are invalid");

  if (chttp_web_auth_bounded_length(
          policy->login_path, CHTTP_WEB_LOGIN_PATH_HARD_MAX,
          &login_size) != SALTS_OK ||
      chttp_web_auth_bounded_length(
          request->target, policy->max_return_target_bytes,
          &target_size) != SALTS_OK)
    return chttp_web_auth_fail(
        error, CHTTP_WEB_CAPACITY, SALTS_EMSGSIZE,
        "login or return target exceeds its byte bound");

  status = chttp_web_local_target_validate(
      request->target, target_size,
      policy->max_return_target_bytes, error);
  if (status != CHTTP_WEB_OK) return status;

  if (login_size + sizeof(parameter) + 1u >= storage_capacity ||
      target_size >
          (storage_capacity - login_size - sizeof(parameter) - 1u) / 3u)
    return chttp_web_auth_fail(
        error, CHTTP_WEB_CAPACITY, SALTS_EMSGSIZE,
        "encoded login destination exceeds its byte bound");

  memcpy(storage, policy->login_path, login_size);
  output_size = login_size;
  storage[output_size++] =
      memchr(policy->login_path, '?', login_size) != NULL ? '&' : '?';
  memcpy(storage + output_size, parameter, sizeof(parameter) - 1u);
  output_size += sizeof(parameter) - 1u;

  for (i = 0u; i < target_size; ++i) {
    const unsigned char value = (unsigned char)request->target[i];
    if (chttp_web_auth_unreserved(value)) {
      storage[output_size++] = (char)value;
    } else {
      storage[output_size++] = '%';
      storage[output_size++] = hex[value >> 4u];
      storage[output_size++] = hex[value & 0x0fu];
    }
  }
  storage[output_size] = '\0';
  return chttp_web_auth_fail(error, CHTTP_WEB_OK, 0, NULL);
}

static int chttp_web_auth_web_result(
    chttp_web_status status,
    const chttp_web_error *error) {
  if (status == CHTTP_WEB_OK) return SALTS_OK;
  if (error != NULL && error->native_status != 0)
    return error->native_status;
  if (status == CHTTP_WEB_CAPACITY) return SALTS_ENOBUFS;
  if (status == CHTTP_WEB_INVALID_ARGUMENT) return SALTS_EINVAL;
  return SALTS_EIO;
}

static int chttp_web_auth_forbidden(
    const chttp_web_auth_policy *policy,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  if (policy->forbidden != NULL)
    return policy->forbidden(
        policy->forbidden_user, request, response);
  return chttp_server_reply(response, 403u, NULL, NULL, 0u);
}

static int chttp_web_auth_unauthenticated(
    const chttp_web_auth_policy *policy,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  char destination_storage[CHTTP_WEB_LOGIN_DESTINATION_HARD_MAX + 1u];
  const char *destination = policy->login_path;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  if (policy->include_return_target) {
    status = chttp_web_auth_login_destination(
        policy, request, destination_storage,
        sizeof(destination_storage), &error);
    if (status == CHTTP_WEB_AUTH || status == CHTTP_WEB_CAPACITY)
      return chttp_server_reply(response, 400u, NULL, NULL, 0u);
    if (status != CHTTP_WEB_OK)
      return chttp_web_auth_web_result(status, &error);
    destination = destination_storage;
  }

  if (chttp_web_request_is_htmx(request)) {
    status = chttp_web_hx_redirect(response, destination, &error);
    if (status != CHTTP_WEB_OK)
      return chttp_web_auth_web_result(status, &error);
    return chttp_server_reply(response, 200u, NULL, NULL, 0u);
  }

  status = chttp_web_redirect(response, 303u, destination, &error);
  return chttp_web_auth_web_result(status, &error);
}

int chttp_web_auth_middleware(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response,
    chttp_server_next *next) {
  const chttp_web_auth_policy *policy =
      (const chttp_web_auth_policy *)user;
  chttp_web_principal principal = CHTTP_WEB_PRINCIPAL_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status web_status;
  int status;

  if (!chttp_web_auth_policy_valid(policy) ||
      request == NULL || response == NULL || next == NULL)
    return SALTS_EINVAL;

  web_status = chttp_web_principal_get(request, &principal, &error);
  if (web_status != CHTTP_WEB_OK)
    return chttp_web_auth_web_result(web_status, &error);

  if (!principal.authenticated)
    return chttp_web_auth_unauthenticated(
        policy, request, response);

  if (policy->authorize != NULL) {
    status = policy->authorize(
        policy->authorize_user, &principal, request);
    if (status == SALTS_EPERM)
      return chttp_web_auth_forbidden(
          policy, request, response);
    if (status != SALTS_OK) return status;
  }

  return chttp_server_next_call(next);
}

chttp_web_status chttp_web_principal_sign_in(
    const chttp_server_request_view *request,
    const chttp_web_form *form,
    const chttp_web_principal_input *principal,
    const char **out_csrf_token,
    chttp_web_error *error) {
  const char *csrf = NULL;
  int status;
  chttp_web_status web_status;

  if (out_csrf_token != NULL) *out_csrf_token = NULL;
  if (request == NULL || request->session == NULL ||
      principal == NULL ||
      principal->size < sizeof(*principal) ||
      principal->subject == NULL ||
      principal->subject[0] == '\0' ||
      request->method != CHTTP_METHOD_POST)
    return chttp_web_auth_fail(
        error,
        CHTTP_WEB_INVALID_ARGUMENT,
        0,
        "POST login requires a Session and non-empty verified subject");

  web_status = chttp_web_csrf_validate(request, form, error);
  if (web_status != CHTTP_WEB_OK) return web_status;

  status = chttp_session_regenerate(request->session);
  if (status != SALTS_OK)
    return chttp_web_auth_session_status(
        error, status, "unable to regenerate login Session");

  status = chttp_web_auth_remove_optional(
      request->session, CHTTP_WEB_PRINCIPAL_SUBJECT_KEY);
  if (status == SALTS_OK)
    status = chttp_web_auth_remove_optional(
        request->session, CHTTP_WEB_PRINCIPAL_ROLE_KEY);
  if (status == SALTS_OK)
    status = chttp_web_auth_remove_optional(
        request->session, CHTTP_WEB_PRINCIPAL_DISPLAY_KEY);
  if (status != SALTS_OK)
    return chttp_web_auth_fail_closed(
        request->session,
        error,
        status,
        "unable to clear previous principal state");

  status = chttp_session_set(
      request->session,
      CHTTP_WEB_PRINCIPAL_SUBJECT_KEY,
      principal->subject);
  if (status == SALTS_OK &&
      principal->role != NULL &&
      principal->role[0] != '\0')
    status = chttp_session_set(
        request->session,
        CHTTP_WEB_PRINCIPAL_ROLE_KEY,
        principal->role);
  if (status == SALTS_OK &&
      principal->display_name != NULL &&
      principal->display_name[0] != '\0')
    status = chttp_session_set(
        request->session,
        CHTTP_WEB_PRINCIPAL_DISPLAY_KEY,
        principal->display_name);
  if (status != SALTS_OK)
    return chttp_web_auth_fail_closed(
        request->session,
        error,
        status,
        "unable to write authenticated principal state");

  web_status =
      chttp_web_csrf_rotate(request->session, &csrf, error);
  if (web_status != CHTTP_WEB_OK) {
    const int invalidate_status =
        chttp_session_invalidate(request->session);
    if (invalidate_status != SALTS_OK)
      return chttp_web_auth_fail(
          error,
          CHTTP_WEB_SERVER,
          invalidate_status,
          "unable to invalidate Session after CSRF rotation failure");
    return web_status;
  }

  if (out_csrf_token != NULL) *out_csrf_token = csrf;
  return chttp_web_auth_fail(error, CHTTP_WEB_OK, 0, NULL);
}

chttp_web_status chttp_web_principal_sign_out(
    const chttp_server_request_view *request,
    const chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_status web_status;
  int status;

  if (request == NULL || request->session == NULL ||
      request->method != CHTTP_METHOD_POST)
    return chttp_web_auth_fail(
        error,
        CHTTP_WEB_INVALID_ARGUMENT,
        0,
        "POST logout requires an active Session");

  web_status = chttp_web_csrf_validate(request, form, error);
  if (web_status != CHTTP_WEB_OK) return web_status;

  status = chttp_session_invalidate(request->session);
  return chttp_web_auth_session_status(
      error, status, "unable to invalidate logout Session");
}
