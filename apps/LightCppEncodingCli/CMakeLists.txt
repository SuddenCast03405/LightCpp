# lightcpp-enc: encoding detector & UTF-8 converter command line tool.
# ("lightcpp" itself is reserved for the future IDE shell executable.)

add_executable(LightCppEncodingCli
    main.cpp
)

set_target_properties(LightCppEncodingCli PROPERTIES
    OUTPUT_NAME lightcpp-enc
)

target_link_libraries(LightCppEncodingCli PRIVATE
    Qt6::Core
    LightCppText
    LightCppCore
    LightCppWarnings
)

# Statically link the GCC runtime so the binary runs without toolchain DLLs
# on PATH (matches the test binaries).
if(MINGW)
    target_link_options(LightCppEncodingCli PRIVATE -static-libgcc -static-libstdc++)
endif()

# --- Stage a runnable product into <build>/dist --------------------------------
if(WIN32)
    # Copy the Qt runtime DLLs next to the executable...
    add_custom_command(TARGET LightCppEncodingCli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppEncodingCli>
                $<TARGET_FILE_DIR:LightCppEncodingCli>
        COMMAND_EXPAND_LISTS)

    # ...and stage exe + DLLs in dist/ as the "final product" of this module.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
    add_custom_command(TARGET LightCppEncodingCli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LightCppEncodingCli> ${CMAKE_BINARY_DIR}/dist
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppEncodingCli> ${CMAKE_BINARY_DIR}/dist
        COMMAND_EXPAND_LISTS
        COMMENT "Staging lightcpp-enc into ${CMAKE_BINARY_DIR}/dist")
endif()
