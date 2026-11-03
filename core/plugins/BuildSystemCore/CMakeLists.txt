# 构建系统适配器的公共实现，被四个构建系统插件静态链接。
add_library(lightcpp_buildsystem_core STATIC
    BuildSystemSpec.cpp
    BuildSystemFactory.cpp)
target_include_directories(lightcpp_buildsystem_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lightcpp_buildsystem_core PUBLIC LightCpp::Core PRIVATE lightcpp_warnings)
