# S3 over CHTTP design

## Decision context

legacy HTTP repository contains an S3 module, but its current `main` revision
`5f1068f5194f94472e54a185ec51638f421d4fc5` is coupled to CoroNet and the
`salts_http` H1/H2/H3 facade. It also contains incomplete presigned-POST and
timestamp parsing paths. The source repository has no root `LICENSE`,
`COPYING`, or `NOTICE` file. This migration was explicitly requested by the
repository owner; `s3/PROVENANCE.md` records the exact source and which ideas
and tests were adapted. No HTTP/3 code, fallback policy, or CoroNet type is
copied into the public or private dependency graph.

The new subsystem affects the root build, installed package, CHTTP transport,
XML parsing, cryptographic signing, filesystem streaming, and public ABI. It
therefore requires an explicit architecture decision.

## Candidate designs

### Put S3 in CNet

Rejected. CNet owns connections and byte-stream transport. Bucket addressing,
AWS Signature Version 4, XML service errors, and multipart upload are
application-protocol state. Putting them in CNet would reverse the existing
CHTTP dependency and make CNet depend on HTTP semantics.

### Extend CHTTP with S3 methods

Rejected. This makes the HTTP client responsible for credentials, object
storage naming, XML schemas, and multipart recovery. It also prevents other
CHTTP consumers from depending on a narrow HTTP ABI.

### Add a separate Salts::S3 adapter above CHTTP

Selected. `Salts::S3` owns S3 request construction and parsing, borrows an
injected CHTTP client, and uses the same public call for H1 or H2. CHTTP remains
the only owner of connections, TLS, HTTP framing, connection pooling, file
source/sink progress, and protocol shutdown.

## Module and dependency boundary

```text
application
    |
    v
Salts::S3  -- private --> BoringSSL OpenSSL-compatible Crypto target
    |        -- private --> Salts::XmlParser / Salts::CSTL / Salts::Core
    v
Salts::CHTTP
    v
Salts::CNet -> NativeIO
```

The installed C target is `Salts::S3`; its implementation library is
`salts_s3`, version and SOVERSION 1. Public headers include CHTTP types but do
not expose BoringSSL, llhttp, the XML engine, CoroNet, or H3 types.

## Public model

`s3_client` and `s3_async_client` are opaque wrappers. A blocking client borrows
one caller-owned `chttp_client`; an advanced client borrows one caller-owned
`chttp_async_client`. The S3 owner never initializes, polls, stops, or destroys
the borrowed HTTP owner. The blocking client is single-owner and one call at a time. The
advanced client follows CHTTP's single progress-owner rule and delivers
callback-scoped response views.

Every client config copies `connection_uri`, base `authority`, `region`, and
capacity settings. It borrows the TLS profile, credential-provider context, and
clock context until destroy. `protocol` is passed through unchanged; an H2
request never falls back to H1.

A credentials provider has `fetch` and optional `release` operations. A
successful fetch produces access-key, secret-key, and optional session-token
views which remain valid until S3 calls `release` exactly once after signing.
The static and environment providers obey the same contract. S3 never logs
credentials or derived signing keys and clears temporary key material before
freeing it.

Progress callbacks execute on the blocking client's owner thread. They may
observe transfer state but must not reenter or destroy the same S3/CHTTP client.

The generic request is the common fact source for convenience APIs:

```c
typedef struct s3_request_options {
  s3_method method;
  const char *bucket;
  const char *key;
  const s3_query_param *query;
  size_t query_count;
  const chttp_header *headers;
  size_t header_count;
  const void *body;
  size_t body_size;
  const chttp_body_source *body_source;
  const chttp_body_sink *body_sink;
  const char *payload_sha256;
} s3_request_options;
```

Memory bodies are hashed by S3. A streamed body must provide the lowercase
64-digit SHA-256 of the exact source bytes; file upload helpers compute it in a
bounded first pass and CHTTP performs the second-pass asynchronous file read.
`body` and `body_source` are mutually exclusive. A response sink is borrowed
only through the blocking call or async terminal callback.

`s3_response` owns status, copied response headers/body, and parsed service
error fields. `s3_response_view` is callback-scoped. A transport or local error
returns the underlying Salts status. A non-2xx S3 response returns
`SALTS_EPROTO`, preserves the HTTP response, and parses bounded `Code`,
`Message`, `RequestId`, and `HostId` fields when an XML error body is present.

## Addressing and signing invariant

Path-style requests use `authority` unchanged and target
`/<encoded-bucket>/<encoded-key>`. Virtual-hosted requests use
`<bucket>.<authority>` and target `/<encoded-key>`. Bucket labels are validated
before use. The caller's TLS profile must be valid for the resulting authority;
S3 does not weaken CHTTP hostname verification.

The request plan owns one canonical path, one sorted encoded query string, one
authority, and one normalized header set. The canonical request and final
CHTTP options borrow those exact values. After the signature is produced, no
transport-visible method, authority, target, signed header, or payload hash is
changed. `host` participates in signing but is omitted from application
headers because CHTTP emits it from `authority`.

