# Copyright (C) 2011-2020  (see AUTHORS file for a list of contributors)
#
# GNSS-SDR is a software-defined Global Navigation Satellite Systems receiver
#
# This file is part of GNSS-SDR.
#
# SPDX-License-Identifier: GPL-3.0-or-later

########################################################################
# Find  GR-GNMAX Module
########################################################################

#
# Provides the following imported target:
# Gnuradio::gnmax
#

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH TRUE)
include(FindPkgConfig)
pkg_check_modules(PC_GR_GNMAX gr-gnmax)

find_path(
    GR_GNMAX_INCLUDE_DIRS
    NAMES gnMAX2769/api.h
    HINTS ${PC_GR_GNMAX_INCLUDEDIR}
    PATHS /usr/include
          /usr/local/include
          /opt/local/include
          ${CMAKE_INSTALL_PREFIX}/include
          ${GRGNMAX_ROOT}/include
          $ENV{GRGNMAX_ROOT}/include
          $ENV{GR_GN3S_DIR}/include
)

find_library(
    GR_GNMAX_LIBRARIES
    NAMES gnuradio-gnMAX2769
    HINTS ${PC_GR_GNMAX_LIBDIR}
    PATHS /usr/lib
          /usr/lib64
          /usr/local/lib
          /usr/local/lib64
          /opt/local/lib
          ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          ${GRGNMAX_ROOT}/lib
          $ENV{GRGNMAX_ROOT}/lib
          ${GRGNMAX_ROOT}/lib64
          $ENV{GRGNMAX_ROOT}/lib64
          $ENV{GR_GN3S_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GRGNMAX DEFAULT_MSG GR_GNMAX_LIBRARIES GR_GNMAX_INCLUDE_DIRS)

if(GRGNMAX_FOUND AND NOT TARGET Gnuradio::gnmax)
    add_library(Gnuradio::gnmax SHARED IMPORTED)
    set_target_properties(Gnuradio::gnmax PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
        IMPORTED_LOCATION "${GR_GNMAX_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${GR_GNMAX_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${GR_GNMAX_LIBRARIES}"
    )
endif()

mark_as_advanced(GR_GNMAX_LIBRARIES GR_GNMAX_INCLUDE_DIRS)
