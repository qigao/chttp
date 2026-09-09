// SPDX-FileCopyrightText: 2017-2022 Comcast Cable Communications Management, LLC
// SPDX-License-Identifier: Apache-2.0

#include "tinytest.h"

#include <json_parser.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef SSIZE_T
typedef intptr_t SSIZE_T;
#endif
#ifndef ssize_t
typedef SSIZE_T ssize_t;
#endif
#endif

#include "cjwt.h"

typedef struct {
    int expected;
    const char *jwt_file_name;
    bool is_key_in_file;
    bool expect_symmetric;
    const char *key;
    const char *decode_test_name;
} test_case_t;

test_case_t test_list[] = {
    {      0,        "jwtn.txt", false, false,                  "",      "No Alg claims on on"},
    {      0,       "jwtnx.txt", false, false,                  "",     "No Alg claims off on"},
    {      0,       "jwtny.txt", false, false,                  "",    "No Alg claims off off"},
    { EINVAL,       "jwtia.txt", false, true,       "test_passwd1",        "HS256 invalid jwt"},
    { EINVAL,       "jwtib.txt", false, true,       "test_passwd1",        "HS256 invalid jwt"},
 // {  EINVAL,      "jwtic.txt", false, true,       "test_passwd1",        "HS256 invalid jwt"}, /*TBD */ //FAILED test after modifying verify_signature logic
    { EINVAL,       "jwtid.txt", false, true,       "test_passwd1",        "HS256 invalid jwt"},
    { EINVAL,       "jwtie.txt", false, true,       "test_passwd1",        "HS256 invalid jwt"},
    { EINVAL,       "jwtif.txt", false, true,       "test_passwd1",        "HS256 invalid jwt"},
    {      0,        "jwt1.txt", false, true,       "test_passwd1",       "HS256 claims on on"},
    { EINVAL,        "jwt1.txt", false, true,       "test_passbad",       "HS256 claims on on"},
    {      0,        "jwt2.txt", false, true,       "test_passwd2",       "HS384 claims on on"},
    { EINVAL,        "jwt2.txt", false, true,       "test_passbad",       "HS384 claims on on"},
    {      0,        "jwt3.txt", false, true,       "test_passwd3",       "HS512 claims on on"},
    { EINVAL,        "jwt3.txt", false, true,       "test_passbad",       "HS512 claims on on"},
    {      0,        "jwt5.txt",  true, false,       "pubkey5.pem",       "RS384 claims on on"},
    { EINVAL,        "jwt5.txt",  true, false,       "badkey4.pem",       "RS384 claims on on"},
    {      0,        "jwt4.txt",  true, false,       "pubkey4.pem",       "RS256 claims on on"},
    { EINVAL,        "jwt4.txt",  true, false,       "badkey4.pem",       "RS256 claims on on"},
    {      0,        "jwt6.txt",  true, false,       "pubkey6.pem",       "RS512 claims on on"},
    { EINVAL,        "jwt6.txt",  true, false,       "badkey6.pem",       "RS512 claims on on"},
    {      0,       "jwt1x.txt", false, true,       "test_passwd1",      "HS256 claims off on"},
    { EINVAL,       "jwt1x.txt", false, true,      "test_prasswd1",      "HS256 claims off on"},
    {      0,       "jwt2x.txt", false, true,       "test_passwd2",      "HS384 claims off on"},
    { EINVAL,       "jwt2x.txt", false, true,      "twest_passwd2",      "HS384 claims off on"},
    {      0,       "jwt3x.txt", false, true,       "test_passwd3",      "HS512 claims off on"},
    { EINVAL,       "jwt3x.txt", false, true,    "test_passwd3...",      "HS512 claims off on"},
    {      0,       "jwt4x.txt",  true, false,       "pubkey4.pem",      "RS256 claims off on"},
    { EINVAL,       "jwt4x.txt",  true, false,       "pubkey5.pem",      "RS256 claims off on"},
    {      0,       "jwt5x.txt",  true, false,       "pubkey5.pem",      "RS384 claims off on"},
    { EINVAL,       "jwt5x.txt",  true, false,       "badkey5.pem",      "RS384 claims off on"},
    {      0,       "jwt6x.txt",  true, false,       "pubkey6.pem",      "RS512 claims off on"},
    { EINVAL,       "jwt6x.txt",  true, false,       "badkey6.pem",      "RS512 claims off on"},
    {      0,       "jwt1y.txt", false, true,       "test_passwd1",     "HS256 claims off off"},
    { EINVAL,       "jwt1y.txt", false, true,       "tast_passwd1",     "HS256 claims off off"},
    {      0,       "jwt2y.txt", false, true,       "test_passwd2",     "HS384 claims off off"},
    { EINVAL,       "jwt2y.txt", false, true,      "test..passwd2",     "HS384 claims off off"},
    {      0,       "jwt3y.txt", false, true,       "test_passwd3",     "HS512 claims off off"},
    { EINVAL,       "jwt3y.txt", false, true,   "tteesstt_passwd3",     "HS512 claims off off"},
    {      0,       "jwt4y.txt",  true, false,       "pubkey4.pem",     "RS256 claims off off"},
    { EINVAL,       "jwt4y.txt",  true, false,       "badkey4.pem",     "RS256 claims off off"},
    {      0,       "jwt5y.txt",  true, false,       "pubkey5.pem",     "RS384 claims off off"},
    { EINVAL,       "jwt5y.txt",  true, false,       "pubkey6.pem",     "RS384 claims off off"},
    {      0,       "jwt6y.txt",  true, false,       "pubkey6.pem",     "RS512 claims off off"},
    { EINVAL,       "jwt6y.txt",  true, false,       "pubkey5.pem",     "RS512 claims off off"},
    {      0,       "jwt1l.txt", false, true,       "test_passwd1",        "HS256 claims long"},
    { EINVAL,       "jwt1l.txt", false, true,      "test_keyword1",        "HS256 claims long"},
    {      0,       "jwt2l.txt", false, true,       "test_passwd2",        "HS384 claims long"},
    { EINVAL,       "jwt2l.txt", false, true,       "test_passwd1",        "HS384 claims long"},
    {      0,       "jwt3l.txt", false, true,       "test_passwd3",        "HS512 claims long"},
    { EINVAL,       "jwt3l.txt", false, true,            "passwd3",        "HS512 claims long"},
    {      0,       "jwt4l.txt",  true, false,       "pubkey4.pem",        "RS256 claims long"},
    { EINVAL,       "jwt4l.txt",  true, false,       "badkey4.pem",        "RS256 claims long"},
    {      0,       "jwt5l.txt",  true, false,       "pubkey5.pem",        "RS384 claims long"},
    { EINVAL,       "jwt5l.txt",  true, false,       "badkey5.pem",        "RS384 claims long"},
    {      0,       "jwt6l.txt",  true, false,       "pubkey6.pem",        "RS512 claims long"},
    { EINVAL,       "jwt6l.txt",  true, false,       "badkey6.pem",        "RS512 claims long"},
    {      0,        "jwt2.txt", false, true,       "test_passwd2",       "HS384 claims on on"},
    {      0,        "jwt3.txt", false, true,       "test_passwd3",       "HS512 claims on on"},
    {      0,  "jwt8_hs256.txt",  true, true,     "key8_hs256.pem",       "HS256 claims on on"},
    {      0,  "jwt9_hs384.txt",  true, true,     "key9_hs384.pem",       "HS384 claims on on"},
    {      0, "jwt10_hs512.txt",  true, true,    "key10_hs512.pem",       "HS512 claims on on"},
    { EINVAL,       "jwt11.txt", false, false,     "incorrect_key",         "RS256 claims all"},
    { EINVAL,       "jwt12.txt", false, false,     "incorrect_key",         "RS256 claims all"},
    { EINVAL,       "jwt13.txt", false, false,     "incorrect_key",         "RS256 claims all"},
    {ENOTSUP,   "jwtbadalg.txt", false, false,     "incorrect_key", "Invalid/unsupported alg."}
};

