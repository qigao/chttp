#include <http_client/http.h>
#include <http_server/http.h>

#include "chttp_jwt_internal.h"
#include "chttp_server_runtime.h"
#include "tinytest.h"

#include <string.h>

static const unsigned char valid_key[] = "0123456789abcdef0123456789abcdef";
static const unsigned char short_key[] = "0123456789abcdef0123456789abcde";

static int chttp_jwt_test_validate_at(const chttp_jwt_claims *claims,
                                      chttp_jwt_bearer_validator *validator,
                                      chttp_server_request_state *state, int64_t now_seconds) {
  char *token = NULL;
  char authorization[512];
  chttp_header header = {0};
  chttp_server_request_view request = {0};
  int status = chttp_jwt_hs256_token_create(claims, valid_key, sizeof(valid_key) - 1u, &token);
  if (status != SALTS_OK) return status;
  status = chttp_jwt_bearer_header(token, authorization, sizeof(authorization), &header);
  if (status == SALTS_OK) {
    request.headers = &header;
    request.header_count = 1u;
    status = chttp_jwt_bearer_request_validate_at(state, &request, validator, now_seconds);
  }
  chttp_jwt_token_destroy(token);
  return status;
}

spec("chttp jwt") {
  group("HS256 key policy") {
    it("rejects HS256 secrets shorter than 32 bytes") {
      const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
      chttp_jwt_bearer_validator validator = {0};
      const chttp_jwt_bearer_validator_options options = {
          .size = sizeof(options), .key = short_key, .key_size = sizeof(short_key) - 1u};
      char *token = NULL;

      check_equal(chttp_jwt_hs256_token_create(&claims, short_key, sizeof(short_key) - 1u, &token),
                  SALTS_EINVAL);
      check_null(token);
      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_EINVAL);
      check_null(validator.impl);
    }

    it("accepts a 32 byte HS256 secret") {
      const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
      char *token = NULL;

      check_equal(chttp_jwt_hs256_token_create(&claims, valid_key, sizeof(valid_key) - 1u, &token),
                  SALTS_OK);
      check_not_null(token);
      chttp_jwt_token_destroy(token);
    }
  }

  /* Deterministic RED: wall-clock behavior is tested through the private validate-at seam. */
  group("HS256 validation time policy") {
    it("requires exp by default and rejects the exact expiration boundary") {
      chttp_server_request_state state = {0};
      chttp_jwt_bearer_validator validator = {0};
      const chttp_jwt_bearer_validator_options options = {
          .size = sizeof(options), .key = valid_key, .key_size = sizeof(valid_key) - 1u};
      const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(200)};

      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(199)), SALTS_OK);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)),
                  SALTS_EPERM);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    }

    it("rejects a missing exp unless explicitly allowed") {
      const chttp_jwt_claims claims = {.subject = "alice"};
      chttp_server_request_state state = {0};
      chttp_jwt_bearer_validator validator = {0};
      chttp_jwt_bearer_validator_options options = {
          .size = sizeof(options), .key = valid_key, .key_size = sizeof(valid_key) - 1u};

      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)),
                  SALTS_EPERM);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);

      options.allow_missing_exp = 1;
      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)), SALTS_OK);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    }

    it("applies expiration skew with a strict upper boundary") {
      const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(200)};
      chttp_server_request_state state = {0};
      chttp_jwt_bearer_validator validator = {0};
      const chttp_jwt_bearer_validator_options options = {.size = sizeof(options),
                                                           .key = valid_key,
                                                           .key_size = sizeof(valid_key) - 1u,
                                                           .clock_skew_seconds = INT64_C(5)};

      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(204)), SALTS_OK);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(205)),
                  SALTS_EPERM);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    }

    it("accepts nbf at its exact boundary") {
      const chttp_jwt_claims claims = {
          .subject = "alice", .not_before = INT64_C(200), .expires_at = INT64_C(300)};
      chttp_server_request_state state = {0};
      chttp_jwt_bearer_validator validator = {0};
      const chttp_jwt_bearer_validator_options options = {
          .size = sizeof(options), .key = valid_key, .key_size = sizeof(valid_key) - 1u};

      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(199)),
                  SALTS_EPERM);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)), SALTS_OK);
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    }
  }

  group("validator string ownership and matching") {
    static chttp_server_request_state state;
    static chttp_jwt_bearer_validator validator;
    static char *token;

    before_each() {
      state = (chttp_server_request_state){0};
      validator = (chttp_jwt_bearer_validator){0};
      token = NULL;
    }
    after_each() {
      chttp_jwt_request_state_reset(&state);
      check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
      chttp_jwt_token_destroy(token);
    }

    it("owns issuer and audience independently of the options and compares exact case") {
      char issuer[] = "Issuer.Example";
      char audience[] = "Api.Example";
      const chttp_jwt_bearer_validator_options options = {
          .size = sizeof(options), .key = valid_key, .key_size = sizeof(valid_key) - 1u,
          .expected_issuer = issuer, .expected_audience = audience};
      chttp_jwt_claims claims = {
          .issuer = "Issuer.Example", .audience = "Api.Example", .expires_at = INT64_C(300)};
      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
      memset(issuer, 'x', sizeof(issuer) - 1u);
      memset(audience, 'y', sizeof(audience) - 1u);
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)), SALTS_OK);
      chttp_jwt_request_state_reset(&state);
      claims.issuer = "issuer.example";
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)), SALTS_EPERM);
      claims.issuer = "Issuer.Example";
      claims.audience = "api.example";
      check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)), SALTS_EPERM);
    }

    it("keeps each absent optional restriction disabled") {
      const chttp_jwt_claims claims = {
          .issuer = "issuer", .audience = "audience", .expires_at = INT64_C(300)};
      for (unsigned int mask = 0u; mask < 4u; ++mask) {
        const chttp_jwt_bearer_validator_options options = {
            .size = sizeof(options), .key = valid_key, .key_size = sizeof(valid_key) - 1u,
            .expected_issuer = (mask & 1u) != 0u ? "issuer" : NULL,
            .expected_audience = (mask & 2u) != 0u ? "audience" : NULL};
        check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
        check_equal(chttp_jwt_test_validate_at(&claims, &validator, &state, INT64_C(200)), SALTS_OK);
        chttp_jwt_request_state_reset(&state);
        check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
      }
    }

    it("accepts ASCII case variants but rejects duplicate or malformed Authorization") {
      const chttp_jwt_claims claims = {.expires_at = INT64_C(300)};
      const chttp_jwt_bearer_validator_options options = {
          .size = sizeof(options), .key = valid_key, .key_size = sizeof(valid_key) - 1u};
      char authorization[512];
      chttp_header headers[2] = {{0}};
      chttp_server_request_view request = {.headers = headers, .header_count = 1u};
      check_equal(chttp_jwt_bearer_validator_init(&validator, &options), SALTS_OK);
      check_equal(chttp_jwt_hs256_token_create(&claims, valid_key, sizeof(valid_key) - 1u, &token),
                  SALTS_OK);
      check_equal(chttp_jwt_bearer_header(token, authorization, sizeof(authorization), &headers[0]),
                  SALTS_OK);
      headers[0].name = "aUtHoRiZaTiOn";
      memcpy(authorization, "bEaReR", 6u);
      check_equal(chttp_jwt_bearer_request_validate_at(&state, &request, &validator, INT64_C(200)),
                  SALTS_OK);
      chttp_jwt_request_state_reset(&state);
      headers[1] = (chttp_header){"AUTHORIZATION", authorization};
      request.header_count = 2u;
      check_equal(chttp_jwt_bearer_request_validate_at(&state, &request, &validator, INT64_C(200)),
                  SALTS_EPERM);
      request.header_count = 1u;
      authorization[6] = '\t';
      check_equal(chttp_jwt_bearer_request_validate_at(&state, &request, &validator, INT64_C(200)),
                  SALTS_EPERM);
      authorization[6] = ' ';
      authorization[1] = (char)0xe9;
      check_equal(chttp_jwt_bearer_request_validate_at(&state, &request, &validator, INT64_C(200)),
                  SALTS_EPERM);
      headers[0].value = "Bearer ";
      check_equal(chttp_jwt_bearer_request_validate_at(&state, &request, &validator, INT64_C(200)),
                  SALTS_EPERM);
    }
  }

  group("HS256 client token") {
    it("creates a token and formats an Authorization Bearer header") {
      const chttp_jwt_claims claims = {
          .issuer = "issuer.example",
          .subject = "alice",
          .audience = "api.example",
          .issued_at = 1700000000,
          .not_before = 1700000000,
          .expires_at = 1700003600,
      };
      char *token = NULL;
      char authorization[512];
      chttp_header header = {0};

      check_equal(chttp_jwt_hs256_token_create(&claims, valid_key, sizeof(valid_key) - 1u, &token),
                  SALTS_OK);
      check_not_null(token);
      check_equal(chttp_jwt_bearer_header(token, authorization, sizeof(authorization), &header),
                  SALTS_OK);
      check_equal(header.name, "Authorization");
      check_equal(strncmp(header.value, "Bearer ", strlen("Bearer ")), 0);
      check_equal(header.value + strlen("Bearer "), token);

      chttp_jwt_token_destroy(token);
    }
  }
}
