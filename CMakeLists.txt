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

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Lua REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Find libuv for async support
pkg_check_modules(LIBUV REQUIRED libuv)

# Line editing library detection (editline preferred over readline due to BSD license)
set(LINEEDIT_FOUND FALSE)
set(LINEEDIT_LIBRARY "")
set(LINEEDIT_INCLUDE_DIR "")
set(LINEEDIT_TYPE "none")

# Try editline first (BSD license, more permissive than GPL readline)
find_path(EDITLINE_INCLUDE_DIR
    NAMES histedit.h
    PATHS /usr/include /usr/local/include /opt/local/include
)
find_library(EDITLINE_LIBRARY
    NAMES edit
    PATHS /usr/lib /usr/local/lib /opt/local/lib
)

if (EDITLINE_INCLUDE_DIR AND EDITLINE_LIBRARY)
    set(LINEEDIT_FOUND TRUE)
    set(LINEEDIT_LIBRARY ${EDITLINE_LIBRARY})
    set(LINEEDIT_INCLUDE_DIR ${EDITLINE_INCLUDE_DIR})
    set(LINEEDIT_TYPE "editline")
    message(STATUS "Found editline: ${EDITLINE_LIBRARY}")
else()
    # Try readline as fallback
    find_package(Readline QUIET)

    if (NOT Readline_FOUND)
        # Try Homebrew readline on macOS
        execute_process(
            COMMAND brew --prefix readline
            OUTPUT_VARIABLE BREW_READLINE_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if (BREW_READLINE_PREFIX)
            find_path(READLINE_INCLUDE_DIR
                NAMES readline/readline.h
                HINTS
                    ${BREW_READLINE_PREFIX}/include
                    ${BREW_READLINE_PREFIX}/include/readline
            )
            find_library(READLINE_LIBRARY
                NAMES readline
                HINTS ${BREW_READLINE_PREFIX}/lib
            )
            if (READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
                set(Readline_FOUND ON)
                set(Readline_INCLUDE_DIR ${READLINE_INCLUDE_DIR})
                set(Readline_LIBRARY ${READLINE_LIBRARY})
            endif()
        endif()
    endif()

    if (Readline_FOUND)
        set(LINEEDIT_FOUND TRUE)
        set(LINEEDIT_LIBRARY ${Readline_LIBRARY})
        set(LINEEDIT_INCLUDE_DIR ${Readline_INCLUDE_DIR})
        set(LINEEDIT_TYPE "readline")
        message(STATUS "Found readline: ${Readline_LIBRARY}")
    endif()
endif()

if (NOT LINEEDIT_FOUND)
    message(STATUS "Line editing library not found; loki-repl will use basic fallback (no history/completion)")
    message(STATUS "  To enable features: brew install readline (or apt-get install libedit-dev on Linux)")
endif()

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
    src/loki_markdown.c
    src/http.c
)

add_library(libloki ${LOKI_LIBRARY_TYPE} ${LOKI_SOURCES})

target_include_directories(libloki
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LUA_INCLUDE_DIR}
        ${CURL_INCLUDE_DIRS}
        ${LIBUV_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cmark/src
)

target_compile_options(libloki PRIVATE -Wall -Wextra -pedantic)

target_link_directories(libloki PUBLIC ${LIBUV_LIBRARY_DIRS})

target_link_libraries(libloki
    PUBLIC
        ${LUA_LIBRARIES}
        CURL::libcurl
        Threads::Threads
        ${CMAKE_DL_LIBS}
        ${LIBUV_LIBRARIES}
        cmark
)

if (NOT MSVC)
    target_link_libraries(libloki PUBLIC m)
endif()

# Main executable
add_executable(loki src/main.c)
target_compile_options(loki PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(loki PRIVATE libloki)

if (LINEEDIT_FOUND)
    target_include_directories(loki PRIVATE ${LINEEDIT_INCLUDE_DIR})
    target_link_libraries(loki PRIVATE ${LINEEDIT_LIBRARY})
    if (LINEEDIT_TYPE STREQUAL "editline")
        target_compile_definitions(loki PRIVATE LOKI_HAVE_EDITLINE=1)
    else()
        target_compile_definitions(loki PRIVATE LOKI_HAVE_READLINE=1)
    endif()
    message(STATUS "loki will use ${LINEEDIT_TYPE} for line editing (history, completion)")
endif()

add_custom_target(show-config
    COMMAND ${CMAKE_COMMAND} -E echo "Lua include: ${LUA_INCLUDE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "Lua libraries: ${LUA_LIBRARIES}"
    COMMAND ${CMAKE_COMMAND} -E echo "libcurl include: ${CURL_INCLUDE_DIRS}"
    COMMAND ${CMAKE_COMMAND} -E echo "libcurl libraries: ${CURL_LIBRARIES}"
    COMMAND ${CMAKE_COMMAND} -E echo "libuv include: ${LIBUV_INCLUDE_DIRS}"
    COMMAND ${CMAKE_COMMAND} -E echo "libuv libraries: ${LIBUV_LIBRARIES}"
    COMMAND ${CMAKE_COMMAND} -E echo "Line editing: ${LINEEDIT_TYPE}"
    COMMAND ${CMAKE_COMMAND} -E echo "Line edit library: ${LINEEDIT_LIBRARY}"
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

    # HTTP tests
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

# Install
install(TARGETS loki RUNTIME DESTINATION bin)
install(DIRECTORY .loki DESTINATION share/loki)
