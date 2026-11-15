# Firmware version (esp_app_desc_t.version, GET_INFO fw_version), from the product version in the repo's root
# package.json. Included by firmware/CMakeLists.txt before project(): cb_set_project_version() sets PROJECT_VER,
# which ESP-IDF uses instead of its own `git describe`.
#
#   -DCB_VERSION=<v>            used as is (CI passes the release tag's version). One-shot: removed from the cache
#                               straight away, so the next configure goes back to the computed version.
#   clean and HEAD at v<ver>    <ver>                         (a release build)
#   otherwise                   <ver>-dev+<shorthash>[.dirty] (dirty: tracked files changed, submodules included)
#   git unusable                <ver>-dev                     (no git, not a repo, "dubious ownership", ...)
#
# At most 31 characters (esp_app_desc_t.version is char[32]): a longer version fails the configure rather than
# being cut (a cut would drop the hash or the dirty mark). The string logic (cb_format_version,
# cb_read_package_version) is pure and tested by firmware/test/version_test.cmake (cmake -P).

set(CB_VERSION_MAX_LEN 31)

# An empty one-value argument (TAG "") is an empty string, not unset (and no dev warning). include() gives this
# file its own policy scope, so the includer is unaffected.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

# Reads the top-level "version" string of a package.json. Sets <out_ver>, or <out_err> (and <out_ver> empty).
function(cb_read_package_version out_ver out_err file)
    set(${out_ver} "" PARENT_SCOPE)
    if(NOT EXISTS "${file}")
        set(${out_err} "${file} not found" PARENT_SCOPE)
        return()
    endif()
    file(READ "${file}" json)
    string(JSON type ERROR_VARIABLE err TYPE "${json}" version)
    if(err)
        set(${out_err} "${file}: no version (${err})" PARENT_SCOPE)
        return()
    endif()
    if(NOT type STREQUAL "STRING")
        set(${out_err} "${file}: version isn't a string" PARENT_SCOPE)
        return()
    endif()
    string(JSON ver GET "${json}" version)
    if(ver STREQUAL "")
        set(${out_err} "${file}: version is empty" PARENT_SCOPE)
        return()
    endif()
    set(${out_ver} "${ver}" PARENT_SCOPE)
    set(${out_err} "" PARENT_SCOPE)
endfunction()

# Builds the version string from injected inputs (no git, no files):
#   cb_format_version(<out_ver> <out_err> [OVERRIDE <v>] VERSION <v> GIT_OK <bool> TAG <tag at HEAD or "">
#                     HASH <short hash> DIRTY <bool>)
function(cb_format_version out_ver out_err)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "OVERRIDE;VERSION;GIT_OK;TAG;HASH;DIRTY" "")
    set(${out_ver} "" PARENT_SCOPE)
    if(NOT "${arg_OVERRIDE}" STREQUAL "")
        set(ver "${arg_OVERRIDE}")
    elseif("${arg_VERSION}" STREQUAL "")
        set(${out_err} "no product version" PARENT_SCOPE)
        return()
    elseif(NOT arg_GIT_OK OR "${arg_HASH}" STREQUAL "")
        set(ver "${arg_VERSION}-dev")
    elseif("${arg_TAG}" STREQUAL "v${arg_VERSION}" AND NOT arg_DIRTY)
        set(ver "${arg_VERSION}")
    else()
        set(ver "${arg_VERSION}-dev+${arg_HASH}")
        if(arg_DIRTY)
            string(APPEND ver ".dirty")
        endif()
    endif()
    string(LENGTH "${ver}" len)
    if(len GREATER CB_VERSION_MAX_LEN)
        set(${out_err} "firmware version '${ver}' is too long (${len} chars, max ${CB_VERSION_MAX_LEN}: esp_app_desc_t.version)" PARENT_SCOPE)
        return()
    endif()
    set(${out_ver} "${ver}" PARENT_SCOPE)
    set(${out_err} "" PARENT_SCOPE)
