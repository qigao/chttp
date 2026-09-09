#include "chttp_file_sink.h"

#include "tinytest.h"

#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdlib.h>
#include <string.h>

static cflow_io_native_backend_kind chttp_file_sink_test_backend(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_IO_URING;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static int chttp_file_sink_test_drive(cflow_io_file_runtime *runtime,
                                      chttp_file_sink_transfer *transfer) {
  size_t attempts;
  for (attempts = 0u; attempts < 100000u; ++attempts) {
    size_t progressed = 0u;
    int status = cflow_io_file_runtime_run_ready(runtime, 32u, &progressed);
    if (status != SALTS_OK) return status;
    if (chttp_file_sink_transfer_ready(transfer)) return SALTS_OK;
    if (progressed == 0u) salts_thread_yield();
  }
  return SALTS_ETIMEDOUT;
}

spec("CHTTP asynchronous file sink") {
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
#if !defined(_WIN32)
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

    if (cflow_io_file_operation_supported(&transfer.file, CFLOW_IO_NATIVE_FILE_FLUSH)) {
      check_equal(chttp_file_sink_transfer_flush(&transfer), CHTTP_FILE_SINK_WAIT);
      check_equal(chttp_file_sink_test_drive(&runtime, &transfer), SALTS_OK);
      check_equal(chttp_file_sink_transfer_flush(&transfer), CHTTP_FILE_SINK_READY);
    }
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
