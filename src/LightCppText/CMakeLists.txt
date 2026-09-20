# LightCppText: encoding detection & conversion library (Qt 6 based).

add_library(LightCppText STATIC
    Source/Encoding.cpp
    Source/Converter.cpp
    Source/TextFile.cpp
)

add_library(LightCpp::Text ALIAS LightCppText)

target_include_directories(LightCppText PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppText PUBLIC cxx_std_20)

target_link_libraries(LightCppText
    PUBLIC
        Qt6::Core
        LightCppCore
    PRIVATE
        LightCppWarnings
)
