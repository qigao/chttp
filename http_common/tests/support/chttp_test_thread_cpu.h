#ifndef CHTTP_TEST_THREAD_CPU_H
#define CHTTP_TEST_THREAD_CPU_H

/* Include first: the strict-C11 Linux build needs POSIX thread CPU clocks.
 * Benchmark/test-only platform adapter. Salts has no public CPU-time query;
 * never inspect the representation of its opaque cmeta_thread_t. */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <salts/thread.h>
#include <stdint.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <pthread.h>
#include <time.h>
#endif

typedef struct chttp_test_thread_cpu {
  const void *token;
#if defined(_WIN32)
  HANDLE handle;
#elif defined(__APPLE__)
  thread_t handle;
#elif defined(__linux__)
  clockid_t clock_id;
#endif
} chttp_test_thread_cpu;

static inline int chttp_test_cpu_units_ns(uint64_t user, uint64_t kernel,
                                         uint64_t scale, uint64_t *out) {
  if (out == NULL || scale == 0u) return SALTS_EINVAL;
  if (kernel > UINT64_MAX - user || user + kernel > UINT64_MAX / scale)
    return SALTS_ERANGE;
  *out = (user + kernel) * scale;
  return SALTS_OK;
}

/* Capture on the measured thread, then publish with external synchronization.
 * Readers may query from another thread. The measured thread must remain alive
 * through the last sample (Linux clocks do not retain the underlying thread).
 * token identifies duplicate captures only while that thread remains alive. */
static inline int chttp_test_thread_cpu_capture(chttp_test_thread_cpu *cpu) {
  if (cpu == NULL) return SALTS_EINVAL;
  if (cpu->token != NULL) return SALTS_EBUSY;
#if defined(_WIN32)
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                       &cpu->handle, THREAD_QUERY_INFORMATION, FALSE, 0))
    return SALTS_EIO;
#elif defined(__APPLE__)
  cpu->handle = mach_thread_self();
  if (cpu->handle == MACH_PORT_NULL) return SALTS_EIO;
#elif defined(__linux__)
  if (pthread_getcpuclockid(pthread_self(), &cpu->clock_id) != 0) return SALTS_EIO;
#else
  return SALTS_ENOTSUP;
#endif
  cpu->token = cmeta_thread_current_token();
  return SALTS_OK;
}

/* Cumulative user + kernel CPU nanoseconds, never wall-clock time. */
static inline int chttp_test_thread_cpu_read(const chttp_test_thread_cpu *cpu,
                                             uint64_t *out) {
  if (cpu == NULL || cpu->token == NULL || out == NULL) return SALTS_EINVAL;
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(cpu->handle, &created, &exited, &kernel, &user)) return SALTS_EIO;
  const uint64_t u = ((uint64_t)user.dwHighDateTime << 32u) | user.dwLowDateTime;
  const uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32u) | kernel.dwLowDateTime;
  return chttp_test_cpu_units_ns(u, k, 100u, out);
#elif defined(__APPLE__)
  thread_basic_info_data_t info;
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  if (thread_info(cpu->handle, THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS)
    return SALTS_EIO;
  if (info.user_time.seconds < 0 || info.system_time.seconds < 0 ||
      info.user_time.microseconds < 0 || info.system_time.microseconds < 0)
    return SALTS_ERANGE;
  uint64_t seconds, micros;
  int status = chttp_test_cpu_units_ns((uint64_t)info.user_time.seconds,
      (uint64_t)info.system_time.seconds, 1000000000u, &seconds);
  if (status != SALTS_OK) return status;
  status = chttp_test_cpu_units_ns((uint64_t)info.user_time.microseconds,
      (uint64_t)info.system_time.microseconds, 1000u, &micros);
  if (status != SALTS_OK || micros > UINT64_MAX - seconds) return SALTS_ERANGE;
  *out = seconds + micros;
  return SALTS_OK;
#elif defined(__linux__)
  struct timespec value;
  if (clock_gettime(cpu->clock_id, &value) != 0) return SALTS_EIO;
  if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L)
    return SALTS_ERANGE;
  uint64_t seconds;
  int status = chttp_test_cpu_units_ns((uint64_t)value.tv_sec, 0u, 1000000000u, &seconds);
  if (status != SALTS_OK || (uint64_t)value.tv_nsec > UINT64_MAX - seconds)
    return SALTS_ERANGE;
  *out = seconds + (uint64_t)value.tv_nsec;
  return SALTS_OK;
#else
  return SALTS_ENOTSUP;
#endif
}

static inline int chttp_test_thread_cpu_release(chttp_test_thread_cpu *cpu) {
  if (cpu == NULL) return SALTS_EINVAL;
  if (cpu->token == NULL) return SALTS_OK;
#if defined(_WIN32)
  if (!CloseHandle(cpu->handle)) return SALTS_EIO;
#elif defined(__APPLE__)
  if (mach_port_deallocate(mach_task_self(), cpu->handle) != KERN_SUCCESS) return SALTS_EIO;
#endif
  *cpu = (chttp_test_thread_cpu){0};
  return SALTS_OK;
}

#endif
