# HTTPS compatibility fixtures

These fixed certificate and test-key inputs are copied byte-for-byte from the
former Salts CNet test suite at commit
`2a943b532c56b959a8f395288c23a77c003009db`, path `cnet/tests/fixtures/`.
Source: https://github.com/qigao/cmeta/tree/2a943b532c56b959a8f395288c23a77c003009db/cnet/tests/fixtures

They now belong to the CHttp HTTPS product-path tests. There is no upstream
runtime/source build or external fixture directory in the test configuration.
The material is test data, including intentionally invalid chains and
unsupported identities. Keep the expected outcomes in `chttp_tls_test.c`.