endfunction()

# Reads the git state of <repo_dir> for <version>: sets <prefix>_GIT_OK, _TAG (v<version> if HEAD carries that tag,
# else ""), _HASH (short hash) and _DIRTY in the caller's scope.
function(cb_git_state prefix repo_dir version)
    set(ok FALSE)
    set(tag "")
    set(hash "")
    set(dirty FALSE)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${repo_dir}" RESULT_VARIABLE res OUTPUT_VARIABLE out ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(res EQUAL 0 AND NOT out STREQUAL "")
            set(hash "${out}")
            execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                WORKING_DIRECTORY "${repo_dir}" RESULT_VARIABLE res OUTPUT_VARIABLE out ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(res EQUAL 0)
                set(ok TRUE)
                if(NOT out STREQUAL "")
                    set(dirty TRUE)
                endif()
                execute_process(COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD --list "v${version}"
                    WORKING_DIRECTORY "${repo_dir}" RESULT_VARIABLE res OUTPUT_VARIABLE out ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(res EQUAL 0)
                    set(tag "${out}")
                endif()
            endif()
        endif()
    endif()
    set(${prefix}_GIT_OK ${ok} PARENT_SCOPE)
    set(${prefix}_TAG "${tag}" PARENT_SCOPE)
    set(${prefix}_HASH "${hash}" PARENT_SCOPE)
    set(${prefix}_DIRTY ${dirty} PARENT_SCOPE)
endfunction()

# Re-runs the configure when the version or the commit changes: package.json, .git/HEAD (checkout) and the
# current branch's loose ref (commit). Not .git/index: `git status` above refreshes it, so every build would
# reconfigure. The dirty mark is therefore only refreshed by a configure for another reason.
function(cb_version_configure_depends repo_dir package_json)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${package_json}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        return()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path HEAD
        WORKING_DIRECTORY "${repo_dir}" RESULT_VARIABLE res OUTPUT_VARIABLE head ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(res EQUAL 0 AND EXISTS "${head}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${head}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
        WORKING_DIRECTORY "${repo_dir}" RESULT_VARIABLE res OUTPUT_VARIABLE ref ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(res EQUAL 0 AND NOT ref STREQUAL "")
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path "${ref}"
            WORKING_DIRECTORY "${repo_dir}" RESULT_VARIABLE res OUTPUT_VARIABLE ref_file ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(res EQUAL 0 AND EXISTS "${ref_file}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ref_file}")
        endif()
    endif()
endfunction()

# Sets PROJECT_VER in the caller's scope (call before project()).
function(cb_set_project_version)
    set(repo_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(package_json "${repo_dir}/../package.json")
    get_filename_component(package_json "${package_json}" ABSOLUTE)

    # One-shot override: read it, then drop it from the cache so it doesn't stick to this build folder.
    set(override "${CB_VERSION}")
    unset(CB_VERSION CACHE)

    set(product "")
    if(override STREQUAL "")
        cb_read_package_version(product err "${package_json}")
        if(err)
            message(FATAL_ERROR "Firmware version: ${err}")
        endif()
        cb_git_state(git "${repo_dir}" "${product}")
    endif()
    cb_format_version(ver err OVERRIDE "${override}" VERSION "${product}" GIT_OK ${git_GIT_OK}
        TAG "${git_TAG}" HASH "${git_HASH}" DIRTY ${git_DIRTY})
    if(err)
        message(FATAL_ERROR "Firmware version: ${err}")
    endif()
    cb_version_configure_depends("${repo_dir}" "${package_json}")
    if(override STREQUAL "")
        message(STATUS "Firmware version: ${ver}")
    else()
        message(STATUS "Firmware version: ${ver} (from -DCB_VERSION)")
    endif()
    set(PROJECT_VER "${ver}" PARENT_SCOPE)
endfunction()
