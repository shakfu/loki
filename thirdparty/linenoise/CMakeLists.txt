cmake_minimum_required(VERSION 3.10)
project(linenoise VERSION 2.0.0 LANGUAGES C)

# C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings (only apply if not a subproject)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    if(MSVC)
        add_compile_options(/W4)
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    else()
        add_compile_options(-Wall -Wextra -pedantic)
    endif()
endif()

# Tree-sitter support option
option(WITH_TREESITTER "Build with tree-sitter syntax highlighting support" ON)

# Platform-specific terminal implementation
if(WIN32)
    set(TERMINAL_SRC src/internal/terminal_windows.c)
else()
    set(TERMINAL_SRC src/internal/terminal_posix.c)
endif()

# Module source files
set(MODULE_SOURCES
    src/internal/utf8.c
    src/internal/history.c
    src/internal/completion.c
    src/internal/keyparser.c
    src/internal/render.c
)

# Core library source files
set(LINENOISE_SOURCES
    src/linenoise.c
    ${MODULE_SOURCES}
    ${TERMINAL_SRC}
)

# Linenoise library (static by default)
add_library(linenoise STATIC ${LINENOISE_SOURCES})

target_include_directories(linenoise
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
)

# Silence warnings in linenoise (it has pedantic issues)
if(NOT MSVC)
    target_compile_options(linenoise PRIVATE -Wno-unused-parameter)
endif()

