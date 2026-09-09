#ifndef S3_CREDENTIALS_H
#define S3_CREDENTIALS_H

#include <salts/error_codes.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Provider-owned credential views. They remain valid until `release` returns. */
typedef struct s3_credentials {
  const char *access_key;
  const char *secret_key;
  const char *session_token;
} s3_credentials;

typedef int (*s3_credentials_fetch_fn)(void *user, s3_credentials *out_credentials);
typedef void (*s3_credentials_release_fn)(void *user, s3_credentials *credentials);

/** Borrowed provider operations. S3 never destroys `user`. */
typedef struct s3_credentials_provider {
  s3_credentials_fetch_fn fetch;
  s3_credentials_release_fn release;
  void *user;
} s3_credentials_provider;

/** Caller-owned storage for the allocation-free static provider. */
typedef struct s3_static_credentials {
  const char *access_key;
  const char *secret_key;
  const char *session_token;
} s3_static_credentials;

/** Returns a provider borrowing `credentials`; the storage and strings must outlive every fetch. */
s3_credentials_provider s3_credentials_provider_static(const s3_static_credentials *credentials);

/** Reads AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, and optional AWS_SESSION_TOKEN per fetch. */
s3_credentials_provider s3_credentials_provider_environment(void);

#ifdef __cplusplus
}
#endif

#endif /* S3_CREDENTIALS_H */
