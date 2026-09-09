/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#include "tinytest.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/utils.h"

struct test_vector {
    const char *full;
    size_t len;
    int rv;
    struct split_jwt goal;
};

suite("cjwt utils") {
  group("base64url") {
    it("round trips URL-safe data without padding") {
      const uint8_t input[] = {0xfb, 0xff, 0xef};
      size_t encoded_len = 0;
      size_t decoded_len = 0;

      char *encoded = b64url_encode_with_alloc(input, sizeof(input), &encoded_len);
      check_not_null(encoded);
      check_equal(encoded, "-__v");
      check_equal(encoded_len, 4);

      uint8_t *decoded =
          b64url_decode_with_alloc((const uint8_t *)encoded, encoded_len, &decoded_len);
      check_not_null(decoded);
      check_equal(decoded_len, sizeof(input));
      check_equal(decoded, input, sizeof(input));

      free(decoded);
      free(encoded);
    }

    it("rejects invalid input") {
      size_t decoded_len = 0;
      check_null(b64url_decode_with_alloc((const uint8_t *)"invalid*", 8,
                                          &decoded_len));
    }
  }

  group("split") {
    it("handles all vectors") {
    // clang-format off
    struct test_vector tests[] = {
        {   .full = "abcdefghijkl",
            .len  = 12,
            .rv   = -1,
        },
        {   .full = "a.b.c.d.e.fghijkl",
            .len  = 17,
            .rv   = -1,
        },
        {   .full = "abcd.efg",
            .len  = 8,
            .rv   = 0,
            .goal = {
                .count = 2,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = NULL,   .len = 0 },
                    { .data = NULL,   .len = 0 },
                    { .data = NULL,   .len = 0 },
                },
            },
        },
        {   .full = "abcd.efg.hij",
            .len  = 12,
            .rv   = 0,
            .goal = {
                .count = 3,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "hij",  .len = 3 },
                    { .data = NULL,   .len = 0 },
                    { .data = NULL,   .len = 0 },
                },
            },
        },
        {   .full = "abcd.efg.hij.klm",
            .len  = 16,
            .rv   = 0,
            .goal = {
                .count = 4,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "hij",  .len = 3 },
                    { .data = "klm",  .len = 3 },
                    { .data = NULL,   .len = 0 },
                },
            },
        },
        {   .full = "abcd.efg.hij.klm.op",
            .len  = 19,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "hij",  .len = 3 },
                    { .data = "klm",  .len = 3 },
                    { .data = "op",   .len = 2 },
                },
            },
        },
        {   .full = "abcd.efg..klm.op",
            .len  = 16,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "",     .len = 0 },
                    { .data = "klm",  .len = 3 },
                    { .data = "op",   .len = 2 },
                },
            },
        },
        {   .full = "....",
            .len  = 4,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                },
            },
        },
        {   .full = "d....g",
            .len  = 6,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "d",    .len = 1 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "g",    .len = 1 },
                },
            },
        },
        {   .full = "dog.",
            .len  = 4,
            .rv   = 0,
            .goal = {
                .count = 2,
                .sections = {
                    { .data = "dog",  .len = 3 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                },
            },
        },
    };
    // clang-format on

    for (size_t i = 0; i < sizeof(tests) / sizeof(struct test_vector); i++) {
        struct split_jwt got;
        int rv;

        rv = split(tests[i].full, tests[i].len, &got);

        int expected_rv = tests[i].rv;
        if (expected_rv == 0) {
            expected_rv = (int)tests[i].goal.count;
        }
        check_equal(expected_rv, rv);
        if (rv > 0) {
            check_equal(got.count, tests[i].goal.count);

            for (size_t j = 0; j < got.count; j++) {
                check_equal(got.sections[j].len, tests[i].goal.sections[j].len);

                if (tests[i].goal.sections[j].len > 0) {
                    check(0 == memcmp(tests[i].goal.sections[j].data,
                                      got.sections[j].data,
                                      tests[i].goal.sections[j].len));
                }
            }
        }
    }
    }
  }
}
