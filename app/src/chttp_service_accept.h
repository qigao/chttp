#ifndef CHTTP_SERVICE_ACCEPT_H
#define CHTTP_SERVICE_ACCEPT_H

#include <http_common/http.h>

/* Private, allocation-free selection over buffered HTTP headers. Returns HTTP
 * 400/406 on rejection, zero on success. selected_xml is written only on success.
 * Both offers are parameterless application/json and application/xml. */
unsigned chttp_service_accept(const chttp_header *headers, size_t count,
    int prefer_xml, int *selected_xml);

#endif
