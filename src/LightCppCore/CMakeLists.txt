# LightCppCore: core logic library (Qt-free by design, spec 4.1/4.4).

add_library(LightCppCore STATIC
    Source/FoldModel.cpp
    Source/Utf8File.cpp
)

add_library(LightCpp::Core ALIAS LightCppCore)

target_include_directories(LightCppCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppCore PUBLIC cxx_std_20)

# Strict warnings (see LightCppWarnings) apply to our own targets only.
target_link_libraries(LightCppCore PRIVATE LightCppWarnings)
