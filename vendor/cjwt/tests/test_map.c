/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#include "tinytest.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjwt.h"

suite("cjwt map") {
  group("alg_string_to_enum") {
    it("maps known values and rejects unknown") {
    struct vector {
        const char *s;
        size_t len;
        cjwt_alg_t expected;
        cjwt_code_t rv;
    } tests[] = {
        {   "none", SIZE_MAX,  alg_none,                 CJWTE_OK},
        {  "ES256", SIZE_MAX, alg_es256,                 CJWTE_OK},
        {  "ES384", SIZE_MAX, alg_es384,                 CJWTE_OK},
        {  "ES512", SIZE_MAX, alg_es512,                 CJWTE_OK},
        {  "HS256", SIZE_MAX, alg_hs256,                 CJWTE_OK},
        {  "HS384", SIZE_MAX, alg_hs384,                 CJWTE_OK},
        {  "HS512", SIZE_MAX, alg_hs512,                 CJWTE_OK},
        {  "PS256", SIZE_MAX, alg_ps256,                 CJWTE_OK},
        {  "PS384", SIZE_MAX, alg_ps384,                 CJWTE_OK},
        {  "PS512", SIZE_MAX, alg_ps512,                 CJWTE_OK},
        {  "RS256", SIZE_MAX, alg_rs256,                 CJWTE_OK},
        {  "RS384", SIZE_MAX, alg_rs384,                 CJWTE_OK},
        {  "RS512", SIZE_MAX, alg_rs512,                 CJWTE_OK},
        {  "none_",        4,  alg_none,                 CJWTE_OK},
        { "ES256_",        5, alg_es256,                 CJWTE_OK},
        { "ES384_",        5, alg_es384,                 CJWTE_OK},
        { "ES512_",        5, alg_es512,                 CJWTE_OK},
        { "HS256_",        5, alg_hs256,                 CJWTE_OK},
        { "HS384_",        5, alg_hs384,                 CJWTE_OK},
        { "HS512_",        5, alg_hs512,                 CJWTE_OK},
        { "PS256_",        5, alg_ps256,                 CJWTE_OK},
        { "PS384_",        5, alg_ps384,                 CJWTE_OK},
        { "PS512_",        5, alg_ps512,                 CJWTE_OK},
        { "RS256_",        5, alg_rs256,                 CJWTE_OK},
        { "RS384_",        5, alg_rs384,                 CJWTE_OK},
        { "RS512_",        5, alg_rs512,                 CJWTE_OK},
        { "RS512_",        0,  alg_none, CJWTE_INVALID_PARAMETERS},
        {     NULL,        0,  alg_none, CJWTE_INVALID_PARAMETERS},
        {     NULL,        5,  alg_none, CJWTE_INVALID_PARAMETERS},
        {   "none",        3,  alg_none,        CJWTE_UNKNOWN_ALG},
        {"toolong",        7,  alg_none,        CJWTE_UNKNOWN_ALG},
        {   "tree",        4,  alg_none,        CJWTE_UNKNOWN_ALG},
        {  "trees",        5,  alg_none,        CJWTE_UNKNOWN_ALG},
    };
    cjwt_alg_t alg;

    for (size_t i = 0; i < sizeof(tests) / sizeof(struct vector); i++) {
        if (alg_none == tests[i].expected) {
            alg = alg_es256;
        } else {
            alg = alg_none;
        }
        check_equal(tests[i].rv, cjwt_alg_string_to_enum(tests[i].s, tests[i].len, &alg));
        if (CJWTE_OK == tests[i].rv) {
            check_equal(tests[i].expected, alg);
        }
    }

    /* If a NULL alg is passed, check for that. */
    check_equal(CJWTE_INVALID_PARAMETERS, cjwt_alg_string_to_enum("none", 4, NULL));
    }
  }
}
