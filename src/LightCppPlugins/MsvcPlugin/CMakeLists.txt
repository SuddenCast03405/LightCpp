# MsvcPlugin — Microsoft C/C++ project support as a shared-library plugin.
#
# This is the .vcxproj/.vcproj support behind the ProjectBackend extension
# point. The parsed project model lives in LightCppProject (unit-tested
# there); this plugin adds the backend shell and the toolchain location —
# msbuild via vswhere for the MSBuild format, vcbuild probing for
# VS2005-2008. Without this dll the formats simply are not offered.

add_library(MsvcPlugin MODULE
    MsvcBackend.cpp
    metadata.json
)

set_target_properties(MsvcPlugin PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)

target_include_directories(MsvcPlugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src/LightCppPlugin/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppProject/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppPlugins/Support
)

target_link_libraries(MsvcPlugin PRIVATE
    LightCppPlugin
    LightCppProject
    Qt6::Core
    LightCppWarnings
)

if(MINGW)
    # Same runtime model as the IDE executable: no toolchain DLLs needed.
    target_link_options(MsvcPlugin PRIVATE -static-libgcc -static-libstdc++)
endif()

# Stage the plugin next to the staged IDE, where PluginManager looks.
if(TARGET lightcpp)
    add_dependencies(lightcpp MsvcPlugin)
    add_custom_command(TARGET MsvcPlugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_BINARY_DIR}/dist/plugins
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:MsvcPlugin>
                ${CMAKE_BINARY_DIR}/dist/plugins/
        COMMENT "Staging MsvcPlugin into dist/plugins"
    )
endif()
