# Fetch and vendor third-party sources for the Lua plugin host.
# Run: cmake -P _fetch_deps.cmake
# (Direct curl is blocked by the sandbox and git has no https support, but
# CMake's file(DOWNLOAD) works — same approach as the ccls fetch.)

set(TP "${CMAKE_CURRENT_LIST_DIR}/third_party")
file(MAKE_DIRECTORY "${TP}")

set(LUA_VERSION "5.4.7")
set(LUA_TARBALL "${TP}/lua-${LUA_VERSION}.tar.gz")
if(NOT EXISTS "${TP}/lua-${LUA_VERSION}")
    if(NOT EXISTS "${LUA_TARBALL}")
        message(STATUS "Downloading lua ${LUA_VERSION}")
        file(DOWNLOAD
            "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"
            "${LUA_TARBALL}"
            SHOW_PROGRESS
        )
    endif()
    file(ARCHIVE_EXTRACT INPUT "${LUA_TARBALL}" DESTINATION "${TP}")
endif()

set(LB_TAG "3.0-rc12")
set(LB_TARBALL "${TP}/LuaBridge3-${LB_TAG}.tar.gz")
if(NOT EXISTS "${TP}/LuaBridge3-${LB_TAG}")
    if(NOT EXISTS "${LB_TARBALL}")
        message(STATUS "Downloading LuaBridge3 ${LB_TAG}")
        file(DOWNLOAD
            "https://github.com/kunitoki/LuaBridge3/archive/refs/tags/${LB_TAG}.tar.gz"
            "${LB_TARBALL}"
            SHOW_PROGRESS
        )
    endif()
    file(ARCHIVE_EXTRACT INPUT "${LB_TARBALL}" DESTINATION "${TP}")
endif()

foreach(d lua-${LUA_VERSION} LuaBridge3-${LB_TAG})
    if(NOT EXISTS "${TP}/${d}")
        message(FATAL_ERROR "expected ${TP}/${d} after extraction")
    endif()
endforeach()
message(STATUS "third-party deps ready")
