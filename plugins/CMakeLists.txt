# plugins/ — first-party plugins for the LightC++ IDE.
#
# Shared-library plugins are QPluginLoader DLLs loaded once at start-up from
# <exe dir>/plugins. Lua scripts under lua/ are loaded by the app's sandboxed
# Lua host from <exe dir>/plugins/lua.

add_subdirectory(DevCppPlugin)

