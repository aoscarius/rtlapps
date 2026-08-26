# FindRTLSDR.cmake
#
# librtlsdr ships a pkg-config file (librtlsdr.pc) on essentially every
# distro but does NOT ship a CMake package config, so `find_package(RTLSDR
# CONFIG)` never works out of the box. This module gives find_package()
# something to call: it tries pkg-config first (fast path, covers apt/brew/
# pacman installs), then falls back to a manual find_path/find_library
# search (covers source installs / non-standard prefixes), and exposes a
# single imported target either way so the rest of the build doesn't need
# to care which path succeeded.
#
# Result:
#   RTLSDR_FOUND
#   RTLSDR::rtlsdr   (imported target -- link against this)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_RTLSDR QUIET librtlsdr)
endif()

find_path(RTLSDR_INCLUDE_DIR
    NAMES rtl-sdr.h
    HINTS ${PC_RTLSDR_INCLUDE_DIRS}
    PATH_SUFFIXES rtlsdr
)

find_library(RTLSDR_LIBRARY
    NAMES rtlsdr librtlsdr
    HINTS ${PC_RTLSDR_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RTLSDR
    REQUIRED_VARS RTLSDR_LIBRARY RTLSDR_INCLUDE_DIR
    FAIL_MESSAGE "librtlsdr not found. Install librtlsdr-dev (Debian/Ubuntu), rtl-sdr (Homebrew), or rtl-sdr-git (AUR)."
)

if(RTLSDR_FOUND AND NOT TARGET RTLSDR::rtlsdr)
    add_library(RTLSDR::rtlsdr UNKNOWN IMPORTED)
    set_target_properties(RTLSDR::rtlsdr PROPERTIES
        IMPORTED_LOCATION "${RTLSDR_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${RTLSDR_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(RTLSDR_INCLUDE_DIR RTLSDR_LIBRARY)
