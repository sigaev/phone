#pragma once

#include "chromecast/mbedtls_config.h"

// Tests use the same client algorithms, plus a simulated Chromecast server
// with generated RSA setup keys and a self-signed TLS certificate.
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
