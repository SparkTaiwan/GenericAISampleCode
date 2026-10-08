cmake_minimum_required(VERSION 3.16)
project(GenericAI_Linux LANGUAGES CXX)

# GenericAI wrapper for Linux: the GenericAI.Native sources (ported) plus a
# C++ rewrite of the GenericAI.App host, linked into one `GenericAI` executable.
#
#   cmake -S . -B build -DONNXRUNTIME_ROOT=/opt/onnxruntime-linux-x64-gpu-1.20.1
#   cmake --build build -j
#   -> build/GenericAI/  (GenericAI, GenericAI.Config, models/, libonnxruntime*.so*)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(GAI_ENABLE_ZMQ "Build the ZMQ frame/result planes (libzmq + FFmpeg)" ON)
option(GAI_BUILD_TESTS "Build the native unit tests (ctest)" ON)
set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH
    "Extracted onnxruntime-linux-x64[-gpu]-<ver> release (include/ + lib/)")
set(GAI_MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../GenericAI_ZMQ/native-deps/models" CACHE PATH
    "Folder holding yolox_m_fp16.onnx + LICENSE.md + VERSION.txt")

set(GAI_OUT_DIR "${CMAKE_BINARY_DIR}/GenericAI")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
if(GAI_ENABLE_ZMQ)
    pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
endif()

# ---- header-only deps: system package first, pinned download otherwise ----
include(FetchContent)
find_package(nlohmann_json 3.9 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found on the system; fetching v3.11.3")
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(httplib QUIET)
if(NOT httplib_FOUND)
    message(STATUS "cpp-httplib not found on the system; fetching v0.18.3")
    FetchContent_Declare(httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz)
    set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(httplib)
endif()

# ---- onnxruntime (prebuilt release tarball) ------------------------------
find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}/include/onnxruntime"
    NO_DEFAULT_PATH)
find_library(ORT_LIBRARY onnxruntime PATHS "${ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
    message(FATAL_ERROR
        "onnxruntime not found under ONNXRUNTIME_ROOT=${ONNXRUNTIME_ROOT}. "
        "Run scripts/fetch_onnxruntime.sh or pass -DONNXRUNTIME_ROOT=<extracted release>.")
endif()
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ORT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}")

# ---- native (GenericAI.Native port) ---------------------------------------
set(GAI_NATIVE_SOURCES
    native/buffer_pool.cpp
    native/channel_pipeline.cpp
    native/detector_factory.cpp
    native/detector_motion.cpp
    native/detector_person.cpp
    native/exports.cpp
    native/host_log.cpp
    native/mmf_reader.cpp
    native/param_snapshot.cpp
    native/shared_detector_scheduler.cpp
    native/timing_recorder.cpp)
if(GAI_ENABLE_ZMQ)
    list(APPEND GAI_NATIVE_SOURCES native/nal_decoder.cpp native/zmq_frame_receiver.cpp)
endif()

add_library(gai_native STATIC ${GAI_NATIVE_SOURCES})
target_include_directories(gai_native PUBLIC native)
target_compile_definitions(gai_native PUBLIC $<$<BOOL:${GAI_ENABLE_ZMQ}>:USE_ZMQ>)
target_link_libraries(gai_native PUBLIC onnxruntime Threads::Threads rt)
if(GAI_ENABLE_ZMQ)
    target_link_libraries(gai_native PUBLIC PkgConfig::ZMQ PkgConfig::FFMPEG)
endif()
target_compile_options(gai_native PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- host (GenericAI.App rewrite) -----------------------------------------
add_executable(GenericAI
    host/channel_handle.cpp
    host/command_line_args.cpp
    host/console_log.cpp
    host/file_logger.cpp
    host/http_control_server.cpp
    host/main.cpp
    host/pipeline.cpp
    host/settings_schema.cpp)
target_link_libraries(GenericAI PRIVATE gai_native nlohmann_json::nlohmann_json httplib::httplib
    PkgConfig::TURBOJPEG)
target_compile_options(GenericAI PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(GenericAI PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${GAI_OUT_DIR}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

# ---- deployable folder: runtime files next to the executable --------------
# Only what the loader needs: the SONAME (copy resolves the symlink chain) and,
# for the GPU package, the CUDA provider + its shim. TensorRT is not used.
set(ORT_RUNTIME_LIBS "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so.1")
foreach(_p libonnxruntime_providers_shared.so libonnxruntime_providers_cuda.so)
    if(EXISTS "${ONNXRUNTIME_ROOT}/lib/${_p}")
        list(APPEND ORT_RUNTIME_LIBS "${ONNXRUNTIME_ROOT}/lib/${_p}")
    endif()
endforeach()
add_custom_command(TARGET GenericAI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GAI_OUT_DIR}/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_RUNTIME_LIBS} "${GAI_OUT_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/GenericAI.Config" "${GAI_OUT_DIR}/GenericAI.Config"
    VERBATIM)
if(EXISTS "${GAI_MODEL_DIR}/yolox_m_fp16.onnx")
    add_custom_command(TARGET GenericAI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAI_MODEL_DIR}/yolox_m_fp16.onnx" "${GAI_OUT_DIR}/models/yolo.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAI_MODEL_DIR}/LICENSE.md" "${GAI_OUT_DIR}/models/LICENSE.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAI_MODEL_DIR}/VERSION.txt" "${GAI_OUT_DIR}/models/VERSION.txt"
        VERBATIM)
else()
    message(WARNING "Model not found at ${GAI_MODEL_DIR}/yolox_m_fp16.onnx; "
                    "detector=objectdetection will start degraded until models/yolo.onnx is copied in.")
endif()

install(DIRECTORY "${GAI_OUT_DIR}/" DESTINATION GenericAI USE_SOURCE_PERMISSIONS)

# ---- tests (GenericAI.Native.Tests, unchanged sources) --------------------
if(GAI_BUILD_TESTS)
    enable_testing()
    add_executable(gai_native_tests
        tests/test_main.cpp
        tests/test_param_snapshot.cpp
        tests/test_roi_geometry.cpp
        tests/test_yolox_geometry.cpp
        native/param_snapshot.cpp)
    target_include_directories(gai_native_tests PRIVATE tests native)
    add_test(NAME gai_native_tests COMMAND gai_native_tests)
endif()
