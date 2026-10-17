#include "chttp_service_host_strict_fr_abi_cases.h"
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error C11 required for strict borrowed provider ABI test
#endif
int main(void) { return verify_chttp_service_host_strict_fr1(); }
