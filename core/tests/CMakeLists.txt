# 测试：不依赖第三方框架，clone 下来有编译器就能跑。
#
# 命名约定：可执行文件 lightcpp_tests，支持用第一个参数按用例名过滤
#   例如 ./bin/lightcpp_tests json
add_executable(lightcpp_tests
    lightcpp_test_main.cpp
    test_json.cpp
    test_validator.cpp
    test_runtime.cpp)

target_link_libraries(lightcpp_tests PRIVATE lightcpp_host lightcpp_warnings)

# 插件纯函数测试直接编译插件源码，不加载动态库，便于在 CI 里定位失败。
target_sources(lightcpp_tests PRIVATE
    ../plugins/BuildSystemCore/BuildSystemSpec.cpp
    ../plugins/BuildSystemCore/BuildSystemFactory.cpp
    ../plugins/ClangCorePlugin/ClangCoreSpec.cpp
    ../plugins/GdbPlugin/GdbBreakpoints.cpp
    ../plugins/TestCorePlugin/TestCoreSpec.cpp)

target_include_directories(lightcpp_tests PRIVATE
    ../plugins/BuildSystemCore
    ../plugins/ClangCorePlugin
    ../plugins/GdbPlugin
    ../plugins/TestCorePlugin)

target_compile_definitions(lightcpp_tests PRIVATE
    LIGHTC_ROOT="${LIGHTC_ROOT}"
    LIGHTC_PLUGIN_DIR="${LIGHTC_OUTPUT_DIR}/plugins"
    LIGHTC_CLI="$<TARGET_FILE:lightcpp-pm>")

add_test(NAME unit COMMAND lightcpp_tests)

# 端到端：用编译好的 CLI 跑一遍真实命令，确保安装后的行为与开发时一致。
add_test(NAME cli_validate COMMAND lightcpp-pm validate --strict --root "${LIGHTC_ROOT}")
add_test(NAME cli_docs_check COMMAND lightcpp-pm docs --check --root "${LIGHTC_ROOT}")
add_test(NAME cli_resolve COMMAND lightcpp-pm resolve cmake --root "${LIGHTC_ROOT}")
add_test(NAME cli_host COMMAND lightcpp-pm host --root "${LIGHTC_ROOT}"
                                    --lib-dir "${LIGHTC_OUTPUT_DIR}/plugins")
