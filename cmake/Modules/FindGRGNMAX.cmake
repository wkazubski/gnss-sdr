# Copyright (C) 2011-2019 (see AUTHORS file for a list of contributors)
#
# This file is part of GNSS-SDR.
#
# GNSS-SDR is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# GNSS-SDR is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNSS-SDR. If not, see <https://www.gnu.org/licenses/>.

########################################################################
# Find  GR-GNMAX Module
########################################################################

include(FindPkgConfig)
pkg_check_modules(PC_GR_GNMAX gr-gnmax)

find_path(
    GR_GNMAX_INCLUDE_DIRS
    NAMES gnMAX2769/api.h
    HINTS $ENV{GR_GNMAX_DIR}/include
          ${PC_GR_GNMAX_INCLUDEDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/include
          /usr/local/include
          /usr/include
          ${GRGNMAX_ROOT}/include
          $ENV{GRGNMAX_ROOT}/include
)

find_library(
    GR_GNMAX_LIBRARIES
    NAMES gnuradio-gnMAX2769
    HINTS $ENV{GR_GNMAX_DIR}/lib
          ${PC_GR_GNMAX_LIBDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
          ${GRGNMAX_ROOT}/lib
          $ENV{GRGNMAX_ROOT}/lib
          ${GRGNMAX_ROOT}/lib64
          $ENV{GRGNMAX_ROOT}/lib64
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
