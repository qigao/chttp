#include <type_traits>
#include "staged_scenario.h"
static_assert(std::is_standard_layout<salts_service_host_scope>::value,
              "scope must be an explicitly C-compatible public record");
static_assert(std::is_standard_layout<salts_configurator_context>::value,
              "configurator must remain C-compatible");
int main(int argc, char **argv) { return staged_probe_run(argc, argv); }
