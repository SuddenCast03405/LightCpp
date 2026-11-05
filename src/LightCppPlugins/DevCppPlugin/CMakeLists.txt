# DevCppPlugin — Dev-C++ project support as a shared-library plugin.
#
# This is the .dev support that used to live inside the IDE, extracted behind
# the ProjectBackend extension point. It links the same LightCppProject models
# the IDE core uses; the difference is that the IDE core itself no longer
# knows the .dev format — without this dll the format simply is not offered.

add_library(DevCppPlugin MODULE
    DevCppBackend.cpp
    metadata.json
)

set_target_properties(DevCppPlugin PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)

target_include_directories(DevCppPlugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src/LightCppPlugin/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppProject/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppPlugins/Support
)

target_link_libraries(DevCppPlugin PRIVATE
    LightCppPlugin
    LightCppProject
    Qt6::Core
    LightCppWarnings
)

if(MINGW)
    # Same runtime model as the IDE executable: no toolchain DLLs needed.
    target_link_options(DevCppPlugin PRIVATE -static-libgcc -static-libstdc++)
endif()

# Stage the plugin next to the staged IDE, where PluginManager looks.
if(TARGET lightcpp)
    add_dependencies(lightcpp DevCppPlugin)
    add_custom_command(TARGET DevCppPlugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_BINARY_DIR}/dist/plugins
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:DevCppPlugin>
                ${CMAKE_BINARY_DIR}/dist/plugins/
        COMMENT "Staging DevCppPlugin into dist/plugins"
    )
endif()
