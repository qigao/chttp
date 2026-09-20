#include <chttp_web/web.h>

#include <jinja_cmeta.h>
#include <jinja_cmeta_runtime.h>
#include <salts/error_codes.h>
#include <vstr.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_web_template_entry {
  vstr name;
  vstr source;
  JINJA_CMETA_TEMPLATE *templ;
} chttp_web_template_entry;

typedef struct chttp_web_renderer_impl {
  JINJA_CMETA_ENV *env;
  JINJA_CMETA_RUNTIME_CONFIG *runtime;
  JINJA_CMETA_RENDER_OPTIONS render_options;
  chttp_web_template_entry *templates;
  size_t template_count;
} chttp_web_renderer_impl;

typedef struct chttp_web_render_buffer {
  char *data;
  size_t size;
  size_t capacity;
  size_t limit;
  chttp_web_status failure;
} chttp_web_render_buffer;

static chttp_web_status chttp_web_fail(
    chttp_web_error *error, chttp_web_status status, int native_status,
    const char *message) {
  if (error) {
    error->status = status;
    error->native_status = native_status;
    error->offset = 0u;
    error->template_name[0] = '\0';
    if (message) {
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
    } else {
      error->message[0] = '\0';
    }
  }
  return status;
}

static chttp_web_status chttp_web_from_jinja(
    chttp_web_error *error, JINJA_CMETA_STATUS status,
    const JINJA_CMETA_ERROR *jerror, int compiling) {
  chttp_web_status mapped;
  switch (status) {
  case JINJA_CMETA_OK:
    mapped = CHTTP_WEB_OK;
    break;
  case JINJA_CMETA_ERR_INVALID_ARGUMENT:
    mapped = CHTTP_WEB_INVALID_ARGUMENT;
    break;
  case JINJA_CMETA_ERR_CAPACITY:
    mapped = CHTTP_WEB_CAPACITY;
    break;
  case JINJA_CMETA_ERR_OUT_OF_MEMORY:
    mapped = CHTTP_WEB_OUT_OF_MEMORY;
    break;
  case JINJA_CMETA_ERR_METADATA:
    mapped = CHTTP_WEB_METADATA;
    break;
  case JINJA_CMETA_ERR_NOT_FOUND:
    mapped = CHTTP_WEB_NOT_FOUND;
    break;
  case JINJA_CMETA_ERR_SYNTAX:
  case JINJA_CMETA_ERR_UNSUPPORTED:
  case JINJA_CMETA_ERR_LOADER:
    mapped = compiling ? CHTTP_WEB_TEMPLATE : CHTTP_WEB_RENDER;
    break;
  case JINJA_CMETA_ERR_RENDER:
  default:
    mapped = compiling ? CHTTP_WEB_TEMPLATE : CHTTP_WEB_RENDER;
    break;
  }

  if (error) {
    error->status = mapped;
    error->native_status = 0;
    error->offset = jerror ? jerror->offset : 0u;
    error->template_name[0] = '\0';
    error->message[0] = '\0';
    if (jerror) {
      size_t name_size = jerror->template_name_length;
      if (name_size >= sizeof(error->template_name))
        name_size = sizeof(error->template_name) - 1u;
      if (name_size != 0u)
        memcpy(error->template_name, jerror->template_name, name_size);
      error->template_name[name_size] = '\0';
      (void)snprintf(error->message, sizeof(error->message), "%s", jerror->message);
    }
  }
  return mapped;
}

static int chttp_web_config_valid(const chttp_web_renderer_config *config) {
  return config != NULL && config->max_template_bytes != 0u &&
         config->max_output_bytes != 0u && config->max_nodes != 0u &&
         config->max_value_visits != 0u && config->max_render_depth != 0u;
}

static char *chttp_web_copy_bytes(const char *data, size_t size) {
  char *copy;
  if ((size != 0u && data == NULL) || size == SIZE_MAX) return NULL;
  copy = (char *)malloc(size + 1u);
  if (!copy) return NULL;
  if (size != 0u) memcpy(copy, data, size);
  copy[size] = '\0';
  return copy;
}

static JINJA_CMETA_STATUS chttp_web_bundle_load(
    void *userdata, vstr name, JINJA_CMETA_SOURCE *source,
    JINJA_CMETA_ERROR *error) {
  chttp_web_renderer_impl *impl = (chttp_web_renderer_impl *)userdata;
  (void)error;
  if (!impl || !source || !vstr_is_valid(name))
    return JINJA_CMETA_ERR_INVALID_ARGUMENT;
  for (size_t i = 0u; i < impl->template_count; ++i) {
    if (!vstr_eq(impl->templates[i].name, name)) continue;
    *source = (JINJA_CMETA_SOURCE){
        .text = impl->templates[i].source,
        .lease = &impl->templates[i]};
    return JINJA_CMETA_OK;
  }
  return JINJA_CMETA_ERR_NOT_FOUND;
}

