#include <chttp_web/web.h>

#include <cmeta/struct.h>
#include <salts/clock.h>
#include <vstr.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_BENCH_STARTUP_SAMPLES = 50,
  WEB_BENCH_RENDER_SAMPLES = 1000,
  WEB_BENCH_WARMUP = 100
};

typedef struct web_bench_model {
  vstr title;
} web_bench_model;

static const cmeta_type_identity WEB_BENCH_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.benchmark.model");

static const cmeta_type_desc WEB_BENCH_MODEL_TYPE = {
    "web_bench_model",
    sizeof(web_bench_model),
    _Alignof(web_bench_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_BENCH_MODEL_ID};

static const cmeta_field_desc WEB_BENCH_LAYOUT_FIELDS[] = {
    {"title",
     "vstr",
     offsetof(web_bench_model, title),
     sizeof(((web_bench_model *)0)->title),
     _Alignof(vstr),
     NULL,
     NULL}};

static const cmeta_struct_desc WEB_BENCH_LAYOUT = {
    "web_bench_model",
    sizeof(web_bench_model),
    _Alignof(web_bench_model),
    WEB_BENCH_LAYOUT_FIELDS,
    1u};

static const char WEB_BENCH_LAYOUT_TEMPLATE[] =
    "<!doctype html><html><head><title>{{ title }}</title></head>"
    "<body>{% block body %}{% endblock %}</body></html>";

static const char WEB_BENCH_PAGE_TEMPLATE[] =
    "{% extends \"layout.html\" %}"
    "{% block body %}<main><h1>{{ title }}</h1>"
    "<p>{{ title }}</p><p>{{ title }}</p><p>{{ title }}</p>"
    "</main>{% endblock %}";

static const char WEB_BENCH_FRAGMENT_TEMPLATE[] =
    "<section><h2>{{ title }}</h2><p>{{ title }}</p></section>";

static const chttp_web_template WEB_BENCH_TEMPLATES[] = {
    {"layout.html", WEB_BENCH_LAYOUT_TEMPLATE,
     sizeof(WEB_BENCH_LAYOUT_TEMPLATE) - 1u},
    {"page.html", WEB_BENCH_PAGE_TEMPLATE,
     sizeof(WEB_BENCH_PAGE_TEMPLATE) - 1u},
    {"fragment.html", WEB_BENCH_FRAGMENT_TEMPLATE,
     sizeof(WEB_BENCH_FRAGMENT_TEMPLATE) - 1u}};

static int web_bench_u64_compare(const void *lhs, const void *rhs) {
  const uint64_t a = *(const uint64_t *)lhs;
  const uint64_t b = *(const uint64_t *)rhs;
  return a < b ? -1 : a > b ? 1 : 0;
}

static const char *web_bench_env(const char *name) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : "unknown";
}

static const char *web_bench_platform(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

static const char *web_bench_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "unknown";
#endif
}

static cmeta_data_desc web_bench_model_desc(
    cmeta_data_field_desc fields[1],
    cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.benchmark.model.title",
      "title",
      offsetof(web_bench_model, title),
      chttp_web_vstr_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_BENCH_LAYOUT,
      fields,
      1u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.benchmark.model.data",
      .display_name = "CHttp Web benchmark model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_BENCH_MODEL_TYPE,
      .shape = shape};
}

