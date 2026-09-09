#include "chttp_server_runtime.h"

#include <datetime_parser.h>
#include <platform.h>
#include <salts/clock.h>
#include <salts_fs.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { FILE_ETAG_CAPACITY = 80, FILE_RANGE_CAPACITY = 96, HTTP_DATE_INPUT_CAPACITY = 64,
       MICROSECONDS_PER_SECOND = 1000000,
       MILLISECONDS_PER_SECOND = 1000, YEAR_BASE = 1900, CENTURY = 100, OBS_YEAR_WINDOW = 50 };

static const char *file_skip_space(const char *value) {
  while (*value == ' ' || *value == '\t') ++value;
  return value;
}

static const char *file_tag_end(const char *tag) {
  if (strncmp(tag, "W/", 2) == 0) tag += 2;
  if (*tag++ != '"') return NULL;
  while (*tag != '"') {
    const unsigned char ch = (unsigned char)*tag;
    if (ch < 0x21 || ch == 0x7f) return NULL;
    ++tag;
  }
  return tag + 1;
}

/* O(header bytes), no allocation; quoted commas belong to the tag, not the list. */
static int file_tag_matches(const char *field, const char *etag, bool strong) {
  bool matched = false;
  field = file_skip_space(field);
  if (*field == '*') return *file_skip_space(field + 1) == '\0' ? 1 : -1;
  while (*field != '\0') {
    if (*field == ',') { field = file_skip_space(field + 1); continue; }
    const char *end = file_tag_end(field);
    if (end == NULL) return -1;
    const bool weak = strncmp(field, "W/", 2) == 0;
    const bool current_weak = strncmp(etag, "W/", 2) == 0;
    const char *candidate = field + (weak ? 2 : 0);
    const char *current = etag + (current_weak ? 2 : 0);
    if ((!strong || (!weak && !current_weak)) &&
        (size_t)(end - candidate) == strlen(current) &&
        memcmp(candidate, current, strlen(current)) == 0) matched = true;
    field = file_skip_space(end);
    if (*field == '\0') break;
    if (*field++ != ',') return -1;
    field = file_skip_space(field);
  }
  return matched ? 1 : 0;
}

/* Repeated list fields are one condition; a match on any field is sufficient. */
static int file_request_tags(const chttp_server_request_view *request, const char *name,
                             const char *etag, bool strong) {
  int result = -2;
  size_t count = 0;
  bool wildcard = false;
  for (size_t index = 0; index < request->header_count; ++index) {
    const chttp_server_request_view field_view = {.headers = request->headers + index, .header_count = 1};
    const char *value = chttp_server_request_header(&field_view, name);
    if (value == NULL) continue;
    ++count;
    wildcard = wildcard || *file_skip_space(value) == '*';
    const int match = file_tag_matches(value, etag, strong);
    if (match < 0 || (count > 1 && wildcard)) return -1;
    if (result < 0) result = match;
    else result = result || match;
  }
  return result;
}

static bool file_http_date(const char *value, time_t *out) {
  char normalized[CHTTP_SERVER_DATE_CAPACITY];
  char trimmed[HTTP_DATE_INPUT_CAPACITY];
  char weekday[16], month[4], zone[4];
  int day, year, hour, minute, second, consumed = 0;
  datetime_t date;
  struct tm utc;
  value = file_skip_space(value);
  size_t length = strlen(value);
  while (length != 0 && (value[length - 1] == ' ' || value[length - 1] == '\t')) --length;
  if (length >= sizeof(trimmed)) return false;
  memcpy(trimmed, value, length);
  trimmed[length] = '\0';
  value = trimmed;
  if (length != CHTTP_SERVER_DATE_BYTES || value[3] != ',' || strcmp(value + 26, "GMT") != 0) {
    /* Normalize the two obsolete HTTP-date wire formats for the existing parser. */
    if (sscanf(value, "%15[^,], %2d-%3s-%2d %2d:%2d:%2d %3s%n", weekday, &day, month,
               &year, &hour, &minute, &second, zone, &consumed) == 8 &&
        (size_t)consumed == length && strcmp(zone, "GMT") == 0) {
      if (salts_gmtime((time_t)(salts_realtime_ms() / MILLISECONDS_PER_SECOND), &utc) != 0)
        return false;
      const int current_year = utc.tm_year + YEAR_BASE;
      year += (current_year / CENTURY) * CENTURY;
      if (year > current_year + OBS_YEAR_WINDOW) year -= CENTURY;
    } else {
      consumed = 0;
      if (sscanf(value, "%3s %3s %2d %2d:%2d:%2d %4d%n", weekday, month, &day,
                 &hour, &minute, &second, &year, &consumed) != 7 ||
          (size_t)consumed != length) return false;
    }
    if (snprintf(normalized, sizeof(normalized), "%.3s, %02d %s %04d %02d:%02d:%02d GMT",
                 weekday, day, month, year, hour, minute, second) != CHTTP_SERVER_DATE_BYTES)
      return false;
    value = normalized;
  }
  if (datetime_parse(value, CHTTP_SERVER_DATE_BYTES, &date) != 0 ||
      date.year < YEAR_BASE || date.month < 1 || date.month > 12 || date.day < 1 ||
      date.day > 31 || date.hour < 0 || date.hour > 23 || date.minute < 0 ||
      date.minute > 59 || date.second < 0 || date.second > 59) return false;
  *out = datetime_to_time(&date);
  if (*out == (time_t)-1 || salts_gmtime(*out, &utc) != 0) return false;
  return utc.tm_year + YEAR_BASE == date.year && utc.tm_mon + 1 == date.month &&
         utc.tm_mday == date.day;
}

