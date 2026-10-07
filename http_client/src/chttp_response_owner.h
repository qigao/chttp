#ifndef CHTTP_RESPONSE_OWNER_H
#define CHTTP_RESPONSE_OWNER_H

#include <http_client/http.h>

/* Copy a callback-borrowed view into a zero response. Failure leaves it zero.
 * Input counts are bounded by the active client/parser configuration. */
int chttp_response_copy(const chttp_response_view *source, chttp_response *out);

#endif
