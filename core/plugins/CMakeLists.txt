# 内置插件（清单在仓库根的 plugins/<id>/plugin.json）。
#
# 目录职责：
#   build_system_common.cpp / .h  构建系统适配器的公共契约与纯函数实现
#   *_generic.cpp                 一类插件的共用代码，差异由 LIGHTC_PLUGIN_ID 决定
#   cpp_base.cpp 等               单个插件的实现
#
# 所有插件都编译成 MODULE 动态库，由宿主用 dlopen / LoadLibrary 加载，
# 与第三方插件走完全相同的路径 —— 宿主能力不会"只有内置插件能用到"。
#
# 构建产物统一落到 build/bin/plugins，`lightcpp-pm host` 直接从这里发现实现库。

add_subdirectory(BuildSystemCore)
add_subdirectory(CppBasePlugin)
add_subdirectory(ClangCorePlugin)
add_subdirectory(GdbPlugin)
add_subdirectory(TestCorePlugin)

foreach(_system IN ITEMS CMakePlugin MesonPlugin MakePlugin NinjaPlugin)
    add_subdirectory(${_system})
endforeach()