static const char *file_single_header(const chttp_server_request_view *request, const char *name) {
  const char *result = NULL;
  for (size_t index = 0; index < request->header_count; ++index) {
    const chttp_server_request_view field_view = {.headers = request->headers + index, .header_count = 1};
    const char *value = chttp_server_request_header(&field_view, name);
    if (value == NULL) continue;
    if (result != NULL) return NULL;
    result = value;
  }
  return result;
}

static bool file_if_range_matches(const char *value, const char *etag) {
  value = file_skip_space(value);
  if (*value != '"' || *etag != '"') return false;
  const char *end = file_tag_end(value);
  return end != NULL && *file_skip_space(end) == '\0' &&
         (size_t)(end - value) == strlen(etag) && memcmp(value, etag, strlen(etag)) == 0;
}

static bool file_decimal(const char **cursor, uint64_t *out) {
  enum { DECIMAL_BASE = 10 };
  const char *start = *cursor;
  uint64_t number = 0;
  while (**cursor >= '0' && **cursor <= '9') {
    const unsigned int digit = (unsigned int)(**cursor - '0');
    if (number > (UINT64_MAX - digit) / DECIMAL_BASE) return false;
    number = number * DECIMAL_BASE + digit;
    ++*cursor;
  }
  *out = number;
  return *cursor != start;
}

/* 0: ignore unsupported/invalid range; 1: selected; -1: unsatisfiable. */
static int file_range(const char *field, uint64_t size, uint64_t *offset, uint64_t *length) {
  uint64_t first = 0, last = 0;
  field = file_skip_space(field);
  if (strncmp(field, "bytes=", sizeof("bytes=") - 1) != 0) return 0;
  field += sizeof("bytes=") - 1;
  const bool suffix = *field == '-';
  if (!suffix && !file_decimal(&field, &first)) return 0;
  if (*field++ != '-') return 0;
  const bool open_end = *file_skip_space(field) == '\0';
  if ((!open_end || suffix) && !file_decimal(&field, &last)) return 0;
  if (*file_skip_space(field) != '\0') return 0;
  if (!suffix && !open_end && last < first) return 0;
  if (size == 0 || (suffix && last == 0) || (!suffix && first >= size)) return -1;
  if (suffix) {
    *length = last < size ? last : size;
    *offset = size - *length;
  } else {
    if (open_end || last >= size) last = size - 1;
    *offset = first;
    *length = last - first + 1;
  }
  return 1;
}

