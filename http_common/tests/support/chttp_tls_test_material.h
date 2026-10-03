#ifndef CHTTP_TLS_TEST_MATERIAL_H
#define CHTTP_TLS_TEST_MATERIAL_H

/*
 * GmSSL validates trust anchors as CAs and its TLS 1.3 client-certificate
 * selection requires an EC issuer when matching signature_algorithms_cert.
 * Keep this fixture as a real P-256 chain: localhost leaf first, then root CA.
 * The leaf is valid for both serverAuth and clientAuth because mTLS tests reuse it.
 */
static const char CHTTP_TLS_TEST_CERTIFICATE[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB0TCCAXagAwIBAgIUbrFOcwBbzF/EBOIrqLAADQev87IwCgYIKoZIzj0EAwIw\n"
    "HTEbMBkGA1UEAwwSQ0hUVFAgVGVzdCBSb290IENBMB4XDTI2MTAwMzIwNDg0MVoX\n"
    "DTM2MDkzMDIwNDg0MVowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0C\n"
    "AQYIKoZIzj0DAQcDQgAEhihR5dg9gQaYCAI1vRbMU4zGIRa+a9x1xYYGUrnH/jR6\n"
    "jczoxKwMO3lJV6nwWjTNOUlajzjUt6SNxeFqPJUCqaOBnDCBmTAMBgNVHRMBAf8E\n"
    "AjAAMA4GA1UdDwEB/wQEAwIHgDAdBgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUH\n"
    "AwIwGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMB0GA1UdDgQWBBRjaI1dPRQL\n"
    "VRjJxwFzutP0mjll8jAfBgNVHSMEGDAWgBRiBirofviDvO9rY9RNU+aMOSizVDAK\n"
    "BggqhkjOPQQDAgNJADBGAiEA1/a2JOM5HpaRDD8xAnZ84csKvSHn9SwB6SF+fg77\n"
    "0IkCIQDJoniouPxN7h8PL0vcDdppMwFGv3kn0wKNnYVtBJe/4Q==\n"
    "-----END CERTIFICATE-----\n";

static const char CHTTP_TLS_TEST_CA_CERTIFICATE[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBozCCAUigAwIBAgIUQTZIoHft+GDMregpujVjIpMKpuowCgYIKoZIzj0EAwIw\n"
    "HTEbMBkGA1UEAwwSQ0hUVFAgVGVzdCBSb290IENBMB4XDTI2MTAwMzIwNDg0MVoX\n"
    "DTM2MDkzMDIwNDg0MVowHTEbMBkGA1UEAwwSQ0hUVFAgVGVzdCBSb290IENBMFkw\n"
    "EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEANF/KhU1SA5czzk8K/SnWvyXuk/snIWC\n"
    "TQHuxX7gJ/4KjuQT3xVezErVYnuOLeRYCzmw6KKveT1VZN267yGZiqNmMGQwEgYD\n"
    "VR0TAQH/BAgwBgEB/wIBATAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFGIGKuh+\n"
    "+IO872tj1E1T5ow5KLNUMB8GA1UdIwQYMBaAFGIGKuh++IO872tj1E1T5ow5KLNU\n"
    "MAoGCCqGSM49BAMCA0kAMEYCIQCQqYWEVxX1P94Yaer6JBGHM5kN1gmWrtjR7mVx\n"
    "i5osBgIhALgHAsFHBrVPYfYIjNnrJfVfYCs/oNAkxl4UlPpa2Wa3\n"
    "-----END CERTIFICATE-----\n";

static const char CHTTP_TLS_TEST_KEY[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgzJT68YEt6tUw0WN1\n"
    "hMWrubaihY3bQKfXqVkmCXR6yNOhRANCAASGKFHl2D2BBpgIAjW9FsxTjMYhFr5r\n"
    "3HXFhgZSucf+NHqNzOjErAw7eUlXqfBaNM05SVqPONS3pI3F4Wo8lQKp\n"
    "-----END PRIVATE KEY-----\n";

#endif /* CHTTP_TLS_TEST_MATERIAL_H */
