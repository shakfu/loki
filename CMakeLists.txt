cmake_minimum_required(VERSION 3.28)
project(loki VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(LOKI_BUILD_SHARED "Build libloki as a shared library" OFF)
option(BUILD_TESTS "Build unit tests" ON)
option(LOKI_ENABLE_HTTP "Enable async HTTP support (requires libcurl)" OFF)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Lua REQUIRED)
find_package(Threads REQUIRED)

# Optional: libcurl for async HTTP
if(LOKI_ENABLE_HTTP)
    find_package(CURL REQUIRED)
endif()

# Find libuv for async support
pkg_check_modules(LIBUV REQUIRED libuv)

# Linenoise line editing library (with tree-sitter syntax highlighting)
# Built from thirdparty/linenoise
add_subdirectory(thirdparty)

set(LOKI_LIBRARY_TYPE STATIC)
if (LOKI_BUILD_SHARED)
    set(LOKI_LIBRARY_TYPE SHARED)
endif()

# Loki core library - new modular architecture
set(LOKI_SOURCES
    src/core.c
    src/buffers.c
    src/terminal.c
    src/renderer.c
    src/event.c
    src/modal.c
    src/selection.c
    src/syntax.c
    src/languages.c
    src/search.c
    src/undo.c
    src/indent.c
    src/json.c
    src/serialize.c
    src/async_queue.c
    src/command.c
    src/command/basic.c
    src/command/file.c
    src/command/goto.c
    src/command/substitute.c
    src/editor.c
    src/lua.c
    src/lang_bridge.c
    src/session.c
    src/host.c
    src/cli.c
    src/jsonrpc.c
    src/repl_helpers.c
    src/repl.c
    src/repl_linenoise.c
    src/treesitter.c
    src/loki_markdown.c
)

# Optional HTTP support
if(LOKI_ENABLE_HTTP)
    list(APPEND LOKI_SOURCES src/http.c)
endif()

add_library(libloki ${LOKI_LIBRARY_TYPE} ${LOKI_SOURCES})

target_include_directories(libloki
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIR}
        ${LIBUV_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cmark/src
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/linenoise/include
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/linenoise/thirdparty/tree-sitter/lib/include
)

# Add CURL include directories if HTTP enabled
if(LOKI_ENABLE_HTTP)
    target_include_directories(libloki PUBLIC ${CURL_INCLUDE_DIRS})
endif()

target_compile_options(libloki PRIVATE -Wall -Wextra -pedantic)

target_link_directories(libloki PUBLIC ${LIBUV_LIBRARY_DIRS})

target_link_libraries(libloki
    PUBLIC
        ${LUA_LIBRARIES}
        Threads::Threads
        ${CMAKE_DL_LIBS}
        ${LIBUV_LIBRARIES}
        cmark
        linenoise
        linenoise-theme
        linenoise-highlight-lua
        linenoise-highlight-python
        linenoise-highlight-scheme
        linenoise-highlight-haskell
        linenoise-highlight-markdown
        tree-sitter
        tree-sitter-lua
        tree-sitter-python
        tree-sitter-scheme
        tree-sitter-haskell
        tree-sitter-markdown
)

# Link CURL if HTTP enabled
if(LOKI_ENABLE_HTTP)
    target_link_libraries(libloki PUBLIC CURL::libcurl)
    target_compile_definitions(libloki PUBLIC LOKI_ENABLE_HTTP=1)
endif()

if (NOT MSVC)
    target_link_libraries(libloki PUBLIC m)
endif()

# Linenoise compile definition for REPL
target_compile_definitions(libloki PUBLIC LOKI_USE_LINENOISE=1)

# Main executable
add_executable(loki src/main.c)
target_compile_options(loki PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(loki PRIVATE libloki)

message(STATUS "loki will use linenoise for line editing (with tree-sitter syntax highlighting)")

if(LOKI_ENABLE_HTTP)
    set(HTTP_STATUS "enabled")
else()
    set(HTTP_STATUS "disabled - use -DLOKI_ENABLE_HTTP=ON to enable")
endif()

add_custom_target(show-config
    COMMAND ${CMAKE_COMMAND} -E echo "Lua include: ${LUA_INCLUDE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "Lua libraries: ${LUA_LIBRARIES}"
    COMMAND ${CMAKE_COMMAND} -E echo "libuv include: ${LIBUV_INCLUDE_DIRS}"
    COMMAND ${CMAKE_COMMAND} -E echo "libuv libraries: ${LIBUV_LIBRARIES}"
    COMMAND ${CMAKE_COMMAND} -E echo "Line editing: linenoise with tree-sitter"
    COMMAND ${CMAKE_COMMAND} -E echo "Async HTTP: ${HTTP_STATUS}"
)

# Tests
if(BUILD_TESTS)
    enable_testing()

    add_test(NAME loki_version COMMAND $<TARGET_FILE:loki> --version)

    # Test framework library
    add_library(test_framework STATIC tests/test_framework.c)
    target_include_directories(test_framework PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # Core editor tests (new modular structure)
    set(LOKI_TESTS
        test_core
        test_buffers
        test_modal
        test_terminal
        test_syntax
        test_search
        test_selection
        test_undo
        test_indent
        test_command
        test_serialize
        test_lua_api
        test_lang_registration
        test_file_io
        test_row_operations
        test_async_queue
    )

    foreach(test_name ${LOKI_TESTS})
        add_executable(${test_name} tests/loki/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${LUA_INCLUDE_DIR}
        )
        target_link_libraries(${test_name} PRIVATE libloki test_framework)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    # HTTP tests (only if HTTP enabled)
    if(LOKI_ENABLE_HTTP)
        add_executable(test_http_security tests/test_http_security.c)
        target_include_directories(test_http_security PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${LUA_INCLUDE_DIR}
        )
        target_link_libraries(test_http_security PRIVATE libloki test_framework)
        add_test(NAME test_http_security COMMAND test_http_security)

        add_executable(test_http_simple tests/test_http_simple.c)
        target_include_directories(test_http_simple PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${LUA_INCLUDE_DIR}
        )
        target_link_libraries(test_http_simple PRIVATE libloki test_framework)
        add_test(NAME test_http_simple COMMAND test_http_simple)
    endif()

    # Interactive linenoise REPL test (not run automatically)
    add_executable(test_linenoise_repl tests/test_linenoise_repl.c)
    target_include_directories(test_linenoise_repl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/linenoise/include
    )
    target_link_libraries(test_linenoise_repl PRIVATE
        linenoise
        linenoise-theme
        linenoise-highlight-lua
        tree-sitter
        tree-sitter-lua
    )
    target_compile_definitions(test_linenoise_repl PRIVATE LOKI_USE_LINENOISE=1)
endif()

# Install
install(TARGETS loki RUNTIME DESTINATION bin)
install(DIRECTORY .loki DESTINATION share/loki)