static const char *file_mime(const char *path) {
  static const struct { const char *extension; const char *type; } types[] = {
    {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
    {".json", "application/json"}, {".txt", "text/plain; charset=utf-8"},
    {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"}, {".webp", "image/webp"}, {".pdf", "application/pdf"},
    {".wasm", "application/wasm"}, {".ico", "image/x-icon"}
  };
  const char *extension = strrchr(path, '.');
  if (extension != NULL)
    for (size_t index = 0; index < sizeof(types) / sizeof(types[0]); ++index)
      if (strcmp(extension, types[index].extension) == 0) return types[index].type;
  return "application/octet-stream";
}

int chttp_server_serve_file(chttp_server_response *response,
    const chttp_server_request_view *request, const chttp_server_file_options *options) {
  salts_fs_stat_t metadata;
  char generated_etag[FILE_ETAG_CAPACITY], modified[CHTTP_SERVER_DATE_CAPACITY];
  char content_range[FILE_RANGE_CAPACITY];
  const char *etag, *condition;
  time_t condition_time;
  uint64_t offset = 0, length;
  unsigned int code = 200;
  int status, match;
  if (response == NULL || response->impl == NULL || request == NULL || options == NULL ||
      options->path == NULL || options->path[0] == '\0') return SALTS_EINVAL;
  if (request->method != CHTTP_METHOD_GET && request->method != CHTTP_METHOD_HEAD) {
    status = chttp_server_response_set_header(response, "Allow", "GET, HEAD");
    return status == SALTS_OK ? chttp_server_reply(response, 405, NULL, NULL, 0) : status;
  }
  status = salts_fs_stat(options->path, &metadata);
  if (status != SALTS_OK) return status;
  if (!metadata.is_file) return SALTS_EISDIR;
  if (metadata.size > SIZE_MAX) return SALTS_EFBIG;
  length = metadata.size;
  const int tag_size = snprintf(generated_etag, sizeof(generated_etag),
      "W/\"%" PRIx64 "-%" PRIx64 "-%" PRIx64 "\"", metadata.size, metadata.mtime, metadata.ctime);
  if (tag_size < 0 || (size_t)tag_size >= sizeof(generated_etag)) return SALTS_ERANGE;
  etag = options->etag != NULL ? options->etag : generated_etag;
  const char *tag_end = file_tag_end(etag);
  if (tag_end == NULL || *tag_end != '\0') return SALTS_EINVAL;
  const uint64_t now = salts_realtime_ms() / MILLISECONDS_PER_SECOND;
  const uint64_t modified_seconds = metadata.mtime / MICROSECONDS_PER_SECOND;
  const time_t mtime = (time_t)(modified_seconds < now ? modified_seconds : now);
  status = chttp_server_format_date(mtime, modified);
  if (status != SALTS_OK) return status;
  status = chttp_server_response_set_header(response, "ETag", etag);
  if (status == SALTS_OK) status = chttp_server_response_set_header(response, "Last-Modified", modified);
  if (status == SALTS_OK) status = chttp_server_response_set_header(response, "Accept-Ranges", "bytes");
  if (status != SALTS_OK) return status;
  match = file_request_tags(request, "If-Match", etag, true);
  if (match != -2) {
    if (match < 0) code = 400;
    else if (!match) code = 412;
  } else {
    condition = file_single_header(request, "If-Unmodified-Since");
    if (condition != NULL && file_http_date(condition, &condition_time) && mtime > condition_time) code = 412;
  }
  if (code != 200) return chttp_server_reply(response, code, NULL, NULL, 0);
  match = file_request_tags(request, "If-None-Match", etag, false);
  if (match != -2) {
    if (match < 0) code = 400;
    else if (match) code = 304;
  } else {
    condition = file_single_header(request, "If-Modified-Since");
    if (condition != NULL && file_http_date(condition, &condition_time) && mtime <= condition_time) code = 304;
  }
  if (code != 200) return chttp_server_reply(response, code, NULL, NULL, 0);
  condition = file_single_header(request, "Range");
  const char *if_range = file_single_header(request, "If-Range");
  if (if_range == NULL && chttp_server_request_header(request, "If-Range") != NULL) condition = NULL;
  if (condition != NULL && request->method == CHTTP_METHOD_GET &&
      (if_range == NULL || file_if_range_matches(if_range, etag))) {
    const int selected = file_range(condition, metadata.size, &offset, &length);
    if (selected < 0) {
      (void)snprintf(content_range, sizeof(content_range), "bytes */%" PRIu64, metadata.size);
      code = 416;
    } else if (selected > 0) {
      (void)snprintf(content_range, sizeof(content_range), "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
                     offset, offset + length - 1, metadata.size);
      code = 206;
    }
    if (code != 200) {
      status = chttp_server_response_set_header(response, "Content-Range", content_range);
      if (status != SALTS_OK) return status;
      if (code == 416) return chttp_server_reply(response, code, NULL, NULL, 0);
    }
  }
  return chttp_server_send_file(response, code,
      options->content_type != NULL ? options->content_type : file_mime(options->path),
      options->path, offset, (size_t)length, true);
}
