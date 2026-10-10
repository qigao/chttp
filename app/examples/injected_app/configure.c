#include <chttp_app/application.h>
#include "factor_source.h"

int configure_injected_app(chttp_application_options *options) {
  static const FactorSettings settings = {7u};
  static salts_component_deployment providers[1];
  providers[0] = (salts_component_deployment){factor_source_provider(), cmeta_reflected_data(FactorSettings), &settings};
  options->providers = providers;
  options->provider_count = 1u;
  return SALTS_OK;
}
