#include "chttp_schema_example.service_native.h"

int databind_7_WebDemo_10_Calculator_3_Add(const Numbers_t *request, Answer_t *response) {
  response->value = (uint64_t)request->left + request->right;
  return 0;
}

int databind_7_WebDemo_10_Calculator_8_Multiply(const Numbers_t *request, Answer_t *response) {
  response->value = (uint64_t)request->left * request->right;
  return 0;
}

int databind_7_WebDemo_6_System_5_Check(const Probe_t *request, Health_t *response) {
  (void)request;
  response->ready = true;
  return 0;
}
