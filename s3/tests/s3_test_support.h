#ifndef S3_TEST_SUPPORT_H
#define S3_TEST_SUPPORT_H

#include <chttp/chttp.h>

#include <stddef.h>
#include <stdint.h>

enum { S3_TEST_TIMEOUT_MS = 5000 };

native_io_backend_kind s3_test_backend(void);
cnet_client_config s3_test_network_config(size_t connection_capacity);
chttp_server_config s3_test_server_config(void);
chttp_client_config s3_test_client_config(void);
int s3_test_endpoint(uint16_t port, char *connection_uri, size_t uri_capacity, char *authority,
                     size_t authority_capacity);

#endif /* S3_TEST_SUPPORT_H */
