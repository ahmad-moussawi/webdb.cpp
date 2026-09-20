cmake_minimum_required(VERSION 3.20)
project(webdb LANGUAGES CXX)

option(WEBDB_WASM_SIZE_OPTIMIZED "Optimize the Emscripten build for output size" OFF)

file(REAL_PATH "${CMAKE_SOURCE_DIR}/dist" WEBDB_DIST_DIR)
file(REAL_PATH "${CMAKE_BINARY_DIR}" WEBDB_BINARY_DIR)
cmake_path(IS_PREFIX WEBDB_DIST_DIR "${WEBDB_BINARY_DIR}" NORMALIZE WEBDB_BINARY_UNDER_DIST)
if (NOT WEBDB_BINARY_UNDER_DIST)
    message(FATAL_ERROR "Build files must be placed under ${WEBDB_DIST_DIR}. Use: cmake -S . -B dist/native")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warning flags for high code quality
if (MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

# Core database engine library (independent of any web/WASM dependencies)
add_library(webdb_core STATIC
    src/engine.cpp
    src/common/checksum.cpp
    src/storage/buffer_pool_manager.cpp
    src/storage/master_page.cpp
    src/storage/operation_scheduler.cpp
    src/storage/slotted_page.cpp
    src/storage/tuple.cpp
    src/storage/value.cpp
    src/storage/table_heap.cpp
)

target_include_directories(webdb_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

# ------------------------------------------------------------------------------
# Build Targets: Emscripten (WASM) vs Native
# ------------------------------------------------------------------------------
if (EMSCRIPTEN)
    message(STATUS "Configuring build for WebAssembly (Emscripten)")

    if (WEBDB_WASM_SIZE_OPTIMIZED)
        set(WEBDB_WASM_OPT_FLAGS -Oz -flto)
    else()
        set(WEBDB_WASM_OPT_FLAGS -O3)
    endif()

    target_compile_options(webdb_core PRIVATE ${WEBDB_WASM_OPT_FLAGS})

    add_executable(webdb_wasm
        wasm/bindings.cpp
    )

    target_link_libraries(webdb_wasm PRIVATE webdb_core)

    # Emscripten compiler and linker flags:
    # -O3: Aggressive optimization
    # --bind: Embind support for C++ <-> JS bindings
    # ALLOW_MEMORY_GROWTH=1: Dynamic linear memory resizing
    # MODULARIZE=1: Wraps output in a module factory function
    # EXPORT_ES6=1: ES6 module output syntax
    # EXPORT_NAME="createWebDB": JavaScript factory function name
    # NO_DISABLE_EXCEPTION_CATCHING: Clean exception handling support across boundaries
    target_compile_options(webdb_wasm PRIVATE ${WEBDB_WASM_OPT_FLAGS})
    target_link_options(webdb_wasm PRIVATE
        ${WEBDB_WASM_OPT_FLAGS}
        --bind
        -sALLOW_MEMORY_GROWTH=1
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createWebDB
        -sNO_DISABLE_EXCEPTION_CATCHING
        -sFILESYSTEM=0
    )

    # Output artifact name: webdb.js (which generates webdb.wasm alongside it)
    set_target_properties(webdb_wasm PROPERTIES
        OUTPUT_NAME "webdb"
        SUFFIX ".js"
    )
else()
    message(STATUS "Configuring build for Native Platform")

    # Native CLI runner for testing and local development
    add_executable(webdb_cli
        src/main.cpp
    )
    target_link_libraries(webdb_cli PRIVATE webdb_core)

    # Storage Engine Unit Tests (Phase 1)
    enable_testing()

    function(add_storage_test target source definition test_name)
        add_executable(${target}
            ${source}
            tests/test_main.cpp
        )
        target_compile_definitions(${target} PRIVATE ${definition})
        target_link_libraries(${target} PRIVATE webdb_core)
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    add_storage_test(test_checksum tests/test_checksum.cpp WEBDB_TEST_CHECKSUMS checksum)
    add_storage_test(test_master_page tests/test_master_page.cpp WEBDB_TEST_MASTER_PAGE master_page)
    add_storage_test(test_slotted_page tests/test_slotted_page.cpp WEBDB_TEST_SLOTTED_PAGE slotted_page)
    add_storage_test(test_async_page_store tests/test_async_page_store.cpp WEBDB_TEST_ASYNC_PAGE_STORE async_page_store)
    add_storage_test(test_operation_scheduler tests/test_operation_scheduler.cpp WEBDB_TEST_OPERATION_SCHEDULER operation_scheduler)
    add_storage_test(test_buffer_pool_manager tests/test_buffer_pool_manager.cpp WEBDB_TEST_BUFFER_POOL_MANAGER buffer_pool_manager)
    add_storage_test(test_tuple tests/test_tuple.cpp WEBDB_TEST_TUPLE tuple)
    add_storage_test(test_table_heap tests/test_table_heap.cpp WEBDB_TEST_TABLE_HEAP table_heap)
endif()