# Tree-sitter based syntax highlighting libraries (always build when WITH_TREESITTER)
if(WITH_TREESITTER)
    # Tree-sitter library sources
    set(TREESITTER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter)
    set(TREESITTER_LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-lua)
    set(TREESITTER_PYTHON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-python)
    set(TREESITTER_SCHEME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-scheme)
    set(TREESITTER_HASKELL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-haskell)
    set(TREESITTER_FORTH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-forth)
    set(TREESITTER_FAUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-faust)
    set(TREESITTER_CHUCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-chuck)
    set(TREESITTER_MARKDOWN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tree-sitter-markdown)

    # Build tree-sitter as a static library
    add_library(tree-sitter STATIC
        ${TREESITTER_DIR}/lib/src/lib.c
    )
    target_include_directories(tree-sitter
        PUBLIC
            ${TREESITTER_DIR}/lib/include
        PRIVATE
            ${TREESITTER_DIR}/lib/src
    )
    if(MSVC)
        target_compile_options(tree-sitter PRIVATE /w)
    else()
        target_compile_options(tree-sitter PRIVATE -w)
        target_compile_definitions(tree-sitter PRIVATE _DEFAULT_SOURCE)
    endif()

    # Syntax highlighting theme library
    add_library(linenoise-theme STATIC
        src/syntax/theme.c
    )
    target_include_directories(linenoise-theme
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    # Build tree-sitter-lua grammar
    add_library(tree-sitter-lua STATIC
        ${TREESITTER_LUA_DIR}/src/parser.c
        ${TREESITTER_LUA_DIR}/src/scanner.c
    )
    target_include_directories(tree-sitter-lua
        PRIVATE
            ${TREESITTER_LUA_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-lua PRIVATE /w)
    else()
        target_compile_options(tree-sitter-lua PRIVATE -w)
    endif()

    # Lua highlighter module
    add_library(linenoise-highlight-lua STATIC
        src/syntax/lua.c
    )
    target_include_directories(linenoise-highlight-lua
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-lua
        PRIVATE linenoise-theme tree-sitter tree-sitter-lua
    )

    # Build tree-sitter-python grammar
    add_library(tree-sitter-python STATIC
        ${TREESITTER_PYTHON_DIR}/src/parser.c
        ${TREESITTER_PYTHON_DIR}/src/scanner.c
    )
    target_include_directories(tree-sitter-python
        PRIVATE
            ${TREESITTER_PYTHON_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-python PRIVATE /w)
    else()
        target_compile_options(tree-sitter-python PRIVATE -w)
    endif()

    # Python highlighter module
    add_library(linenoise-highlight-python STATIC
        src/syntax/python.c
    )
    target_include_directories(linenoise-highlight-python
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-python
        PRIVATE linenoise-theme tree-sitter tree-sitter-python
    )

    # Build tree-sitter-scheme grammar
    add_library(tree-sitter-scheme STATIC
        ${TREESITTER_SCHEME_DIR}/src/parser.c
    )
    target_include_directories(tree-sitter-scheme
        PRIVATE
            ${TREESITTER_SCHEME_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-scheme PRIVATE /w)
    else()
        target_compile_options(tree-sitter-scheme PRIVATE -w)
    endif()

    # Scheme highlighter module
    add_library(linenoise-highlight-scheme STATIC
        src/syntax/scheme.c
    )
    target_include_directories(linenoise-highlight-scheme
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-scheme
        PRIVATE linenoise-theme tree-sitter tree-sitter-scheme
    )

    # Build tree-sitter-haskell grammar
    add_library(tree-sitter-haskell STATIC
        ${TREESITTER_HASKELL_DIR}/src/parser.c
        ${TREESITTER_HASKELL_DIR}/src/scanner.c
    )
    target_include_directories(tree-sitter-haskell
        PRIVATE
            ${TREESITTER_HASKELL_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-haskell PRIVATE /w)
    else()
        target_compile_options(tree-sitter-haskell PRIVATE -w)
    endif()

    # Haskell highlighter module
    add_library(linenoise-highlight-haskell STATIC
        src/syntax/haskell.c
    )
    target_include_directories(linenoise-highlight-haskell
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-haskell
        PRIVATE linenoise-theme tree-sitter tree-sitter-haskell
    )

    # Build tree-sitter-forth grammar
    add_library(tree-sitter-forth STATIC
        ${TREESITTER_FORTH_DIR}/src/parser.c
    )
    target_include_directories(tree-sitter-forth
        PRIVATE
            ${TREESITTER_FORTH_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-forth PRIVATE /w)
    else()
        target_compile_options(tree-sitter-forth PRIVATE -w)
    endif()

    # Forth highlighter module
    add_library(linenoise-highlight-forth STATIC
        src/syntax/forth.c
    )
    target_include_directories(linenoise-highlight-forth
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-forth
        PRIVATE linenoise-theme tree-sitter tree-sitter-forth
    )

    # Build tree-sitter-faust grammar
    add_library(tree-sitter-faust STATIC
        ${TREESITTER_FAUST_DIR}/src/parser.c
    )
    target_include_directories(tree-sitter-faust
        PRIVATE
            ${TREESITTER_FAUST_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-faust PRIVATE /w)
    else()
        target_compile_options(tree-sitter-faust PRIVATE -w)
    endif()

    # Faust highlighter module
    add_library(linenoise-highlight-faust STATIC
        src/syntax/faust.c
    )
    target_include_directories(linenoise-highlight-faust
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-faust
        PRIVATE linenoise-theme tree-sitter tree-sitter-faust
    )

    # Build tree-sitter-chuck grammar
    add_library(tree-sitter-chuck STATIC
        ${TREESITTER_CHUCK_DIR}/src/parser.c
    )
    target_include_directories(tree-sitter-chuck
        PRIVATE
            ${TREESITTER_CHUCK_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-chuck PRIVATE /w)
    else()
        target_compile_options(tree-sitter-chuck PRIVATE -w)
    endif()

    # ChucK highlighter module
    add_library(linenoise-highlight-chuck STATIC
        src/syntax/chuck.c
    )
    target_include_directories(linenoise-highlight-chuck
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-chuck
        PRIVATE linenoise-theme tree-sitter tree-sitter-chuck
    )

    # Build tree-sitter-markdown grammar
    add_library(tree-sitter-markdown STATIC
        ${TREESITTER_MARKDOWN_DIR}/src/parser.c
        ${TREESITTER_MARKDOWN_DIR}/src/scanner.c
    )
    target_include_directories(tree-sitter-markdown
        PRIVATE
            ${TREESITTER_MARKDOWN_DIR}/src
            ${TREESITTER_DIR}/lib/include
    )
    if(MSVC)
        target_compile_options(tree-sitter-markdown PRIVATE /w)
    else()
        target_compile_options(tree-sitter-markdown PRIVATE -w)
    endif()

    # Markdown highlighter module
    add_library(linenoise-highlight-markdown STATIC
        src/syntax/markdown.c
    )
    target_include_directories(linenoise-highlight-markdown
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
            ${TREESITTER_DIR}/lib/include
    )
    target_link_libraries(linenoise-highlight-markdown
        PRIVATE linenoise-theme tree-sitter tree-sitter-markdown
    )

    message(STATUS "linenoise: tree-sitter syntax highlighting enabled")
endif()

# Summary (only for standalone build)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    message(STATUS "")
    message(STATUS "linenoise ${PROJECT_VERSION} configuration:")
    message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
    message(STATUS "  Terminal: ${TERMINAL_SRC}")
    message(STATUS "  Tree-sitter: ${WITH_TREESITTER}")
    message(STATUS "")
endif()
