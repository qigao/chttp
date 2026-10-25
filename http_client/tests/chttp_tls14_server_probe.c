/* CHttp::Server real independent OpenSSL TLS1.3/mTLS probe.
 * A source-built Salts::CNet + GmSSL 3.2.0#14 is required.
 * No plaintext fallback, no TLS verifier bypass and no application
 * authority from forwarded HTTP headers. Test-only loopback daemon.
 */
#include <http_server/http.h>
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_TLS14_HANDSHAKE_MS = 7000u };
static const char kRoute[] = "/interop";
static const char kResponse[] = "{\"mTLS\":true,\"version\":\"TLSv1.3\"}";

static native_io_backend_kind network_kind(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int serve(void *user, const chttp_server_request_view *request,
                 chttp_server_response *response) {
  const char *expected = (const char *)user;
  if (!expected || !request || !response) return SALTS_EINVAL;
  if (request->peer_certificate_sha256 == NULL)
    return chttp_server_reply(response, 401u, "text/plain", "unauthenticated", 15u);
  /* The digest comes exclusively from verified CNet TLS, NOT HTTP headers. */
  if (strcmp(request->peer_certificate_sha256, expected) != 0)
    return chttp_server_reply(response, 403u, "text/plain", "forbidden", 9u);
  return chttp_server_reply(response, 200u, "application/json",
                            kResponse, sizeof(kResponse) - 1u);
}

int main(int argc, char **argv) {
  static const char *const alpn[] = {"http/1.1"};
  chttp_server server = {0};
  chttp_server_config c = {0};
  cnet_tls_server_config tls = {0};
  int ready = 0, started = 0, result = EXIT_FAILURE;
  uint16_t port = 0u;

  if (argc != 5 || strlen(argv[4]) != 64u) {
    fputs("usage: tls14-http-server ca.pem cert.pem key.pem allowed-cert-sha256\n", stderr);
    return EXIT_FAILURE;
  }
  c.host = "127.0.0.1";
  c.port = 0u;
  c.backlog = 8u;
  c.network.backend = network_kind();
  c.network.connection_capacity = 8u;
  c.network.command_capacity = 32u;
  c.network.request_capacity = 16u;
  c.network.completion_batch_capacity = 8u;
  c.network.event_capacity = 32u;
  c.network.max_send_bytes = 16384u;
  c.network.receive_buffer_bytes = 4096u;
  c.network.connect_timeout_ms = CHTTP_TLS14_HANDSHAKE_MS;
  c.network.read_timeout_ms = CHTTP_TLS14_HANDSHAKE_MS;
  c.network.write_timeout_ms = CHTTP_TLS14_HANDSHAKE_MS;
  c.network.tls_io_buffer_bytes = 32768u;
  c.network.tls_handshake_timeout_ms = CHTTP_TLS14_HANDSHAKE_MS;
  c.route_capacity = 1u;
  c.middleware_capacity = 1u;
  c.max_route_middleware_count = 1u;
  c.max_route_param_count = 1u;
  c.max_route_param_bytes = 24u;
  c.max_target_bytes = 128u;
  c.max_header_count = 16u;
  c.max_header_bytes = 2048u;
  c.max_request_body_bytes = 0u;
  c.max_response_header_count = 8u;
  c.max_response_header_bytes = 512u;
  c.max_response_body_bytes = 512u;
  c.max_buffered_response_body_bytes = 512u;
  c.buffer_capacity_bytes = 512u * 1024u;
  c.poll_slice_ms = 2u;

  tls.size = sizeof(tls);
  tls.cert_file = argv[2];
  tls.key_file = argv[3];
  tls.ca_file = argv[1];
  tls.client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED;
  tls.alpn_protocols = alpn;
  tls.alpn_protocol_count = 1u;
  c.tls = &tls;

  if (chttp_server_init(&server, &c) != SALTS_OK)
    goto end;
  ready = 1;
  if (chttp_server_get(&server, kRoute, serve, argv[4]) != SALTS_OK ||
      chttp_server_start(&server) != SALTS_OK)
    goto end;
  started = 1;
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u)
    goto end;
  printf("CHTTP_TLS14_PORT=%u\n", (unsigned int)port);
  fflush(stdout);
  (void)getchar(); /* test harness owns explicit shutdown */

  result = EXIT_SUCCESS;
end:
  if (started && chttp_server_stop(&server, CHTTP_TLS14_HANDSHAKE_MS) != SALTS_OK)
    result = EXIT_FAILURE;
  if (ready && chttp_server_destroy(&server) != SALTS_OK)
    result = EXIT_FAILURE;
  return result;
}
