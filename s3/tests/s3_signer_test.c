#include <s3/s3_signer.h>

#include "tinytest.h"

#include <string.h>

static const char s3_empty_sha256[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

suite("S3 Signature Version 4") {
  it("matches the published AWS GET object vector") {
    static const s3_signer_header headers[] = {{"Host", "examplebucket.s3.amazonaws.com"},
                                               {"Range", "bytes=0-9"}};
    const s3_signer_request request = {.size = sizeof(request),
                                       .method = "GET",
                                       .canonical_uri = "/test.txt",
                                       .region = "us-east-1",
                                       .access_key = "AKIAIOSFODNN7EXAMPLE",
                                       .secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
                                       .payload_sha256 = s3_empty_sha256,
                                       .amz_date = "20130524T000000Z",
                                       .headers = headers,
                                       .header_count = sizeof(headers) / sizeof(headers[0])};
    s3_signer_result result = {0};

    check_equal(s3_signer_sign(&request, &result), SALTS_OK);
    check_equal(result.authorization,
                "AWS4-HMAC-SHA256 "
                "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,"
                "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,"
                "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    check_equal(result.signed_headers, "host;range;x-amz-content-sha256;x-amz-date");
    check_equal(result.signature,
                "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    s3_signer_result_destroy(&result);
  }

  it("encodes then sorts repeated query names without normalizing the path") {
    static const s3_signer_header headers[] = {{"host", "example.test"}};
    static const s3_signer_query queries[] = {
        {"prefix", "a/b"}, {"marker", "x y"}, {"prefix", "a+b"}, {"acl", ""}};
    const s3_signer_request request = {.size = sizeof(request),
                                       .method = "GET",
                                       .canonical_uri = "/bucket/a//b",
                                       .region = "us-east-1",
                                       .access_key = "access",
                                       .secret_key = "secret",
                                       .session_token = "token",
                                       .payload_sha256 = s3_empty_sha256,
                                       .amz_date = "20260903T010203Z",
                                       .headers = headers,
                                       .header_count = 1u,
                                       .query = queries,
                                       .query_count = sizeof(queries) / sizeof(queries[0])};
    s3_signer_result result = {0};

    check_equal(s3_signer_sign(&request, &result), SALTS_OK);
    check_equal(result.canonical_query, "acl=&marker=x%20y&prefix=a%2Bb&prefix=a%2Fb");
    check_contains(result.canonical_request, "/bucket/a//b\n");
    check_contains(result.signed_headers, "x-amz-security-token");
    s3_signer_result_destroy(&result);
  }

  it("rejects invalid dates duplicate normalized headers and capacity overflow") {
    static const s3_signer_header duplicate_headers[] = {{"Host", "example.test"},
                                                         {"host", "other.test"}};
    s3_signer_request request = {.size = sizeof(request),
                                 .method = "GET",
                                 .canonical_uri = "/",
                                 .region = "us-east-1",
                                 .access_key = "access",
                                 .secret_key = "secret",
                                 .payload_sha256 = s3_empty_sha256,
                                 .amz_date = "2026-09-03",
                                 .headers = duplicate_headers,
                                 .header_count = 2u};
    s3_signer_result result = {0};

    check_equal(s3_signer_sign(&request, &result), SALTS_EINVAL);
    request.amz_date = "20260903T010203Z";
    check_equal(s3_signer_sign(&request, &result), SALTS_EINVAL);
    request.header_count = 1u;
    request.max_header_count = 2u;
    check_equal(s3_signer_sign(&request, &result), SALTS_ENOBUFS);
    check_null(result.authorization);
  }

  it("matches an independently calculated query-auth vector") {
    static const s3_signer_query query[] = {{"response-content-type", "text/plain"}};
    const s3_presign_request request = {.size = sizeof(request),
                                        .method = "GET",
                                        .canonical_uri = "/bucket/a%20b",
                                        .authority = "example.com",
                                        .region = "us-east-1",
                                        .access_key = "AKIAIOSFODNN7EXAMPLE",
                                        .secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
                                        .amz_date = "20130524T000000Z",
                                        .expires_seconds = 60u,
                                        .query = query,
                                        .query_count = 1u};
    s3_presign_result result = {0};

    check_equal(s3_signer_presign(&request, &result), SALTS_OK);
    check_equal(result.signature,
                "c37f9eb5c09ebe0ae4bceb51b69afa4dd86d044c178f08a55189bc56c0ddacdd");
    check_equal(result.canonical_query,
                "X-Amz-Algorithm=AWS4-HMAC-SHA256&"
                "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2F"
                "aws4_request&X-Amz-Date=20130524T000000Z&X-Amz-Expires=60&"
                "X-Amz-Signature="
                "c37f9eb5c09ebe0ae4bceb51b69afa4dd86d044c178f08a55189bc56c0ddacdd&"
                "X-Amz-SignedHeaders=host&response-content-type=text%2Fplain");
    s3_presign_result_destroy(&result);
  }

  it("signs temporary credentials and rejects reserved query-auth fields") {
    s3_signer_query query[] = {{"response-content-disposition", "attachment"}};
    s3_presign_request request = {.size = sizeof(request),
                                  .method = "GET",
                                  .canonical_uri = "/bucket/key",
                                  .authority = "example.com",
                                  .region = "us-east-1",
                                  .access_key = "access",
                                  .secret_key = "secret",
                                  .session_token = "token/+=",
                                  .amz_date = "20260903T010203Z",
                                  .expires_seconds = 604800u,
                                  .query = query,
                                  .query_count = 1u};
    s3_presign_result result = {0};

    check_equal(s3_signer_presign(&request, &result), SALTS_OK);
    check_contains(result.canonical_query, "X-Amz-Security-Token=token%2F%2B%3D");
    s3_presign_result_destroy(&result);

    request.expires_seconds = 0u;
    check_equal(s3_signer_presign(&request, &result), SALTS_EINVAL);
    request.expires_seconds = 604801u;
    check_equal(s3_signer_presign(&request, &result), SALTS_EINVAL);
    request.expires_seconds = 60u;
    query[0] = (s3_signer_query){"x-amz-signature", "caller-value"};
    check_equal(s3_signer_presign(&request, &result), SALTS_EINVAL);
    check_null(result.canonical_query);
    check_null(result.signature);
  }
}
