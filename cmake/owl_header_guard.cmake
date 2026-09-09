# owl_header_guard(<target> <name> <out_public>)
#
# The consistency guard every header-only library here runs at configure
# time. The umbrella rotted once before and shipped with ctest fully green
# (see coro/tests/test_umbrella.cpp); discipline was the thing that failed,
# so the guard is a build failure instead. Three lists must agree exactly:
#
#   1. the headers actually on disk under include/<name>/
#   2. the target's FILE_SET manifest, which decides what installs
#   3. the #includes in <name>/<name>.h, which decide what a consumer gets
#
# detail/ headers are in 1 and 2 but deliberately not in 3. The public
# list (1 minus the umbrella and detail/) is handed back through
# <out_public> for the per-header compile checks.
function(owl_header_guard target name out_public)
    set(_include "${CMAKE_CURRENT_SOURCE_DIR}/include")

    file(GLOB_RECURSE _on_disk
            RELATIVE "${_include}"
            CONFIGURE_DEPENDS
            "${_include}/${name}/*.h")
    list(SORT _on_disk)

    get_target_property(_manifest_raw ${target} HEADER_SET)
    set(_manifest "")
    foreach (_f IN LISTS _manifest_raw)
        cmake_path(ABSOLUTE_PATH _f
                BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE
                OUTPUT_VARIABLE _abs)
        cmake_path(RELATIVE_PATH _abs
                BASE_DIRECTORY "${_include}"
                OUTPUT_VARIABLE _rel)
        list(APPEND _manifest "${_rel}")
    endforeach ()
    list(SORT _manifest)

    if (NOT _on_disk STREQUAL _manifest)
        set(_missing ${_on_disk})
        if (_manifest)
            list(REMOVE_ITEM _missing ${_manifest})
        endif ()
        set(_stale ${_manifest})
        if (_on_disk)
            list(REMOVE_ITEM _stale ${_on_disk})
        endif ()
        message(FATAL_ERROR
                "${name}: FILE_SET does not match the header tree.\n"
                "  on disk but not in FILE_SET (would not install): ${_missing}\n"
                "  in FILE_SET but not on disk: ${_stale}")
    endif ()

    set(_public ${_on_disk})
    list(REMOVE_ITEM _public "${name}/${name}.h")
    list(FILTER _public EXCLUDE REGEX "/detail/")
    list(SORT _public)

    file(STRINGS "${_include}/${name}/${name}.h" _umbrella_lines
            REGEX "^#include \"${name}/")
    set(_umbrella "")
    foreach (_line IN LISTS _umbrella_lines)
        string(REGEX REPLACE "^#include \"([^\"]+)\".*$" "\\1" _inc "${_line}")
        list(APPEND _umbrella "${_inc}")
    endforeach ()
    list(SORT _umbrella)

    if (NOT _public STREQUAL _umbrella)
        set(_unumbrellaed ${_public})
        if (_umbrella)
            list(REMOVE_ITEM _unumbrellaed ${_umbrella})
        endif ()
        set(_phantom ${_umbrella})
        if (_public)
            list(REMOVE_ITEM _phantom ${_public})
        endif ()
        message(FATAL_ERROR
                "${name}: ${name}.h does not match the public header tree.\n"
                "  public but not in the umbrella: ${_unumbrellaed}\n"
                "  in the umbrella but not public (or a detail/ header): ${_phantom}")
    endif ()

    set(${out_public} ${_public} PARENT_SCOPE)
endfunction()
