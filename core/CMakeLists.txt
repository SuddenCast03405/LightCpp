cmake_minimum_required(VERSION 3.16)

project(LightCpp
    VERSION 0.2.0
    DESCRIPTION "轻量级 C/C++ IDE 的插件生态内核"
    HOMEPAGE_URL "https://github.com/SuddenCast03405/LightCpp"
    LANGUAGES CXX)

# ---------------------------------------------------------------- 选项
option(LIGHTC_BUILD_TESTS "构建测试" ON)
option(LIGHTC_BUILD_PLUGINS "构建内置插件动态库" ON)
option(LIGHTC_WERROR "把编译警告视为错误" OFF)
option(LIGHTC_WITH_SANITIZERS "启用 ASan + UBSan（仅 Debug，用于本地/CI 检查）" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------- sanitizer
# The option used to be declared and never read: CI passed
# -DLIGHTC_WITH_SANITIZERS=ON and the build silently ran without any
# instrumentation. Apply it to every target in this directory instead.
if(LIGHTC_WITH_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "LIGHTC_WITH_SANITIZERS 需要 GCC 或 Clang（MSVC 上请用 /fsanitize=address 配置项）")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "Sanitizers: address + undefined")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LightCppPlugins)

# ---------------------------------------------------------------- 共享变量
set(LIGHTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(LIGHTC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${LIGHTC_OUTPUT_DIR}")

# ---------------------------------------------------------------- 警告
add_library(lightcpp_warnings INTERFACE)
if(MSVC)
    target_compile_options(lightcpp_warnings INTERFACE /W4 /permissive- /utf-8)
    if(LIGHTC_WERROR)
        target_compile_options(lightcpp_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(lightcpp_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual -Wdouble-promotion
        -Wformat=2 -Wimplicit-fallthrough)
    if(LIGHTC_WERROR)
        target_compile_options(lightcpp_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------- 核心库
add_library(lightcpp_core STATIC
    src/json.cpp
    src/version.cpp
    src/fs.cpp
    src/text.cpp
    src/manifest.cpp
    src/validator.cpp
    src/resolver.cpp
    src/docgen.cpp
)
add_library(LightCpp::Core ALIAS lightcpp_core)

target_include_directories(lightcpp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(lightcpp_core PRIVATE lightcpp_warnings)

if(MSVC)
    target_compile_definitions(lightcpp_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # 静态库也要 -fPIC，插件与宿主共享同一份核心实现时才能链接进动态库。
    target_compile_options(lightcpp_core PUBLIC -fPIC)
endif()

# 插件宿主（依赖 dlopen / LoadLibrary）
add_library(lightcpp_host STATIC
    src/host.cpp
)
target_link_libraries(lightcpp_host PUBLIC lightcpp_core PRIVATE lightcpp_warnings)
if(NOT MSVC)
    target_link_libraries(lightcpp_host PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------- CLI
add_executable(lightcpp-pm
    src/cli_main.cpp
)
target_link_libraries(lightcpp-pm PRIVATE lightcpp_host lightcpp_warnings)
set_target_properties(lightcpp-pm PROPERTIES OUTPUT_NAME lightcpp-pm)

# 让 CLI 在任意目录运行；同时把仓库根传给测试，避免测试写死相对路径。
target_compile_definitions(lightcpp-pm PRIVATE
    LIGHTC_ROOT="${LIGHTC_ROOT}"
    LIGHTC_PROJECT_VERSION="${PROJECT_VERSION}"
    # 原生插件库的实际输出目录：独立构建时是 core/build/bin/plugins，
    # 作为根项目子目录构建时是 <root build>/bin/plugins —— 由 CMake 给出，
    # 不写死任何一种布局。
    LIGHTC_BUILD_LIB_DIR="${LIGHTC_OUTPUT_DIR}/plugins")

# ---------------------------------------------------------------- 内置插件
if(LIGHTC_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

# ---------------------------------------------------------------- 测试
if(LIGHTC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- 安装
include(GNUInstallDirs)
install(TARGETS lightcpp-pm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${LIGHTC_ROOT}/plugins/" DESTINATION "${CMAKE_INSTALL_DATADIR}/lightcpp/plugins"
        FILES_MATCHING PATTERN "plugin.json")
install(DIRECTORY "${LIGHTC_ROOT}/catalog/" DESTINATION "${CMAKE_INSTALL_DATADIR}/lightcpp/catalog"
        FILES_MATCHING PATTERN "*.json")

message(STATUS "LightCpp ${PROJECT_VERSION}: 编译器 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}，"
               "构建类型 ${CMAKE_BUILD_TYPE}")