static void chttp_web_bundle_release(
    void *userdata, JINJA_CMETA_SOURCE *source) {
  (void)userdata;
  if (source) *source = (JINJA_CMETA_SOURCE){0};
}

static int chttp_web_html_autoescape(void *userdata, vstr name) {
  (void)userdata;
  (void)name;
  return 1;
}

static chttp_web_template_entry *chttp_web_template_find(
    chttp_web_renderer_impl *impl, const char *name) {
  const vstr requested = name ? vstr_from_cstr(name) : (vstr){0};
  if (!impl || !name || !vstr_is_valid(requested)) return NULL;
  for (size_t i = 0u; i < impl->template_count; ++i)
    if (vstr_eq(impl->templates[i].name, requested)) return &impl->templates[i];
  return NULL;
}

static int chttp_web_render_buffer_write(
    const char *text, size_t size, void *userdata) {
  chttp_web_render_buffer *buffer = (chttp_web_render_buffer *)userdata;
  size_t required;
  size_t storage;
  size_t max_capacity;
  size_t next_capacity;
  char *grown;

  if (!buffer || (size != 0u && !text)) return -1;
  if (buffer->failure != CHTTP_WEB_OK) return -1;
  if (buffer->size > buffer->limit || size > buffer->limit - buffer->size) {
    buffer->failure = CHTTP_WEB_CAPACITY;
    return -1;
  }

  required = buffer->size + size;
  if (required == SIZE_MAX) {
    buffer->failure = CHTTP_WEB_CAPACITY;
    return -1;
  }
  storage = required + 1u;
  if (storage > buffer->capacity) {
    max_capacity = buffer->limit == SIZE_MAX ? SIZE_MAX : buffer->limit + 1u;
    next_capacity = buffer->capacity ? buffer->capacity : 256u;
    if (next_capacity > max_capacity) next_capacity = max_capacity;
    while (next_capacity < storage) {
      if (next_capacity > max_capacity / 2u) {
        next_capacity = max_capacity;
        break;
      }
      next_capacity *= 2u;
    }
    if (next_capacity < storage) next_capacity = storage;
    grown = (char *)realloc(buffer->data, next_capacity);
    if (!grown) {
      buffer->failure = CHTTP_WEB_OUT_OF_MEMORY;
      return -1;
    }
    buffer->data = grown;
    buffer->capacity = next_capacity;
  }

  if (size != 0u) memcpy(buffer->data + buffer->size, text, size);
  buffer->size = required;
  buffer->data[buffer->size] = '\0';
  return 0;
}

static void chttp_web_impl_destroy(chttp_web_renderer_impl *impl) {
  if (!impl) return;
  if (impl->templates) {
    for (size_t i = 0u; i < impl->template_count; ++i)
      jinja_cmeta_release(impl->templates[i].templ);
  }
  jinja_cmeta_runtime_config_destroy(impl->runtime);
  jinja_cmeta_env_destroy(impl->env);
  if (impl->templates) {
    for (size_t i = 0u; i < impl->template_count; ++i) {
      free((void *)impl->templates[i].name.data);
      free((void *)impl->templates[i].source.data);
    }
  }
  free(impl->templates);
  free(impl);
}

