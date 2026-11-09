/* Internal to the web_server component. */
#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

/* Validates the Authorization header against the stored admin credentials.
 * On failure it sends 401 with a WWW-Authenticate challenge and returns false.
 * Call at the top of every route handler. */
bool web_auth_check(httpd_req_t *req);
