# 插件构建辅助：所有插件统一编译成 MODULE 动态库，输出到 bin/plugins。
#
# 让宿主能找到实现库的关键是"输出目录 + 文件名"约定：
#   <repo>/core/build/bin/plugins/lib<id 中的 - 换成 _>.so
# 宿主按同一约定拼路径，因此新增插件不需要改宿主代码。

function(lightcpp_add_plugin target)
    cmake_parse_arguments(ARG "" "OUTPUT_OUT" "SOURCES" ${ARGN})
    if(NOT ARG_OUTPUT_OUT)
        message(FATAL_ERROR "lightcpp_add_plugin(${target}) 缺少 OUTPUT_OUT")
    endif()
    add_library(${target} MODULE ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE LightCpp::Core lightcpp_warnings)
    # 插件公共头（plugin_helpers.h）对所有插件可见
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${ARG_OUTPUT_OUT}"
        LIBRARY_OUTPUT_DIRECTORY "${LIGHTC_OUTPUT_DIR}/plugins"
        RUNTIME_OUTPUT_DIRECTORY "${LIGHTC_OUTPUT_DIR}/plugins"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LABELS "plugin")
    if(MSVC)
        # Windows 没有 lib 前缀，保证文件名与宿主约定一致
        set_target_properties(${target} PROPERTIES PREFIX "")
    endif()
endfunction()
