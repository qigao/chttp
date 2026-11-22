file(READ "${CERTIFICATE}" leaf)
file(READ "${ISSUER_CERTIFICATE}" issuer)
file(WRITE "${CERTIFICATE_CHAIN}" "${leaf}${issuer}")
