# S3 migration provenance

This module was implemented for Salts from the S3 protocol boundary and test
ideas in a same-owner legacy HTTP codebase at source revision
`5f1068f5194f94472e54a185ec51638f421d4fc5` (2026-09-02).

At the migration revision, the recorded source repository does not contain a
root `LICENSE`, `COPYING`, or `NOTICE`
file. The repository owner explicitly requested this migration. This record is
not a substitute for a license grant to unrelated third parties.

The migration retains the following conceptual mapping:

| Legacy source | Salts destination | Adaptation |
| --- | --- | --- |
| `s3/src/s3_signer.c` | `s3/src/s3_signer.c` | Rebuilt on BoringSSL's OpenSSL-compatible API with strict bounds, duplicate handling, key cleansing, and AWS vectors |
| `s3/src/s3_url.c`, `s3/src/s3_multimap.c` | `s3/src/s3_request.c` | One encoded/sorted request-plan fact source; repeated query names retained |
| `s3/src/s3_http.c` | `s3/src/s3_client.c` | Replaced the legacy CoroNet/H3 facade with injected CHTTP H1/H2 clients |
| `s3/src/s3_response.c` | `s3/src/s3_xml.c` | Replaced unbounded parser use and timestamp placeholders with the bounded Salts XML facade |
| `s3/tests/test_s3_streaming.c` | `s3/tests/s3_file_test.c` | Replaced Iris/CoroNet fixture with CHTTP H1/H2 mock server |

No HTTP/3 source or behavior is included. The legacy presigned POST path,
which contains a fixed expiration placeholder at the source revision, is not
published by this module.
