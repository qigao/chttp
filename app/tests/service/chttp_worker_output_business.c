#include "chttp_worker_output.service_native.h"
#include "chttp_worker_output_native.h"
#include "chttp_service_plugin_probe.h"
#include <string.h>

static int output_text(tstr *slot, uint32_t value) {
  static const char escaped[] = {'a', '"', '\\', '\n', '\0', 'z'};
  char long_text[256];
  memset(long_text, 'x', sizeof(long_text));
  tstr text = value == 0u ? tstr_new() : value == 2u ? tstr_dup_len(long_text, sizeof(long_text)) :
      tstr_dup_len(escaped, sizeof(escaped));
  if (!text) return -1;
  tstr_free(*slot); *slot = text;
  return 0;
}
int databind_6_Output_5_Probe_3_Get(const Query_t *request, Envelope_t *response) {
  if (!request || !response || !chttp_service_plugin_probe_enter()) return -1;
  response->payload.id = UINT64_MAX;
  response->payload.child.number = request->value;
  return output_text(&response->payload.note, request->value);
}
int databind_6_Output_5_Probe_4_Fail(const Query_t *request, Envelope_t *response,
    databind_6_Output_5_Probe_4_Fail__error *error) {
  if (!request || !response || !error || !chttp_service_plugin_probe_enter()) return -1;
  if (databind_6_Output_5_Probe_4_Fail__error_select(error,
          databind_6_Output_5_Probe_4_Fail__ERROR_1) != DATA_BIND_OK) return -1;
  error->payload.error_1.code = 77u;
  return output_text(&error->payload.error_1.reason, request->value);
}
