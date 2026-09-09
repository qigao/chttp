#ifndef CHTTP_JWT_INTERNAL_H
#define CHTTP_JWT_INTERNAL_H

#include <stdint.h>

typedef struct chttp_server_request_state chttp_server_request_state;
typedef struct chttp_jwt_bearer_validator chttp_jwt_bearer_validator;
typedef struct chttp_server_request_view chttp_server_request_view;

void chttp_jwt_request_state_reset(chttp_server_request_state *state);
int chttp_jwt_bearer_request_validate(chttp_server_request_state *state,
                                      const chttp_server_request_view *request,
                                      chttp_jwt_bearer_validator *validator);
int chttp_jwt_bearer_request_validate_at(chttp_server_request_state *state,
                                         const chttp_server_request_view *request,
                                         chttp_jwt_bearer_validator *validator,
                                         int64_t now_seconds);
int chttp_jwt_bearer_unauthorized_response(chttp_server_response *response);

#endif
