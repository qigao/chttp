#include "chttp_web_registration.h"
#include "../../http_server/src/chttp_server_engine.h"
#include <chttp_app/web.h>

#include <core/error_codes.h>
#include <file/fs.h>
#include <core/vstr.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static int chttp_web_asset_bounded_length(
    const char *value, size_t limit, size_t *out_size) {
  size_t size;
  if (value == NULL || out_size == NULL) return SALTS_EINVAL;
  for (size = 0u; size <= limit; ++size) {
    if (value[size] == '\0') {
      *out_size = size;
      return SALTS_OK;
    }
  }
  return SALTS_EMSGSIZE;
}

static int chttp_web_asset_prefix_valid(const char *prefix) {
  size_t size;
  size_t index;
  if (chttp_web_asset_bounded_length(
          prefix, CHTTP_WEB_ASSET_PREFIX_HARD_MAX, &size) != SALTS_OK ||
      size < 2u || prefix[0] != '/' || prefix[size - 1u] == '/')
    return 0;
  for (index = 0u; index < size; ++index) {
    const unsigned char value = (unsigned char)prefix[index];
    if (value < 0x20u || value == 0x7fu || value == (unsigned char)'\\' ||
        value == (unsigned char)'?' || value == (unsigned char)'#' ||
        value == (unsigned char)'%')
      return 0;
  }
  return vstr_utf8_invalid_offset(vstr_from_buf(prefix, size)) == VSTR_NPOS;
}

static int chttp_web_asset_prefix_matches(
    const char *prefix, const char *path) {
  const size_t prefix_size = strlen(prefix);
  return path != NULL && strncmp(path, prefix, prefix_size) == 0 &&
         (path[prefix_size] == '\0' || path[prefix_size] == '/');
}

static int chttp_web_asset_hex(unsigned char value) {
  if (value >= (unsigned char)'0' && value <= (unsigned char)'9')
    return (int)(value - (unsigned char)'0');
  if (value >= (unsigned char)'a' && value <= (unsigned char)'f')
    return (int)(value - (unsigned char)'a') + 10;
  if (value >= (unsigned char)'A' && value <= (unsigned char)'F')
    return (int)(value - (unsigned char)'A') + 10;
  return -1;
}

static int chttp_web_asset_component_byte_valid(unsigned char value) {
  if (value < 0x20u || value == 0x7fu || value == (unsigned char)'\\' ||
      value == (unsigned char)'/' || value == (unsigned char)':' ||
      value == (unsigned char)'*' || value == (unsigned char)'?' ||
      value == (unsigned char)'"' || value == (unsigned char)'<' ||
      value == (unsigned char)'>' || value == (unsigned char)'|' ||
      value == (unsigned char)'#')
    return 0;
  return 1;
}

static int chttp_web_asset_normalize_relative(
    const char *input,
    size_t max_relative_bytes,
    int decode_percent,
    char output[CHTTP_WEB_ASSET_RELATIVE_HARD_MAX + 1u]) {
  char segment[CHTTP_WEB_ASSET_RELATIVE_HARD_MAX + 1u];
  size_t output_size = 0u;
  const char *cursor;

  if (input == NULL || output == NULL || max_relative_bytes == 0u ||
      max_relative_bytes > CHTTP_WEB_ASSET_RELATIVE_HARD_MAX)
    return SALTS_EINVAL;

  cursor = input;
  while (*cursor == '/') ++cursor;
  while (*cursor != '\0') {
    size_t segment_size = 0u;
    while (*cursor != '\0' && *cursor != '/') {
      unsigned char value = (unsigned char)*cursor++;
      if (decode_percent && value == (unsigned char)'%') {
        int high;
        int low;
        if (cursor[0] == '\0' || cursor[1] == '\0')
          return SALTS_EINVAL;
        high = chttp_web_asset_hex((unsigned char)cursor[0]);
        low = chttp_web_asset_hex((unsigned char)cursor[1]);
        if (high < 0 || low < 0) return SALTS_EINVAL;
        value = (unsigned char)((high << 4) | low);
        cursor += 2;
      }
      if (!chttp_web_asset_component_byte_valid(value))
        return SALTS_EPERM;
      if (segment_size == max_relative_bytes ||
          segment_size == CHTTP_WEB_ASSET_RELATIVE_HARD_MAX)
        return SALTS_ENAMETOOLONG;
      segment[segment_size++] = (char)value;
    }

    if (segment_size != 0u) {
      if (segment_size == 1u && segment[0] == '.') {
        /* Canonical dot segment: omit it. */
      } else {
        if (segment_size == 2u && segment[0] == '.' && segment[1] == '.')
          return SALTS_EPERM;
        if (segment[segment_size - 1u] == '.' ||
            segment[segment_size - 1u] == ' ')
          return SALTS_EPERM;
        if (output_size != 0u) {
          if (output_size == max_relative_bytes)
            return SALTS_ENAMETOOLONG;
          output[output_size++] = '/';
        }
        if (segment_size > max_relative_bytes - output_size)
          return SALTS_ENAMETOOLONG;
        memcpy(output + output_size, segment, segment_size);
        output_size += segment_size;
      }
    }

    while (*cursor == '/') ++cursor;
  }

  if (output_size == 0u) return SALTS_ENOENT;
  output[output_size] = '\0';
  if (vstr_utf8_invalid_offset(vstr_from_buf(output, output_size)) != VSTR_NPOS)
    return SALTS_ECHARSET;
  return SALTS_OK;
}

