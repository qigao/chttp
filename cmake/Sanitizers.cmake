if(ENABLE_TSAN AND
   (ENABLE_ASAN OR ENABLE_UBSAN))
  message(FATAL_ERROR "ThreadSanitizer cannot be combined with ASan/UBSan")
endif()

if(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CHttp sanitizer profiles require Clang/LLVM")
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
            "CHttp sanitizer profiles are qualified on Linux only")
  endif()

  set(_sanitizers)
  if(ENABLE_ASAN)
    list(APPEND _sanitizers address)
  endif()
  if(ENABLE_UBSAN)
    list(APPEND _sanitizers undefined)
  endif()
  if(ENABLE_TSAN)
    list(APPEND _sanitizers thread)
  endif()
  list(JOIN _sanitizers "," _sanitizer_list)
  add_compile_options(
    "-fsanitize=${_sanitizer_list}"
    -fno-omit-frame-pointer)
  add_link_options("-fsanitize=${_sanitizer_list}")
endif()
