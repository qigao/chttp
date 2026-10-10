#include "chttp_schema_example_application.h"
#include <http_client/http.h>
#include <fmt.h>
#include <string.h>

int main(void) {
  chttp_application app = {0};
  chttp_client client = {0};
  chttp_response response = {0};
  chttp_error error = {0};
  tstr uri = NULL;
  int failed = 1;
  chttp_application_options options = chttp_application_options_default();
  if (chttp_schema_example_application_init(&app, &options) != SALTS_OK ||
      chttp_application_start(&app) != SALTS_OK) goto done;
  uri = tstr_format("tcp://127.0.0.1:{}", chttp_application_port(&app));
  if (uri == NULL) goto done;
  chttp_client_config config = {.network = options.server.network,
      .request_capacity = 2u, .max_start_line_bytes = 256u,
      .max_header_count = 16u, .max_header_bytes = 4096u,
      .max_request_body_bytes = 4096u, .max_response_body_bytes = 4096u,
      .max_informational_responses = 2u};
  if (chttp_client_init(&client, &config) != SALTS_OK) goto done;
  const chttp_header header = {"Content-Type", "application/json"};
  const char body[] = "{\"left\":6,\"right\":7}";
  chttp_options request = {.connection_uri = uri, .authority = "127.0.0.1",
      .target = "/multiply", .headers = &header, .header_count = 1u,
      .body = body, .body_size = sizeof(body) - 1u, .timeout_ms = 5000u};
  if (chttp_post(&client, &request, &response, &error) != SALTS_OK ||
      response.status_code != 200u) goto done;
  const char *type = chttp_response_header(&response, "Content-Type");
  failed = type == NULL || strcmp(type, "application/xml") != 0 ||
      !vstr_contains(vstr_from_buf((const char *)response.body, response.body_size),
          vstr_from_cstr("<value>42</value>"));
done:
  chttp_response_destroy(&response);
  if (chttp_client_destroy(&client, 5000u) != SALTS_OK) failed = 1;
  if (chttp_application_close(&app, 5000u) != SALTS_OK) failed = 1;
  tstr_free(uri);
  return failed;
}