static int chttp_web_asset_path_has_reparse(
    const char *path, fs_stat_t *out) {
  int status = fs_lstat(path, out);
  if (status != SALTS_OK) return status;
#ifdef _WIN32
  {
    const DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return SALTS_EIO;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
      return SALTS_ELOOP;
  }
#else
  if (out->is_symlink) return SALTS_ELOOP;
#endif
  return SALTS_OK;
}

static int chttp_web_asset_resolve_file(
    const chttp_web_asset_mount *mount,
    const char *relative,
    char output[CHTTP_WEB_ASSET_FS_PATH_HARD_MAX + 1u]) {
  fs_stat_t metadata = {0};
  size_t root_size;
  size_t output_size;
  const char *cursor;
  int status;
#ifdef _WIN32
  const char separator = '\\';
#else
  const char separator = '/';
#endif

  if (mount == NULL || relative == NULL || output == NULL)
    return SALTS_EINVAL;
  status = chttp_web_asset_bounded_length(
      mount->filesystem_root, CHTTP_WEB_ASSET_ROOT_HARD_MAX, &root_size);
  if (status != SALTS_OK || root_size == 0u)
    return SALTS_EINVAL;

  memcpy(output, mount->filesystem_root, root_size);
  output_size = root_size;
  output[output_size] = '\0';

  status = chttp_web_asset_path_has_reparse(output, &metadata);
  if (status != SALTS_OK || !metadata.is_directory)
    return status != SALTS_OK ? status : SALTS_ENOTDIR;

  cursor = relative;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, '/');
    const size_t segment_size =
        end != NULL ? (size_t)(end - cursor) : strlen(cursor);
    const int final = end == NULL;

    if (output_size + 1u + segment_size >
        CHTTP_WEB_ASSET_FS_PATH_HARD_MAX)
      return SALTS_ENAMETOOLONG;
    if (output_size == 0u ||
        (output[output_size - 1u] != '/' &&
         output[output_size - 1u] != '\\'))
      output[output_size++] = separator;
    memcpy(output + output_size, cursor, segment_size);
    output_size += segment_size;
    output[output_size] = '\0';

    metadata = (fs_stat_t){0};
    status = chttp_web_asset_path_has_reparse(output, &metadata);
    if (status != SALTS_OK) return status;
    if (final) {
      if (!metadata.is_file) return SALTS_EISDIR;
    } else if (!metadata.is_directory) {
      return SALTS_ENOTDIR;
    }

    if (final) break;
    cursor = end + 1u;
  }

  return SALTS_OK;
}

static int chttp_web_asset_not_found(chttp_server_response *response) {
  return chttp_server_reply(
      response, 404u, "text/plain", "Not Found", sizeof("Not Found") - 1u);
}

static int chttp_web_asset_method_not_allowed(
    chttp_server_response *response) {
  int status = chttp_server_response_set_header(
      response, "Allow", "GET, HEAD");
  return status == SALTS_OK
             ? chttp_server_reply(response, 405u, NULL, NULL, 0u)
             : status;
}

bool chttp_web_assets_matches(const chttp_web_asset_mount *mount, const char *path) {
  return chttp_web_asset_prefix_matches(mount->url_prefix, path) ||
      (mount->spa_url_prefix && chttp_web_asset_prefix_matches(mount->spa_url_prefix, path));
}