chttp_web_status chttp_web_renderer_init(
    chttp_web_renderer *renderer, const chttp_web_template *templates,
    size_t template_count, const chttp_web_renderer_config *config,
    chttp_web_error *error) {
  JINJA_CMETA_ERROR jerror = JINJA_CMETA_ERROR_INIT;
  JINJA_CMETA_ENV_OPTIONS env_options = JINJA_CMETA_ENV_OPTIONS_INIT;
  chttp_web_renderer_impl *impl = NULL;
  size_t source_bytes = 0u;

  if (!renderer || !templates || template_count == 0u ||
      template_count > JINJA_CMETA_MAX_CACHED_TEMPLATES ||
      !chttp_web_config_valid(config))
    return chttp_web_fail(error, CHTTP_WEB_INVALID_ARGUMENT, 0,
                          "renderer initialization arguments are invalid");
  renderer->impl = NULL;

  impl = (chttp_web_renderer_impl *)calloc(1u, sizeof(*impl));
  if (!impl)
    return chttp_web_fail(error, CHTTP_WEB_OUT_OF_MEMORY, 0,
                          "renderer is out of memory");
  impl->templates = (chttp_web_template_entry *)calloc(
      template_count, sizeof(*impl->templates));
  if (!impl->templates) {
    chttp_web_impl_destroy(impl);
    return chttp_web_fail(error, CHTTP_WEB_OUT_OF_MEMORY, 0,
                          "unable to allocate template bundle");
  }
  impl->template_count = template_count;

  for (size_t i = 0u; i < template_count; ++i) {
    const chttp_web_template *input = &templates[i];
    size_t name_size;
    char *name_copy;
    char *source_copy;

    if (!input->name || input->name[0] == '\0' ||
        (input->source_size != 0u && !input->source)) {
      chttp_web_impl_destroy(impl);
      return chttp_web_fail(error, CHTTP_WEB_INVALID_ARGUMENT, 0,
                            "template bundle entry is invalid");
    }
    name_size = strlen(input->name);
    if (name_size > JINJA_CMETA_MAX_TEMPLATE_BYTES ||
        input->source_size > config->max_template_bytes ||
        source_bytes > config->max_template_bytes - input->source_size) {
      chttp_web_impl_destroy(impl);
      return chttp_web_fail(error, CHTTP_WEB_CAPACITY, 0,
                            "template bundle exceeds renderer limit");
    }

    for (size_t j = 0u; j < i; ++j) {
      if (impl->templates[j].name.len == name_size &&
          memcmp(impl->templates[j].name.data, input->name, name_size) == 0) {
        chttp_web_impl_destroy(impl);
        return chttp_web_fail(error, CHTTP_WEB_INVALID_ARGUMENT, 0,
                              "template names must be unique");
      }
    }

    name_copy = chttp_web_copy_bytes(input->name, name_size);
    source_copy = chttp_web_copy_bytes(input->source, input->source_size);
    if (!name_copy || !source_copy) {
      free(name_copy);
      free(source_copy);
      chttp_web_impl_destroy(impl);
      return chttp_web_fail(error, CHTTP_WEB_OUT_OF_MEMORY, 0,
                            "unable to copy template bundle");
    }
    impl->templates[i].name = vstr_from_buf(name_copy, name_size);
    impl->templates[i].source = vstr_from_buf(source_copy, input->source_size);
    source_bytes += input->source_size;
  }

  env_options.loader = (JINJA_CMETA_LOADER){
      chttp_web_bundle_load, chttp_web_bundle_release, impl};
  env_options.max_loaded_templates = template_count;
  env_options.max_loaded_source_bytes = source_bytes ? source_bytes : 1u;
  env_options.autoescape_selector = chttp_web_html_autoescape;
  impl->env = jinja_cmeta_env_create(&env_options, &jerror);
  if (!impl->env) {
    const chttp_web_status result =
        chttp_web_from_jinja(error, jerror.status, &jerror, 1);
    chttp_web_impl_destroy(impl);
    return result;
  }

  for (size_t i = 0u; i < template_count; ++i) {
    impl->templates[i].templ =
        jinja_cmeta_env_load(impl->env, impl->templates[i].name, &jerror);
    if (!impl->templates[i].templ) {
      const chttp_web_status result =
          chttp_web_from_jinja(error, jerror.status, &jerror, 1);
      chttp_web_impl_destroy(impl);
      return result;
    }
  }

  impl->runtime = jinja_cmeta_runtime_config_create(&jerror);
  if (!impl->runtime) {
    const chttp_web_status result =
        chttp_web_from_jinja(error, jerror.status, &jerror, 0);
    chttp_web_impl_destroy(impl);
    return result;
  }
  if (jinja_cmeta_runtime_config_set_limit(
          impl->runtime, JINJA_CMETA_RESOURCE_CELLS,
          config->max_nodes, &jerror) != JINJA_CMETA_OK ||
      jinja_cmeta_runtime_config_set_limit(
          impl->runtime, JINJA_CMETA_RESOURCE_ACTIVATIONS,
          config->max_nodes, &jerror) != JINJA_CMETA_OK ||
      jinja_cmeta_runtime_config_set_limit(
          impl->runtime, JINJA_CMETA_RESOURCE_VALUES,
          config->max_value_visits, &jerror) != JINJA_CMETA_OK) {
    const chttp_web_status result =
        chttp_web_from_jinja(error, jerror.status, &jerror, 0);
    chttp_web_impl_destroy(impl);
    return result;
  }

  impl->render_options =
      (JINJA_CMETA_RENDER_OPTIONS)JINJA_CMETA_RENDER_OPTIONS_INIT;
  impl->render_options.max_nodes = config->max_nodes;
  impl->render_options.max_string_bytes = config->max_output_bytes;
  impl->render_options.max_render_depth = config->max_render_depth;
  impl->render_options.max_value_visits = config->max_value_visits;

  renderer->impl = impl;
  return chttp_web_fail(error, CHTTP_WEB_OK, 0, NULL);
}

