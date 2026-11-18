#include "chttp_file_sink.h"
#include "chttp_file_transfer.h"

#include "tinytest.h"

#include <core/clock.h>
#include <core/error_codes.h>
#include <threads/threads.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static cflow_io_native_backend_kind chttp_file_sink_test_backend(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_IO_URING;
#elif defined(__APPLE__) && TARGET_OS_OSX
  return CFLOW_IO_NATIVE_DARWIN_AIO;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static const uint64_t CHTTP_FILE_SINK_TEST_TIMEOUT_NS = UINT64_C(5000000000);

static int chttp_file_sink_test_drive(cflow_io_file_runtime *runtime,
                                      chttp_file_sink_transfer *transfer) {
  const uint64_t started = hrtime();
  for (;;) {
    size_t progressed = 0u;
    int status = cflow_io_file_runtime_run_ready(runtime, 32u, &progressed);
    if (status != SALTS_OK) return status;
    if (chttp_file_sink_transfer_ready(transfer)) return SALTS_OK;
    if (hrtime() - started >= CHTTP_FILE_SINK_TEST_TIMEOUT_NS)
      return SALTS_ETIMEDOUT;
    if (progressed == 0u) sync_thread_yield();
  }
}

spec("CHTTP asynchronous file sink") {
  it("admits concurrent reads and two writes in one runtime") {
    static const char payload[] = "shared file leases";
    char *paths[3] = {tt_make_temp_file("chttp-mixed-input", ".bin"),
        tt_make_temp_file("chttp-mixed-output", ".bin"), tt_make_temp_file("chttp-mixed-output", ".bin")};
    for (size_t i = 0u; i < 3u; ++i) check_not_null(paths[i]);
    check_equal(tt_write_file(paths[0], payload, sizeof(payload)-1u), 0);
    cflow_io_file_runtime runtime = {0};
    const cflow_io_file_runtime_config config = {.backend_kind = chttp_file_sink_test_backend(),
        .file_capacity = 3u, .request_capacity = 3u, .command_capacity = 6u,
        .completion_batch_capacity = 3u};
    check_equal(cflow_io_file_runtime_init(&runtime, &config), SALTS_OK);
    chttp_file_transfer source = {0};
    chttp_file_sink_transfer sinks[2] = {0};
    check_equal(chttp_file_transfer_open_read_range(&source, &runtime, paths[0], 0u,
        sizeof(payload)-1u, sizeof(payload)), SALTS_OK);
    char output[sizeof(payload)] = {0}; size_t size = 0u;
    check_equal(chttp_file_transfer_read(&source, output, sizeof(output), &size), CHTTP_FILE_SOURCE_WAIT);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(chttp_file_sink_transfer_open(&sinks[i], &runtime, paths[i+1u],
          sizeof(payload), NULL, NULL), SALTS_OK);
      check_equal(chttp_file_sink_transfer_write(&sinks[i], payload, sizeof(payload)-1u), CHTTP_FILE_SINK_WAIT);
    }
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(chttp_file_sink_test_drive(&runtime, &sinks[i]), SALTS_OK);
      check_equal(chttp_file_sink_transfer_advance(&sinks[i]), CHTTP_FILE_SINK_READY);
    }
    chttp_file_source_result result;
    const uint64_t deadline = hrtime()+CHTTP_FILE_SINK_TEST_TIMEOUT_NS;
    do {
      size_t progressed;
      check_equal(cflow_io_file_runtime_run_ready(&runtime, 32u, &progressed), SALTS_OK);
      result = chttp_file_transfer_read(&source, output, sizeof(output), &size);
    } while (result == CHTTP_FILE_SOURCE_WAIT && hrtime() < deadline);
    check_equal(result, CHTTP_FILE_SOURCE_DATA); check_equal(size, sizeof(payload)-1u);
    check_equal(memcmp(output, payload, size), 0);
    for (size_t i = 0u; i < 2u; ++i) {
      check_true(cflow_io_file_operation_supported(&sinks[i].file, CFLOW_IO_NATIVE_FILE_FLUSH));
      check_equal(chttp_file_sink_transfer_flush(&sinks[i]), CHTTP_FILE_SINK_WAIT);
    }
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(chttp_file_sink_test_drive(&runtime, &sinks[i]), SALTS_OK);
      check_equal(chttp_file_sink_transfer_flush(&sinks[i]), CHTTP_FILE_SINK_READY);
    }
    check_equal(chttp_file_transfer_drain_destroy(&source, &runtime), SALTS_OK);
    for (size_t i = 0u; i < 2u; ++i)
      check_equal(chttp_file_sink_transfer_drain_destroy(&sinks[i], &runtime), SALTS_OK);
    check_equal(cflow_io_file_runtime_close(&runtime), SALTS_OK);
    check_equal(cflow_io_file_runtime_destroy(&runtime), SALTS_OK);
    for (size_t i = 1u; i < 3u; ++i) {
      size_t written;
      char *contents = tt_read_file(paths[i], &written); check_not_null(contents);
      check_equal(written, sizeof(payload)-1u); check_equal(memcmp(contents, payload, written), 0);
      free(contents);
    }
    for (size_t i = 0u; i < 3u; ++i) { check_equal(tt_remove_file(paths[i]), 0); free(paths[i]); }
  }
  it("owns response bytes until asynchronous write and flush completion") {
    static const unsigned char payload[] = "asynchronous-file-sink";
    char *path = tt_make_temp_file("chttp-file-sink", ".bin");
    cflow_io_file_runtime runtime = {0};
    cflow_io_file_runtime_config runtime_config = {.backend_kind = chttp_file_sink_test_backend(),
                                                   .file_capacity = 1u,
                                                   .request_capacity = 1u,
                                                   .command_capacity = 2u,
                                                   .completion_batch_capacity = 1u};
    chttp_file_sink_transfer transfer = {0};
    char *contents;
    size_t contents_size = 0u;
    int status;

    check_not_null(path);
    status = cflow_io_file_runtime_init(&runtime, &runtime_config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    check_equal(
        chttp_file_sink_transfer_open(&transfer, &runtime, path, sizeof(payload), NULL, NULL),
        SALTS_OK);
    check_equal(chttp_file_sink_transfer_write(&transfer, payload, sizeof(payload) - 1u),
                CHTTP_FILE_SINK_WAIT);
    check_equal(chttp_file_sink_transfer_destroy(&transfer), SALTS_EBUSY);
    check_equal(chttp_file_sink_test_drive(&runtime, &transfer), SALTS_OK);
    check_equal(chttp_file_sink_transfer_advance(&transfer), CHTTP_FILE_SINK_READY);
    check_equal(chttp_file_sink_transfer_transferred(&transfer), sizeof(payload) - 1u);

    check_true(cflow_io_file_operation_supported(&transfer.file, CFLOW_IO_NATIVE_FILE_FLUSH));
    check_equal(chttp_file_sink_transfer_flush(&transfer), CHTTP_FILE_SINK_WAIT);
    check_equal(chttp_file_sink_test_drive(&runtime, &transfer), SALTS_OK);
    check_equal(chttp_file_sink_transfer_flush(&transfer), CHTTP_FILE_SINK_READY);
    check_equal(chttp_file_sink_transfer_drain_destroy(&transfer, &runtime), SALTS_OK);
    check_equal(cflow_io_file_runtime_close(&runtime), SALTS_OK);
    check_true(cflow_io_file_runtime_is_quiescent(&runtime));
    check_equal(cflow_io_file_runtime_destroy(&runtime), SALTS_OK);

    contents = tt_read_file(path, &contents_size);
    check_not_null(contents);
    check_equal(contents_size, sizeof(payload) - 1u);
    check_equal(memcmp(contents, payload, contents_size), 0);
    free(contents);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }
}
