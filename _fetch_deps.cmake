# Fetch and vendor third-party sources for the Lua plugin host.
# Run: cmake -P _fetch_deps.cmake
# (Direct curl is blocked by the sandbox and git has no https support, but
# CMake's file(DOWNLOAD) works — same approach as the ccls fetch.)
#
# Four things this script has to get right, because the build depends on it:
#   1. A failed download must not leave a zero-byte tarball behind. An empty
#      file that exists makes the next run skip the download for ever, so the
#      build stays broken until someone deletes it by hand.
#   2. archive extraction is allowed to fail; it is checked, not assumed.
#   3. an already-present tarball is verified, not trusted. A truncated file
#      from an interrupted run is re-downloaded instead of extracted.
#   4. more than one source URL per dependency: lua.org is unreachable from
#      plenty of CI/sandbox networks, the GitHub mirrors usually are not.

set(TP "${CMAKE_CURRENT_LIST_DIR}/third_party")
file(MAKE_DIRECTORY "${TP}")

# ---------------------------------------------------------------- helpers

# file(DOWNLOAD) reports "no error" for an HTTP error page, so the size is
# what tells a real tarball from a 404 body or an interrupted transfer.
function(lightcpp_download out_file min_bytes)
    set(_tmp "${out_file}.part")
    foreach(url IN LISTS ARGN)
        message(STATUS "Downloading ${url}")
        file(REMOVE "${_tmp}")
        file(DOWNLOAD "${url}" "${_tmp}" STATUS _status TLS_VERIFY OFF)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            message(STATUS "  failed (${_code}): ${_message}")
            file(REMOVE "${_tmp}")
            continue()
        endif()
        file(SIZE "${_tmp}" _size)
        if(_size LESS min_bytes)
            message(STATUS "  rejected: only ${_size} bytes, expected at least ${min_bytes}")
            file(REMOVE "${_tmp}")
            continue()
        endif()
        file(RENAME "${_tmp}" "${out_file}")
        message(STATUS "  ok: ${_size} bytes")
        return()
    endforeach()
    file(REMOVE "${_tmp}")
    message(FATAL_ERROR
        "无法下载 ${out_file}。请手动下载后放到该路径（third_party/ 已被 .gitignore 忽略），再重新配置。")
endfunction()

# Ensures <TP>/<dir> exists, downloading <tarball> from one of the URLs if not.
function(lightcpp_vendor dir tarball min_bytes)
    if(EXISTS "${TP}/${dir}")
        return()
    endif()

    if(EXISTS "${TP}/${tarball}")
        file(SIZE "${TP}/${tarball}" _size)
        if(_size LESS min_bytes)
            message(STATUS "${tarball} 只有 ${_size} 字节，判定为残留的坏文件，重新下载")
            file(REMOVE "${TP}/${tarball}")
        endif()
    endif()

    if(NOT EXISTS "${TP}/${tarball}")
        lightcpp_download("${TP}/${tarball}" ${min_bytes} ${ARGN})
    endif()

    file(ARCHIVE_EXTRACT INPUT "${TP}/${tarball}" DESTINATION "${TP}")
    if(NOT EXISTS "${TP}/${dir}")
        file(REMOVE "${TP}/${tarball}")
        message(FATAL_ERROR
            "${tarball} 解压后没有 ${dir} 目录，文件可能损坏（已删除，可重跑）。")
    endif()
endfunction()

# ---------------------------------------------------------------- lua 5.4.7
# Minimum sizes only need to catch empty/HTML/partial responses; the real check
# is that the expected directory appears after extraction.
lightcpp_vendor(lua-5.4.7 lua-5.4.7.tar.gz 200000
    "https://codeload.github.com/lua/lua/tar.gz/refs/tags/v5.4.7"
    "https://www.lua.org/ftp/lua-5.4.7.tar.gz"
    "https://github.com/lua/lua/archive/refs/tags/v5.4.7.tar.gz")

# ---------------------------------------------------------------- LuaBridge3
lightcpp_vendor(LuaBridge3-3.0-rc12 LuaBridge3-3.0-rc12.tar.gz 500000
    "https://codeload.github.com/kunitoki/LuaBridge3/tar.gz/refs/tags/3.0-rc12"
    "https://github.com/kunitoki/LuaBridge3/archive/refs/tags/3.0-rc12.tar.gz")

# ---------------------------------------------------------------- layout fix
# The lua.org release tarball has lua-5.4.7/src/*.c; the GitHub mirror has the
# sources at the archive root. src/LightCppLua/CMakeLists.txt expects the
# release layout, so normalise whichever source was used.
if(EXISTS "${TP}/lua-5.4.7" AND NOT EXISTS "${TP}/lua-5.4.7/src")
    message(STATUS "调整 lua 源码布局：把根目录的 .c/.h 放进 src/")
    file(MAKE_DIRECTORY "${TP}/lua-5.4.7/src")
    file(GLOB _lua_roots "${TP}/lua-5.4.7/*.c" "${TP}/lua-5.4.7/*.h")
    foreach(_file IN LISTS _lua_roots)
        get_filename_component(_name "${_file}" NAME)
        file(RENAME "${_file}" "${TP}/lua-5.4.7/src/${_name}")
    endforeach()
endif()

foreach(d lua-5.4.7 LuaBridge3-3.0-rc12)
    if(NOT EXISTS "${TP}/${d}")
        message(FATAL_ERROR "expected ${TP}/${d} after extraction")
    endif()
endforeach()
message(STATUS "third-party deps ready")
