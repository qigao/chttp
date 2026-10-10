#include "chttp_test_thread_cpu.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include "tinytest.h"

static chttp_test_thread_cpu measured;
static cmeta_thread_t worker;
static bool worker_started;
static int worker_status;
static uint64_t worker_cpu_ns;
static chttp_test_thread_cpu published_cpu;
static atomic_int worker_ready;
static atomic_int worker_release;

static void cpu_work(void *unused) {
  chttp_test_thread_cpu cpu = {0};
  uint64_t before = 0u, after = 0u;
  volatile uint64_t value = 1u;
  (void)unused;
  worker_status = chttp_test_thread_cpu_capture(&cpu);
  if (worker_status == SALTS_OK)
    worker_status = chttp_test_thread_cpu_read(&cpu, &before);
  after = before;
  const uint64_t deadline = cmeta_monotonic_ms() + 2000u;
  while (worker_status == SALTS_OK && after - before < 100000000u) {
    for (size_t i = 0u; i < 65536u; ++i) value = value * 1664525u + 1013904223u;
    worker_status = chttp_test_thread_cpu_read(&cpu, &after);
    if (after < before || cmeta_monotonic_ms() >= deadline) {
      worker_status = SALTS_ETIMEDOUT;
      break;
    }
  }
  worker_cpu_ns = after >= before ? after - before : 0u;
  /* A borrowed capability published while its thread remains alive. */
  published_cpu = cpu;
  atomic_store_explicit(&worker_ready, 1, memory_order_release);
  while (!atomic_load_explicit(&worker_release, memory_order_acquire))
    cmeta_sleep_ms(1u);
  const int release_status = chttp_test_thread_cpu_release(&cpu);
  if (worker_status == SALTS_OK) worker_status = release_status;
}

spec("Thread CPU measurement") {
  before_each() {
    measured = (chttp_test_thread_cpu){0};
    worker_started = false;
    worker_status = SALTS_EIO;
    worker_cpu_ns = 0u;
    published_cpu = (chttp_test_thread_cpu){0};
    atomic_init(&worker_ready, 0);
    atomic_init(&worker_release, 0);
  }
  after_each() {
    atomic_store_explicit(&worker_release, 1, memory_order_release);
    if (worker_started) {
      check_equal(cmeta_thread_join(&worker), SALTS_OK);
      cmeta_thread_destroy(&worker);
    }
    check_equal(chttp_test_thread_cpu_release(&measured), SALTS_OK);
  }
  it("rejects invalid state and overflow without replacing the output") {
    uint64_t ns = 7u;
    check_equal(chttp_test_thread_cpu_read(&measured, &ns), SALTS_EINVAL);
    check_equal(chttp_test_cpu_units_ns(UINT64_MAX, 1u, 100u, &ns), SALTS_ERANGE);
    check_equal(chttp_test_cpu_units_ns(UINT64_MAX, 0u, 100u, &ns), SALTS_ERANGE);
    check_equal(ns, 7u);
    check_equal(chttp_test_cpu_units_ns(2u, 3u, 100u, &ns), SALTS_OK);
    check_equal(ns, 500u);
    check_equal(chttp_test_thread_cpu_capture(&measured), SALTS_OK);
    check_equal(chttp_test_thread_cpu_capture(&measured), SALTS_EBUSY);
  }
  it("does not charge sleep or another thread's CPU to the measured thread") {
    uint64_t before, after;
    check_equal(chttp_test_thread_cpu_capture(&measured), SALTS_OK);
    check_equal(chttp_test_thread_cpu_read(&measured, &before), SALTS_OK);
    check_equal(cmeta_thread_create(&worker, cpu_work, NULL), SALTS_OK);
    worker_started = true;
    cmeta_sleep_ms(200u);
    while (!atomic_load_explicit(&worker_ready, memory_order_acquire))
      cmeta_sleep_ms(1u);
    uint64_t other_cpu = 0u;
    check_equal(chttp_test_thread_cpu_read(&published_cpu, &other_cpu), SALTS_OK);
    check(other_cpu >= worker_cpu_ns);
    check(published_cpu.token != measured.token);
    atomic_store_explicit(&worker_release, 1, memory_order_release);
    check_equal(cmeta_thread_join(&worker), SALTS_OK);
    cmeta_thread_destroy(&worker);
    worker_started = false;
    check_equal(chttp_test_thread_cpu_read(&measured, &after), SALTS_OK);
    check_equal(worker_status, SALTS_OK);
    check(worker_cpu_ns >= 100000000u);
    check(after >= before);
    /* Broad enough for instrumented test overhead; rejects both wall time and
     * process-wide CPU accounting. No throughput or scheduler-speed assertion. */
    check(after - before < 80000000u);
  }
}
