#ifndef CHTTP_ATTEMPT_H
#define CHTTP_ATTEMPT_H
#include <stdbool.h>

/* Owner-local evidence, copied during the attempt's terminal callback before
 * its request slot is recycled. Diagnostics never authorize application replay. */
typedef struct chttp_attempt_facts {
  bool retryable_failure;
  bool protocol_not_executed;
  bool response_started;
  bool security_failure;
} chttp_attempt_facts;
#endif
