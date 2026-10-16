#include "staged_scenario.h"
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(SALTS_SERVICE_HOST_MAX_ATTACHED_GENERATIONS == 2u,
               "two-generation bound must remain public");
#else
#error "an exact C11 compiler is required"
#endif
int main(int argc, char **argv) { return staged_probe_run(argc, argv); }
