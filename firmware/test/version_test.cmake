# Tests the firmware version logic in firmware/version.cmake (script mode, no git, no IDF).
# Run from anywhere:  cmake -P firmware/test/version_test.cmake
# Exits non-zero (FATAL_ERROR) on the first failure.

include("${CMAKE_CURRENT_LIST_DIR}/../version.cmake")

function(expect_version name expected)
    cb_format_version(ver err ${ARGN})
    if(err)
        message(SEND_ERROR "FAIL ${name}: unexpected error: ${err}")
    elseif(NOT ver STREQUAL expected)
        message(SEND_ERROR "FAIL ${name}: got '${ver}', expected '${expected}'")
    else()
        message(STATUS "ok   ${name}: ${ver}")
    endif()
endfunction()

function(expect_error name pattern)
    cb_format_version(ver err ${ARGN})
    if(NOT err)
        message(SEND_ERROR "FAIL ${name}: expected an error, got '${ver}'")
    elseif(NOT err MATCHES "${pattern}")
        message(SEND_ERROR "FAIL ${name}: error '${err}' doesn't match '${pattern}'")
    elseif(ver)
        message(SEND_ERROR "FAIL ${name}: error but version set ('${ver}')")
    else()
        message(STATUS "ok   ${name}: ${err}")
    endif()
endfunction()

# Release: clean and exactly at v<version>.
expect_version("release" "0.1.0"
    VERSION 0.1.0 GIT_OK TRUE TAG "v0.1.0" HASH abc1234 DIRTY FALSE)

# At the tag but dirty: a dev build.
expect_version("tag dirty" "0.1.0-dev+abc1234.dirty"
    VERSION 0.1.0 GIT_OK TRUE TAG "v0.1.0" HASH abc1234 DIRTY TRUE)

# No tag at HEAD.
expect_version("no tag clean" "0.1.0-dev+abc1234"
    VERSION 0.1.0 GIT_OK TRUE TAG "" HASH abc1234 DIRTY FALSE)
expect_version("no tag dirty" "0.1.0-dev+abc1234.dirty"
    VERSION 0.1.0 GIT_OK TRUE TAG "" HASH abc1234 DIRTY TRUE)

# A tag for another version (package.json bumped after tagging, or a stale tag).
expect_version("other tag" "0.2.0-dev+abc1234"
    VERSION 0.2.0 GIT_OK TRUE TAG "v0.1.0" HASH abc1234 DIRTY FALSE)
# The tag must match exactly, not as a prefix or without the v.
expect_version("tag no v" "0.1.0-dev+abc1234"
    VERSION 0.1.0 GIT_OK TRUE TAG "0.1.0" HASH abc1234 DIRTY FALSE)
expect_version("tag longer" "0.1.0-dev+abc1234"
    VERSION 0.1.0 GIT_OK TRUE TAG "v0.1.0-rc1" HASH abc1234 DIRTY FALSE)

# Git unusable: no hash, no dirty mark.
expect_version("no git" "0.1.0-dev"
    VERSION 0.1.0 GIT_OK FALSE TAG "v0.1.0" HASH "" DIRTY FALSE)
expect_version("no git ignores dirty" "0.1.0-dev"
    VERSION 0.1.0 GIT_OK FALSE TAG "" HASH "" DIRTY TRUE)
# Git "ok" but no hash (shouldn't happen): treated as no git.
expect_version("empty hash" "0.1.0-dev"
    VERSION 0.1.0 GIT_OK TRUE TAG "" HASH "" DIRTY FALSE)

# The override wins over everything, used as is.
expect_version("override" "9.9.9"
    OVERRIDE 9.9.9 VERSION 0.1.0 GIT_OK TRUE TAG "" HASH abc1234 DIRTY TRUE)
expect_version("override no git" "1.2.3-ci"
    OVERRIDE 1.2.3-ci VERSION "" GIT_OK FALSE TAG "" HASH "" DIRTY FALSE)

# Exactly 31 characters fits; 32 is refused (esp_app_desc_t.version is char[32]).
expect_version("31 chars" "0.1.0-dev+0123456789abcde.dirty"
    VERSION 0.1.0 GIT_OK TRUE TAG "" HASH 0123456789abcde DIRTY TRUE)
expect_error("32 chars" "too long"
    VERSION 0.1.0 GIT_OK TRUE TAG "" HASH 0123456789abcdef DIRTY TRUE)
expect_error("override too long" "too long"
    OVERRIDE 0123456789012345678901234567890X VERSION 0.1.0 GIT_OK FALSE)

# A missing version is an error.
expect_error("no version" "version"
    VERSION "" GIT_OK TRUE TAG "" HASH abc1234 DIRTY FALSE)

# Reading package.json: the repo's root one has a plain x.y.z version.
cb_read_package_version(pkg_ver pkg_err "${CMAKE_CURRENT_LIST_DIR}/../../package.json")
if(pkg_err)
    message(SEND_ERROR "FAIL read root package.json: ${pkg_err}")
elseif(NOT pkg_ver MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(SEND_ERROR "FAIL read root package.json: '${pkg_ver}' isn't x.y.z")
else()
    message(STATUS "ok   read root package.json: ${pkg_ver}")
endif()

cb_read_package_version(pkg_ver pkg_err "${CMAKE_CURRENT_LIST_DIR}/no-such-package.json")
if(NOT pkg_err OR pkg_ver)
    message(SEND_ERROR "FAIL read missing package.json: no error")
else()
    message(STATUS "ok   read missing package.json: ${pkg_err}")
endif()

# Bad package.json files (fixtures written to a scratch folder under the current directory, removed afterwards).
set(fixtures "${CMAKE_CURRENT_BINARY_DIR}/cb_version_test_fixtures")
file(MAKE_DIRECTORY "${fixtures}")
file(WRITE "${fixtures}/no-version.json" "{ \"name\": \"x\" }\n")
file(WRITE "${fixtures}/number.json" "{ \"version\": 3 }\n")
file(WRITE "${fixtures}/empty.json" "{ \"version\": \"\" }\n")
file(WRITE "${fixtures}/broken.json" "{ \"version\": \n")
file(WRITE "${fixtures}/good.json" "{ \"name\": \"x\", \"version\": \"1.22.333\", \"a\": { \"version\": \"9\" } }\n")
foreach(bad no-version number empty broken)
    cb_read_package_version(pkg_ver pkg_err "${fixtures}/${bad}.json")
    if(NOT pkg_err OR pkg_ver)
        message(SEND_ERROR "FAIL read ${bad}.json: no error ('${pkg_ver}')")
    else()
        message(STATUS "ok   read ${bad}.json: ${pkg_err}")
    endif()
endforeach()
cb_read_package_version(pkg_ver pkg_err "${fixtures}/good.json")
if(pkg_err OR NOT pkg_ver STREQUAL "1.22.333")
    message(SEND_ERROR "FAIL read good.json: '${pkg_ver}' ${pkg_err}")
else()
    message(STATUS "ok   read good.json: ${pkg_ver}")
endif()
file(REMOVE_RECURSE "${fixtures}")