int chttp_web_assets_select(const chttp_web_asset_mount *mount,
    const chttp_server_request_view *request, chttp_web_asset_selection *out) {
  if (!mount || !request || !request->path || !out) return SALTS_EINVAL;
  *out = (chttp_web_asset_selection){.kind = CHTTP_WEB_ASSET_NEXT};
  const bool asset = chttp_web_asset_prefix_matches(mount->url_prefix, request->path);
  if (!asset && (!mount->spa_url_prefix || !chttp_web_asset_prefix_matches(mount->spa_url_prefix, request->path)))
    return SALTS_OK;
  if (request->method != CHTTP_METHOD_GET && request->method != CHTTP_METHOD_HEAD) {
    out->kind = CHTTP_WEB_ASSET_METHOD_NOT_ALLOWED;
    return SALTS_OK;
  }
  const char *relative = asset ? request->path + strlen(mount->url_prefix) : mount->spa_fallback_relative_path;
  int status = chttp_web_asset_normalize_relative(relative, mount->max_relative_path_bytes, asset, out->relative);
  if (status == SALTS_OK) status = chttp_web_asset_resolve_file(mount, out->relative, out->path);
  /* Preserve the mount's existing 404 boundary for rejected/missing paths. */
  out->kind = status == SALTS_OK ? CHTTP_WEB_ASSET_FILE : CHTTP_WEB_ASSET_NOT_FOUND;
  return SALTS_OK;
}

static int chttp_web_assets_middleware(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  const chttp_web_asset_mount *mount = user;
  if (!response || !next) return SALTS_EINVAL;
  chttp_web_asset_selection selection;
  int status = chttp_web_assets_select(mount, request, &selection);
  if (status != SALTS_OK) return status;
  switch (selection.kind) {
  case CHTTP_WEB_ASSET_NEXT: return chttp_server_next_call(next);
  case CHTTP_WEB_ASSET_NOT_FOUND: return chttp_web_asset_not_found(response);
  case CHTTP_WEB_ASSET_METHOD_NOT_ALLOWED: return chttp_web_asset_method_not_allowed(response);
  case CHTTP_WEB_ASSET_FILE: break;
  }
  const char *cache = mount->cache_control ? mount->cache_control : "no-cache";
  if (mount->immutable && mount->immutable(mount->user, selection.relative) && mount->immutable_cache_control)
    cache = mount->immutable_cache_control;
  status = chttp_server_response_set_header(response, "Cache-Control", cache);
  if (status != SALTS_OK) return status;
  const chttp_server_file_options options = {selection.path, NULL,
      mount->etag ? mount->etag(mount->user, selection.relative) : NULL};
  return chttp_server_serve_file(response, request, &options);
}

static int chttp_web_asset_mount_valid(
    const chttp_web_asset_mount *mount) {
  fs_stat_t root_metadata = {0};
  char fallback[CHTTP_WEB_ASSET_RELATIVE_HARD_MAX + 1u];
  size_t root_size;
  int status;

  if (mount == NULL || mount->size < sizeof(*mount) ||
      !chttp_web_asset_prefix_valid(mount->url_prefix) ||
      mount->filesystem_root == NULL ||
      mount->max_relative_path_bytes == 0u ||
      mount->max_relative_path_bytes > CHTTP_WEB_ASSET_RELATIVE_HARD_MAX)
    return 0;

  if ((mount->spa_url_prefix == NULL) !=
      (mount->spa_fallback_relative_path == NULL))
    return 0;
  if (mount->spa_url_prefix != NULL &&
      !chttp_web_asset_prefix_valid(mount->spa_url_prefix))
    return 0;

  if (chttp_web_asset_bounded_length(
          mount->filesystem_root, CHTTP_WEB_ASSET_ROOT_HARD_MAX,
          &root_size) != SALTS_OK ||
      root_size == 0u)
    return 0;

  status = chttp_web_asset_path_has_reparse(
      mount->filesystem_root, &root_metadata);
  if (status != SALTS_OK || !root_metadata.is_directory)
    return 0;

  if (mount->spa_fallback_relative_path != NULL &&
      chttp_web_asset_normalize_relative(
          mount->spa_fallback_relative_path,
          mount->max_relative_path_bytes, 0, fallback) != SALTS_OK)
    return 0;

  return 1;
}

int chttp_web_assets_validate(const chttp_web_asset_mount *mount) {
  return chttp_web_asset_mount_valid(mount) ? SALTS_OK : SALTS_EINVAL;
}

int chttp_web_assets_use(
    chttp_server *server,
    const chttp_web_asset_mount *mount) {
  if (server == NULL) return SALTS_EINVAL;
  const int status = chttp_web_assets_validate(mount);
  if (status != SALTS_OK) return status;
  return chttp_server_use(
      server, chttp_web_assets_middleware, (void *)mount);
}
