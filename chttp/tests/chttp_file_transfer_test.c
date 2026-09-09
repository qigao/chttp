#include "chttp_file_transfer.h"

#include "tinytest.h"

#include <salts/error_codes.h>
#include <salts/thread.h>

#include <string.h>

static cflow_io_native_backend_kind chttp_file_test_backend(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_IO_URING;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static int chttp_file_test_drive(cflow_io_file_runtime *runtime, chttp_file_transfer *transfer) {
  size_t attempts;
  for (attempts = 0u; attempts < 100000u; ++attempts) {
    size_t progressed = 0u;
    int status = cflow_io_file_runtime_run_ready(runtime, 32u, &progressed);
    if (status != SALTS_OK) return status;
    if (chttp_file_transfer_ready(transfer)) return SALTS_OK;
    if (progressed == 0u) salts_thread_yield();
  }
  return SALTS_ETIMEDOUT;
}

spec("CHTTP asynchronous file transfer") {
  it("waits for an asynchronous read before publishing a bounded chunk") {
    static const unsigned char payload[] = "asynchronous-file-source";
    char *path = tt_make_temp_file("chttp-file-transfer", ".bin");
    cflow_io_file_runtime runtime = {0};
    cflow_io_file_runtime_config runtime_config = {.backend_kind = chttp_file_test_backend(),
                                                   .file_capacity = 1u,
                                                   .request_capacity = 1u,
                                                   .command_capacity = 2u,
                                                   .completion_batch_capacity = 1u};
    chttp_file_transfer transfer = {0};
    unsigned char output[sizeof(payload)] = {0};
    size_t produced = 99u;
    chttp_file_source_result result;
    int status;

    check_not_null(path);
    check_equal(tt_write_file(path, payload, sizeof(payload) - 1u), 0);
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
    check_equal(chttp_file_transfer_open_read(&transfer, &runtime, path, sizeof(payload) - 1u, 8u,
                                              NULL, NULL),
                SALTS_OK);

    result = chttp_file_transfer_read(&transfer, output, sizeof(output), &produced);
    check_equal(result, CHTTP_FILE_SOURCE_WAIT);
    check_equal(produced, 0u);
    check_equal(chttp_file_test_drive(&runtime, &transfer), SALTS_OK);

    result = chttp_file_transfer_read(&transfer, output, sizeof(output), &produced);
    check_equal(result, CHTTP_FILE_SOURCE_DATA);
    check_equal(produced, 8u);
    check_equal(memcmp(output, payload, produced), 0);
    check_equal(chttp_file_transfer_transferred(&transfer), produced);

    check_equal(chttp_file_transfer_close(&transfer), SALTS_OK);
    check_equal(chttp_file_transfer_destroy(&transfer), SALTS_OK);
    check_equal(cflow_io_file_runtime_close(&runtime), SALTS_OK);
    check_true(cflow_io_file_runtime_is_quiescent(&runtime));
    check_equal(cflow_io_file_runtime_destroy(&runtime), SALTS_OK);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }
}