AWS S3 paths are not normalized. `/`, repeated slashes, and object-key bytes
remain distinct. URI encoding uses uppercase hexadecimal, encodes every byte
except `A-Z a-z 0-9 - . _ ~`, and preserves `/` only between object-key
segments. Query names and values are encoded independently and sorted by
encoded name then encoded value. Repeated query names are retained.

## Capacity and lifetime protocol

The data unit is one S3 request plan plus one CHTTP request. The S3 client owns
copied endpoint strings and transient signing storage; CHTTP owns admitted
wire state. Credentials and user callbacks are borrowed. Blocking responses
are caller-owned until `s3_response_destroy`; async views expire when the
terminal callback returns.

All variable input has two bounds: the S3 config bound and the corresponding
CHTTP bound. The initial defaults are:

| Resource | Default hard bound | Full behavior |
| --- | ---: | --- |
| bucket name | 63 bytes | `SALTS_ENAMETOOLONG` |
| object key | 1,024 bytes | `SALTS_ENAMETOOLONG` |
| request target | 8 KiB | `SALTS_EMSGSIZE` |
| query entries | 64 | `SALTS_ENOBUFS` |
| application plus signing headers | 64 | `SALTS_ENOBUFS` |
| aggregate header bytes | 32 KiB | `SALTS_EMSGSIZE` |
| XML response/config document | 4 MiB | `SALTS_EMSGSIZE` |
| parsed XML nodes | 65,536 | `SALTS_EMSGSIZE` |
| returned list entries | 10,000 | `SALTS_ENOBUFS` |
| multipart parts | 10,000 | `SALTS_ENOBUFS` |
| high-level multipart part buffer | 64 MiB; configurable up to `INT_MAX` | `SALTS_EINVAL` |
| file/source chunk | CHTTP `stream_chunk_bytes` | CHTTP backpressure |

Checked addition/multiplication is required before every allocation and target
composition. Capacity failure is observable and never changes to an unbounded
allocation. Source/sink views are consumed synchronously and are invalid after
their callback returns.

## Multipart and recovery state

The multipart handle is an owning, generation-independent control-plane
object. It owns the upload id, bucket, key, and a fixed-capacity part array.
Each successful part number has exactly one ETag; replacement is explicit and
completion sorts by part number. Part numbers are 1 through 10,000. All
non-final parts are 5 MiB through 5 GiB. The accepted terminal states are
`COMPLETED`, `ABORTED`, and `DETACHED_FOR_RESUME`.

An SSE-C upload handle retains only the derived base64 request values required
for subsequent `UploadPart` calls and clears them on destroy. Every part sends
the same algorithm/key/MD5 headers. A resume checkpoint stores only the key MD5
fingerprint; the caller must resupply the same SSE-C options, and a mismatch is
rejected before network I/O. An empty, malformed, or `Error`-root
`CompleteMultipartUpload` HTTP 200 body is still a protocol/service failure and
leaves the handle active.

High-level file upload writes a versioned resume document through
same-directory temporary-file plus fsync plus atomic rename. Its identity
includes endpoint authority, bucket, key, file size, file modification time,
part size, SSE-C key fingerprint, and upload id. Mismatch fails before network I/O. A part ETag is
persisted only after the S3 response succeeds. Completion removes the resume
document only after the server confirms success. A terminal failure attempts
AbortMultipartUpload unless resumability was explicitly requested; abort
failure is returned with the upload id still available for caller recovery.
If the server has confirmed completion but checkpoint removal fails, the object
is already committed; the call returns `SALTS_EIO` at
`multipart-checkpoint-remove`, and the caller removes the stale checkpoint
instead of resuming it.

The current multipart handle and blocking client are both single-owner; the
high-level implementation is deterministic and sequential. A future parallel
adapter must use independent caller-supplied S3/CHTTP clients and serialize
updates to the multipart state fact source. It cannot alter the checkpoint or
ordered-ETag protocol.

## XML and management subresources

The private bounded XML facade parses service errors, list responses, upload
ids, copy responses, and management documents. It disables any need to expose
the private parser. Lifecycle, notification, and replication operations
validate the expected root element before sending caller XML and return the
bounded owned XML response on GET. This keeps the transport adapter complete
without importing legacy HTTP repository's partially validated mutable configuration
objects. Typed builders may be added later without changing the wire API.

## Compatibility, migration, and rollback

This adds a new target and headers; it does not change CHTTP, CNet, or CRPC ABI.
Applications migrating from legacy HTTP repository must replace CoroNet context ownership
with an injected CHTTP client and replace `tstr` return values with explicit
owned response/list destroy calls. H3 is intentionally unavailable and no
runtime compatibility fallback exists.

Rollback is a revert of the S3 module, root CMake/install-consumer additions,
and documentation changes. Because no existing target depends on S3, rollback
does not require a data or configuration migration.

## Verification boundary

Offline tests cover the published AWS SigV4 vectors, duplicate/sorted query
encoding, path and virtual-hosted addressing, XML limits, service errors,
H1/H2 wire parity, object CRUD, streaming file commit, multipart ordering,
resume mismatch, abort, C/C++ header inclusion, and installed consumption.
Release plus ASan run the focused S3/CHTTP suites; the final gate runs all
Release tests. Network tests using real credentials remain opt-in and are not
part of correctness evidence.
