#include "chttp_worker_body.service_native.h"
#include "chttp_worker_body_native.h"
#include "chttp_service_plugin_probe.h"

int databind_11_BodyIngress_5_Probe_4_Echo(const Request_t *request, Response_t *response) {
  if (!request || !response || !chttp_service_plugin_probe_enter()) return -1;
  response->payload.id = request->payload.id;
  response->payload.child.number = request->payload.child.number;
  tstr text = tstr_dup_len(request->payload.note, tstr_len(request->payload.note));
  if (!text) return -1;
  tstr_free(response->payload.note);
  response->payload.note = text;
  return 0;
}

int databind_11_BodyIngress_8_RawProbe_4_Size(const BinaryRequest_t *request, Length_t *response) {
  if (!request || !response) return -1;
  response->value = stl_byte_buffer_size(&request->payload.data);
  return 0;
}