chttp_web_status chttp_web_render(
    chttp_web_renderer *renderer, const char *template_name,
    const cmeta_data_desc *model_desc, const void *model,
    char **out_html, size_t *out_size, chttp_web_error *error) {
  JINJA_CMETA_ERROR jerror = JINJA_CMETA_ERROR_INIT;
  const JINJA_CMETA_RENDERER output_renderer = {chttp_web_render_buffer_write};
  chttp_web_renderer_impl *impl;
  chttp_web_template_entry *entry;
  chttp_web_render_buffer buffer = {0};
  JINJA_CMETA_STATUS status;

  if (out_html) *out_html = NULL;
  if (out_size) *out_size = 0u;
  if (!renderer || !renderer->impl || !template_name || !model_desc ||
      !model || !out_html || !out_size || !cmeta_data_desc_valid(model_desc))
    return chttp_web_fail(error, CHTTP_WEB_INVALID_ARGUMENT, 0,
                          "render arguments are invalid");

  impl = (chttp_web_renderer_impl *)renderer->impl;
  entry = chttp_web_template_find(impl, template_name);
  if (!entry)
    return chttp_web_fail(error, CHTTP_WEB_NOT_FOUND, 0,
                          "template name was not found");

  buffer.limit = impl->render_options.max_string_bytes;
  status = jinja_cmeta_render_ex(
      entry->templ, model_desc, model, &impl->render_options, impl->runtime,
      &output_renderer, &buffer, &jerror);
  if (status != JINJA_CMETA_OK || buffer.failure != CHTTP_WEB_OK) {
    chttp_web_status result;
    if (buffer.failure == CHTTP_WEB_CAPACITY) {
      result = chttp_web_fail(error, CHTTP_WEB_CAPACITY, 0,
                              "render exceeds renderer limits");
    } else if (buffer.failure == CHTTP_WEB_OUT_OF_MEMORY) {
      result = chttp_web_fail(error, CHTTP_WEB_OUT_OF_MEMORY, 0,
                              "renderer is out of memory");
    } else {
      result = chttp_web_from_jinja(error, status, &jerror, 0);
    }
    free(buffer.data);
    return result;
  }

  if (!buffer.data) {
    buffer.data = (char *)malloc(1u);
    if (!buffer.data)
      return chttp_web_fail(error, CHTTP_WEB_OUT_OF_MEMORY, 0,
                            "renderer is out of memory");
    buffer.data[0] = '\0';
  }

  *out_html = buffer.data;
  *out_size = buffer.size;
  return chttp_web_fail(error, CHTTP_WEB_OK, 0, NULL);
}

chttp_web_status chttp_web_render_response(
    chttp_web_renderer *renderer, chttp_server_response *response,
    const char *template_name, const cmeta_data_desc *model_desc,
    const void *model, unsigned int status_code, const char *content_type,
    chttp_web_error *error) {
  static const char default_content_type[] = "text/html; charset=utf-8";
  char *html = NULL;
  size_t html_size = 0u;
  chttp_web_status status;
  int server_status;

  if (!response)
    return chttp_web_fail(error, CHTTP_WEB_INVALID_ARGUMENT, 0,
                          "response is required");

  status = chttp_web_render(
      renderer, template_name, model_desc, model, &html, &html_size, error);
  if (status != CHTTP_WEB_OK) return status;

  if (status_code == 0u) status_code = 200u;
  if (!content_type) content_type = default_content_type;
  server_status =
      chttp_server_reply(response, status_code, content_type, html, html_size);
  free(html);
  if (server_status != SALTS_OK)
    return chttp_web_fail(error, CHTTP_WEB_SERVER, server_status,
                          "CHTTP rejected the rendered response");
  return chttp_web_fail(error, CHTTP_WEB_OK, 0, NULL);
}

const cmeta_data_desc *chttp_web_vstr_cmeta_data(void) {
  return jinja_cmeta_vstr_data();
}

const cmeta_data_desc *chttp_web_sequence_cmeta_data(void) {
  return jinja_cmeta_sequence_data();
}

void chttp_web_output_free(char *html) {
  free(html);
}

void chttp_web_renderer_destroy(chttp_web_renderer *renderer) {
  if (!renderer) return;
  chttp_web_impl_destroy((chttp_web_renderer_impl *)renderer->impl);
  renderer->impl = NULL;
}
