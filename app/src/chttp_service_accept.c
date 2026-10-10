#include "chttp_service_accept.h"
#include <vstr.h>
#include <string.h>

typedef struct accept_score { int specificity; unsigned quality; } accept_score;

static int token_char(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || (c != 0u && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static void ows(const char **cursor) {
  while (**cursor == ' ' || **cursor == '\t') ++*cursor;
}

static vstr token(const char **cursor) {
  const char *start = *cursor;
  while (token_char((unsigned char)**cursor)) ++*cursor;
  return vstr_from_buf(start, (size_t)(*cursor - start));
}

static int quality(vstr value, unsigned *out) {
  if (value.len == 0u || (value.data[0] != '0' && value.data[0] != '1')) return 0;
  unsigned result = value.data[0] == '1' ? 1000u : 0u;
  if (value.len > 1u) {
    if (value.data[1] != '.' || value.len > 5u) return 0;
    unsigned place = 100u;
    for (size_t i = 2u; i < value.len; ++i, place /= 10u) {
      if (value.data[i] < '0' || value.data[i] > '9' ||
          (value.data[0] == '1' && value.data[i] != '0')) return 0;
      result += (unsigned)(value.data[i] - '0') * place;
    }
  }
  *out = result;
  return 1;
}

/* Validate and skip quoted parameter values without copying or treating their
 * embedded commas as list separators. These offers have no media parameters. */
static int quoted(const char **cursor) {
  ++*cursor;
  for (;;) {
    unsigned char c = (unsigned char)**cursor;
    if (c == 0u || (c < 32u && c != '\t') || c == 127u) return 0;
    ++*cursor;
    if (c == '"') return 1;
    if (c == '\\') {
      c = (unsigned char)**cursor;
      if (c == 0u || (c < 32u && c != '\t') || c == 127u) return 0;
      ++*cursor;
    }
  }
}

static int accept_line(const char *cursor, accept_score scores[2]) {
  if (cursor == NULL) return 0;
  for (;;) {
    ows(&cursor);
    if (*cursor == '\0') return 1;
    if (*cursor == ',') { ++cursor; continue; }
    vstr type = token(&cursor);
    if (type.len == 0u || *cursor++ != '/') return 0;
    vstr subtype = token(&cursor);
    if (subtype.len == 0u) return 0;
    int any_type = vstr_eq(type, vstr_from_cstr("*"));
    int any_subtype = vstr_eq(subtype, vstr_from_cstr("*"));
    if (any_type && !any_subtype) return 0;
    int specific = any_type ? 0 : any_subtype ? 1 : 2;
    int parameters = 0, has_quality = 0;
    unsigned q = 1000u;
    ows(&cursor);
    while (*cursor == ';') {
      ++cursor;
      ows(&cursor);
      /* HTTP parameters permit empty semicolon entries. */
      if (*cursor == ';' || *cursor == ',' || *cursor == '\0') continue;
      vstr name = token(&cursor);
      ows(&cursor);
      if (name.len == 0u || *cursor++ != '=') return 0;
      ows(&cursor);
      int is_quality = vstr_ieq(name, vstr_from_cstr("q"));
      if (*cursor == '"') {
        if (is_quality || !quoted(&cursor)) return 0;
      } else {
        vstr value = token(&cursor);
        if (value.len == 0u || (is_quality && !quality(value, &q))) return 0;
      }
      if (is_quality) {
        if (has_quality) return 0;
        has_quality = 1;
      } else parameters = 1;
      ows(&cursor);
    }
    if (*cursor != '\0' && *cursor != ',') return 0;
    if (!parameters && (any_type || vstr_ieq(type, vstr_from_cstr("application")))) {
      for (size_t i = 0u; i < 2u; ++i) {
        if ((any_subtype || vstr_ieq(subtype, vstr_from_cstr(i == 0u ? "json" : "xml"))) &&
            specific > scores[i].specificity)
          scores[i] = (accept_score){specific, q};
      }
    }
  }
}

unsigned chttp_service_accept(const chttp_header *headers, size_t count,
    int prefer_xml, int *selected_xml) {
  accept_score scores[2] = {{-1, 0u}, {-1, 0u}};
  int present = 0;
  for (size_t i = 0u; i < count; ++i) {
    if (!vstr_ieq(vstr_from_cstr(headers[i].name), vstr_from_cstr("Accept"))) continue;
    present = 1;
    if (!accept_line(headers[i].value, scores)) return 400u;
  }
  if (present && scores[0].quality == 0u && scores[1].quality == 0u) return 406u;
  *selected_xml = !present || scores[0].quality == scores[1].quality
      ? prefer_xml : scores[1].quality > scores[0].quality;
  return 0u;
}
