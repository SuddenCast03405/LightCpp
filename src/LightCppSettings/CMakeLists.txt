# LightCppSettings — the IDE's configuration model.
#
# JSON documents next to the executable (<exe dir>/config): toolchain
# registries, kits, defaults and preferences, layered with workspace
# overlays. Qt Core only (QJson), no widgets, so the merge/resolve rules can
# be unit-tested: a silently wrong merge is exactly the kind of bug that
# never shows up in a screenshot.

add_library(LightCppSettings STATIC
    Source/SettingsJson.cpp
    Source/SettingsStore.cpp
)

add_library(LightCpp::Settings ALIAS LightCppSettings)

target_include_directories(LightCppSettings PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppSettings PUBLIC cxx_std_20)

target_link_libraries(LightCppSettings
    PUBLIC Qt6::Core
    PRIVATE LightCppWarnings
)
