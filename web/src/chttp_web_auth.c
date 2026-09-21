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
