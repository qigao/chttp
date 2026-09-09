#include <s3/s3_credentials.h>

#include <stdlib.h>

static int s3_static_fetch(void *user, s3_credentials *out_credentials) {
  const s3_static_credentials *credentials = (const s3_static_credentials *)user;

  if (credentials == NULL || out_credentials == NULL || credentials->access_key == NULL ||
      credentials->secret_key == NULL || credentials->access_key[0] == '\0' ||
      credentials->secret_key[0] == '\0')
    return SALTS_EINVAL;
  *out_credentials = (s3_credentials){credentials->access_key, credentials->secret_key,
                                      credentials->session_token};
  return SALTS_OK;
}

static int s3_environment_fetch(void *user, s3_credentials *out_credentials) {
  const char *access_key;
  const char *secret_key;
  (void)user;

  if (out_credentials == NULL) return SALTS_EINVAL;
  access_key = getenv("AWS_ACCESS_KEY_ID");
  secret_key = getenv("AWS_SECRET_ACCESS_KEY");
  if (access_key == NULL || secret_key == NULL || access_key[0] == '\0' || secret_key[0] == '\0')
    return SALTS_ENOENT;
  *out_credentials = (s3_credentials){access_key, secret_key, getenv("AWS_SESSION_TOKEN")};
  return SALTS_OK;
}

s3_credentials_provider s3_credentials_provider_static(const s3_static_credentials *credentials) {
  return (s3_credentials_provider){s3_static_fetch, NULL, (void *)credentials};
}

s3_credentials_provider s3_credentials_provider_environment(void) {
  return (s3_credentials_provider){s3_environment_fetch, NULL, NULL};
}