static uint64_t web_bench_percentile(
    const uint64_t *samples, size_t count, unsigned int percent) {
  uint64_t ordered[WEB_BENCH_RENDER_SAMPLES];
  size_t index;
  size_t rank;

  if (count == 0u || count > WEB_BENCH_RENDER_SAMPLES) return 0u;
  for (index = 0u; index < count; ++index) ordered[index] = samples[index];
  qsort(ordered, count, sizeof(ordered[0]), web_bench_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  return ordered[rank - 1u];
}

static void web_bench_print_measurement(
    const char *name,
    size_t contexts,
    size_t samples,
    const uint64_t *elapsed,
    size_t output_bytes) {
  uint64_t total = 0u;
  uint64_t minimum = UINT64_MAX;
  uint64_t maximum = 0u;
  size_t index;

  for (index = 0u; index < samples; ++index) {
    total += elapsed[index];
    if (elapsed[index] < minimum) minimum = elapsed[index];
    if (elapsed[index] > maximum) maximum = elapsed[index];
  }

  printf(
      "{\"kind\":\"measurement\",\"benchmark\":\"chttp_web_renderer\","
      "\"name\":\"%s\",\"contexts\":%zu,\"samples\":%zu,"
      "\"total_ns\":%" PRIu64 ",\"mean_ns\":%.3f,"
      "\"min_ns\":%" PRIu64 ",\"p50_ns\":%" PRIu64 ","
      "\"p95_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64 ","
      "\"output_bytes\":%zu}\n",
      name,
      contexts,
      samples,
      total,
      samples != 0u ? (double)total / (double)samples : 0.0,
      minimum == UINT64_MAX ? 0u : minimum,
      web_bench_percentile(elapsed, samples, 50u),
      web_bench_percentile(elapsed, samples, 95u),
      maximum,
      output_bytes);
  fflush(stdout);
}

static int web_bench_startup(void) {
  uint64_t elapsed[WEB_BENCH_STARTUP_SAMPLES];
  chttp_web_renderer_config config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  size_t index;

  for (index = 0u; index < WEB_BENCH_STARTUP_SAMPLES; ++index) {
    chttp_web_renderer renderer = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const uint64_t started = salts_hrtime();
    const chttp_web_status status = chttp_web_renderer_init(
        &renderer,
        WEB_BENCH_TEMPLATES,
        sizeof(WEB_BENCH_TEMPLATES) / sizeof(WEB_BENCH_TEMPLATES[0]),
        &config,
        &error);
    elapsed[index] = salts_hrtime() - started;
    if (status != CHTTP_WEB_OK) {
      fprintf(
          stderr,
          "renderer startup failed: status=%d native=%d message=%s\n",
          (int)status,
          error.native_status,
          error.message);
      chttp_web_renderer_destroy(&renderer);
      return 1;
    }
    chttp_web_renderer_destroy(&renderer);
  }

  web_bench_print_measurement(
      "renderer_startup",
      1u,
      WEB_BENCH_STARTUP_SAMPLES,
      elapsed,
      0u);
  return 0;
}

static int web_bench_render_case(
    const char *name,
    const char *template_name,
    size_t contexts,
    const cmeta_data_desc *desc,
    const web_bench_model *model) {
  chttp_web_renderer *renderers = NULL;
  chttp_web_renderer_config config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  uint64_t elapsed[WEB_BENCH_RENDER_SAMPLES];
  size_t output_bytes = 0u;
  size_t index;
  int result = 1;

  renderers = (chttp_web_renderer *)calloc(contexts, sizeof(*renderers));
  if (renderers == NULL) {
    fprintf(stderr, "failed to allocate %zu renderer contexts\n", contexts);
    return 1;
  }

  for (index = 0u; index < contexts; ++index) {
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const chttp_web_status status = chttp_web_renderer_init(
        &renderers[index],
        WEB_BENCH_TEMPLATES,
        sizeof(WEB_BENCH_TEMPLATES) / sizeof(WEB_BENCH_TEMPLATES[0]),
        &config,
        &error);
    if (status != CHTTP_WEB_OK) {
      fprintf(
          stderr,
          "renderer context init failed: status=%d native=%d message=%s\n",
          (int)status,
          error.native_status,
          error.message);
      goto cleanup;
    }
  }

  for (index = 0u; index < WEB_BENCH_WARMUP; ++index) {
    char *html = NULL;
    size_t html_size = 0u;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const chttp_web_status status = chttp_web_render(
        &renderers[index % contexts],
        template_name,
        desc,
        model,
        &html,
        &html_size,
        &error);
    if (status != CHTTP_WEB_OK) {
      fprintf(stderr, "warmup render failed: status=%d\n", (int)status);
      chttp_web_output_free(html);
      goto cleanup;
    }
    chttp_web_output_free(html);
  }

  for (index = 0u; index < WEB_BENCH_RENDER_SAMPLES; ++index) {
    char *html = NULL;
    size_t html_size = 0u;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const uint64_t started = salts_hrtime();
    const chttp_web_status status = chttp_web_render(
        &renderers[index % contexts],
        template_name,
        desc,
        model,
        &html,
        &html_size,
        &error);
    elapsed[index] = salts_hrtime() - started;
    if (status != CHTTP_WEB_OK || html == NULL || html_size == 0u) {
      fprintf(
          stderr,
          "measured render failed: status=%d native=%d message=%s\n",
          (int)status,
          error.native_status,
          error.message);
      chttp_web_output_free(html);
      goto cleanup;
    }
    if (index == 0u) output_bytes = html_size;
    else if (html_size != output_bytes) {
      fprintf(stderr, "render output size changed during measurement\n");
      chttp_web_output_free(html);
      goto cleanup;
    }
    chttp_web_output_free(html);
  }

  web_bench_print_measurement(
      name,
      contexts,
      WEB_BENCH_RENDER_SAMPLES,
      elapsed,
      output_bytes);
  result = 0;

cleanup:
  for (index = 0u; index < contexts; ++index)
    chttp_web_renderer_destroy(&renderers[index]);
  free(renderers);
  return result;
}

int main(void) {
  static const size_t context_counts[] = {1u, 8u, 32u};
  cmeta_data_field_desc fields[1];
  cmeta_data_struct_shape shape;
  cmeta_data_desc desc = web_bench_model_desc(fields, &shape);
  web_bench_model model = {
      .title = vstr_from_cstr("Benchmark <typed> & deterministic")};
  size_t index;

  if (!cmeta_data_desc_valid(&desc)) {
    fprintf(stderr, "benchmark model descriptor is invalid\n");
    return 2;
  }

  printf(
      "{\"kind\":\"environment\",\"benchmark\":\"chttp_web_renderer\","
      "\"commit\":\"%s\",\"salts_sha\":\"%s\","
      "\"salts_utils_sha\":\"%s\",\"vcpkg_sha\":\"%s\","
      "\"platform\":\"%s\",\"arch\":\"%s\","
      "\"compiler\":\"%s\",\"warmup\":%u,"
      "\"startup_samples\":%u,\"render_samples\":%u,"
      "\"note\":\"timings are evidence only; no performance threshold is applied\"}\n",
      web_bench_env("GITHUB_SHA"),
      web_bench_env("SALTS_SHA"),
      web_bench_env("SALTS_UTILS_SHA"),
      web_bench_env("VCPKG_SHA"),
      web_bench_platform(),
      web_bench_arch(),
#if defined(__clang__)
      "clang " __clang_version__,
#elif defined(__GNUC__)
      "gcc " __VERSION__,
#elif defined(_MSC_VER)
      "msvc",
#else
      "unknown",
#endif
      WEB_BENCH_WARMUP,
      WEB_BENCH_STARTUP_SAMPLES,
      WEB_BENCH_RENDER_SAMPLES);
  fflush(stdout);

  if (web_bench_startup() != 0) return 3;

  for (index = 0u;
       index < sizeof(context_counts) / sizeof(context_counts[0]);
       ++index) {
    const size_t contexts = context_counts[index];
    if (web_bench_render_case(
            "full_page_render",
            "page.html",
            contexts,
            &desc,
            &model) != 0)
      return 4;
    if (web_bench_render_case(
            "fragment_render",
            "fragment.html",
            contexts,
            &desc,
            &model) != 0)
      return 5;
  }

  return 0;
}