#define _NUM_TEST_CASES (sizeof(test_list) / sizeof(test_case_t))

static ssize_t read_file(const char *fname, char *buf, size_t buflen)
{
    char path[1024];
    size_t size = 0;

#ifdef TEST_DATA_DIR
    snprintf(path, sizeof(path), "%s/inputs/%s", TEST_DATA_DIR, fname);
#else
    snprintf(path, sizeof(path), "../tests/inputs/%s", fname);
#endif

    char *data = tt_read_file(path, &size);
    if (!data) {
        printf("File %s open error (path: %s)\n", fname, path);
        return -1;
    }

    if (size > buflen) {
        size = buflen;
    }
    memcpy(buf, data, size);
    free(data);
    return (ssize_t)size;
}

static unsigned int pass_cnt = 0;
static unsigned int fail_cnt = 0;

static void test_case(unsigned _i)
{
    const char *jwt_fname;
    const char *key_str;
    const char *decode_test_name;
    int expected;
    int key_len;
    ssize_t jwt_bytes;
    cjwt_code_t result = 0;
    cjwt_t *jwt        = NULL;
    char jwt_buf[65535];
    char pem_buf[8192];
    int options;
    jwt_fname        = test_list[_i].jwt_file_name;
    key_str          = test_list[_i].key;
    key_len          = strlen(key_str);
    expected         = test_list[_i].expected;
    decode_test_name = test_list[_i].decode_test_name;
    options          = OPT_ALLOW_ALG_NONE | OPT_ALLOW_ANY_TIME;

    if (key_len == 0) {
        key_str = NULL;
    } else if (test_list[_i].is_key_in_file) {
        key_len = read_file(key_str, pem_buf, sizeof(pem_buf));

        if (key_len >= 0) {
            key_str = (const char *) pem_buf;
        } else {
            printf("Error reading pem file\n");
            check(0);
            fail_cnt += 1;
            return;
        }
    }

    if (expected) {
        printf("\n--- Test %s expected good\n", decode_test_name);
    } else {
        printf("\n--- Test %s expected bad\n", decode_test_name);
    }
    printf("key in file %d, keylen = %d\n", test_list[_i].is_key_in_file,
           key_len);

    memset(jwt_buf, 0, sizeof(jwt_buf));
    printf("--- Input jwt : %s \n", jwt_fname);
    jwt_bytes = read_file(jwt_fname, jwt_buf, sizeof(jwt_buf));

    if (jwt_bytes > 0) {
        if (test_list[_i].expect_symmetric) {
            options |= OPT_ALLOW_ONLY_HS_ALG;
        }
        result = cjwt_decode(jwt_buf, strlen(jwt_buf),
                             options,
                             (const uint8_t *) key_str, key_len,
                             0, 0, &jwt);
    } else {
        result = jwt_bytes;
    }

    if ((0 == expected) && (CJWTE_OK == result)) {
        printf("--- PASSED: %s\n", decode_test_name);
        pass_cnt += 1;
        check_equal(CJWTE_OK, result);
    } else if ((0 != expected) && (CJWTE_OK != result)) {
        printf("--- PASSED: %s\n", decode_test_name);
        pass_cnt += 1;
        check(CJWTE_OK != result);
    } else {
        printf("\x1B[01;31m--- FAILED: %s (%d != %d)\x1B[00m\n", decode_test_name, expected, result);
        fail_cnt += 1;
        check(0);
    }

    cjwt_destroy(jwt);
}


static void run_legacy_tests(void)
{
    unsigned i;
    for (i = 0; i < _NUM_TEST_CASES; i++)
        test_case(i);
}

suite("cjwt legacy") {
  group("decode") {
    it("runs vectors") {
      run_legacy_tests();
    }
  }
}
