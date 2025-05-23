# GNSS-SDR is a Global Navigation Satellite System software-defined receiver.
# This file is part of GNSS-SDR.
#
# SPDX-FileCopyrightText: 2010-2020 C. Fernandez-Prades cfernandez(at)cttc.es
# SPDX-License-Identifier: BSD-3-Clause


if(USE_CMAKE_TARGET_SOURCES)
    add_executable(gnss-sdr)
    target_sources(gnss-sdr PRIVATE main.cc)
else()
    add_executable(gnss-sdr main.cc)
endif()

target_link_libraries(gnss-sdr
    PRIVATE
        algorithms_libs
        core_receiver
        gnss_sdr_flags
        Boost::headers
        Boost::thread
        Threads::Threads
)

if(ENABLE_GLOG_AND_GFLAGS)
    target_link_libraries(gnss-sdr PRIVATE Gflags::gflags Glog::glog)
    target_compile_definitions(gnss-sdr PUBLIC -DUSE_GLOG_AND_GFLAGS=1)
else()
    target_link_libraries(gnss-sdr PRIVATE absl::flags absl::flags_parse absl::log absl::log_initialize absl::log_sink absl::log_sink_registry)
    target_link_libraries(gnss-sdr INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,absl::log_flags>")
endif()

if(NOT ENABLE_LOG)
    target_compile_definitions(gnss-sdr PRIVATE -DGOOGLE_STRIP_LOG=1)
endif()

target_compile_definitions(gnss-sdr PRIVATE -DGNSS_SDR_VERSION="${VERSION}")

if(ENABLE_CUDA)
    if(CMAKE_VERSION VERSION_GREATER 3.11)
        target_include_directories(gnss-sdr
            PUBLIC ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
        )
    else()
        target_link_libraries(gnss-sdr
            PUBLIC ${CUDA_LIBRARIES}
        )
        target_include_directories(gnss-sdr
            PUBLIC ${CUDA_INCLUDE_DIRS}
        )
    endif()
    set_target_properties(gnss-sdr PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
    )
    target_compile_definitions(gnss-sdr PRIVATE -DCUDA_GPU_ACCEL=1)
endif()

if(ENABLE_GPERFTOOLS)
    if(GPERFTOOLS_FOUND)
        target_link_libraries(gnss-sdr
            PRIVATE
                Gperftools::profiler
                Gperftools::tcmalloc
        )
    endif()
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_libraries(gnss-sdr PRIVATE "-lc++")
    endif()
endif()

if(ENABLE_STRIP)
    set_target_properties(gnss-sdr PROPERTIES LINK_FLAGS "-s")
endif()

include(XcodeRemoveWarningDuplicates)
xcode_remove_warning_duplicates(gnss-sdr)

if(ENABLE_CLANG_TIDY)
    if(CLANG_TIDY_EXE)
        set_target_properties(gnss-sdr
            PROPERTIES
                CXX_CLANG_TIDY "${DO_CLANG_TIDY}"
        )
    endif()
endif()

add_custom_command(TARGET gnss-sdr
    POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gnss-sdr>
        ${LOCAL_INSTALL_BASE_DIR}/install/$<TARGET_FILE_NAME:gnss-sdr>
)

install(TARGETS gnss-sdr
    RUNTIME DESTINATION bin
    COMPONENT "gnss-sdr"
)

install(DIRECTORY ${GNSSSDR_SOURCE_DIR}/conf
    DESTINATION share/gnss-sdr
    FILES_MATCHING PATTERN "*.conf"
)

install(FILES ${GNSSSDR_SOURCE_DIR}/conf/gnss-sdr.conf
    DESTINATION share/gnss-sdr/conf
    RENAME default.conf
)

if(NOT VOLKGNSSSDR_FOUND)
    install(PROGRAMS ${GNSSSDR_BINARY_DIR}/volk_gnsssdr_module/build/apps/volk_gnsssdr_profile
        DESTINATION bin
        COMPONENT "volk_gnsssdr"
    )

    install(PROGRAMS ${GNSSSDR_BINARY_DIR}/volk_gnsssdr_module/build/apps/volk_gnsssdr-config-info
        DESTINATION bin
        COMPONENT "volk_gnsssdr"
    )
endif()

find_program(GZIP gzip
    ${GNSSSDR_BIN_PATHS}
)

if(NOT GZIP_NOTFOUND)
    execute_process(COMMAND gzip -9 -c ${GNSSSDR_SOURCE_DIR}/docs/manpage/gnss-sdr-manpage
        WORKING_DIRECTORY ${GNSSSDR_BINARY_DIR}
        OUTPUT_FILE "${GNSSSDR_BINARY_DIR}/gnss-sdr.1.gz"
    )

    install(FILES ${GNSSSDR_BINARY_DIR}/gnss-sdr.1.gz
        DESTINATION share/man/man1
    )

    execute_process(COMMAND gzip -9 -c ${GNSSSDR_SOURCE_DIR}/docs/CHANGELOG.md
        WORKING_DIRECTORY ${GNSSSDR_BINARY_DIR}
        OUTPUT_FILE "${GNSSSDR_BINARY_DIR}/changelog.gz"
    )

    install(FILES ${GNSSSDR_BINARY_DIR}/changelog.gz
        DESTINATION share/doc/gnss-sdr
    )

    if(NOT VOLKGNSSSDR_FOUND)
        execute_process(COMMAND gzip -9 -c ${GNSSSDR_SOURCE_DIR}/src/algorithms/libs/volk_gnsssdr_module/volk_gnsssdr/cmake/Packaging/volk_gnsssdr_profile-manpage
            WORKING_DIRECTORY ${GNSSSDR_BINARY_DIR}
            OUTPUT_FILE "${GNSSSDR_BINARY_DIR}/volk_gnsssdr_profile.1.gz"
        )

        execute_process(COMMAND gzip -9 -c ${GNSSSDR_SOURCE_DIR}/src/algorithms/libs/volk_gnsssdr_module/volk_gnsssdr/cmake/Packaging/volk_gnsssdr-config-info-manpage
            WORKING_DIRECTORY ${GNSSSDR_BINARY_DIR}
            OUTPUT_FILE "${GNSSSDR_BINARY_DIR}/volk_gnsssdr-config-info.1.gz"
        )

        install(FILES ${GNSSSDR_BINARY_DIR}/volk_gnsssdr_profile.1.gz
            DESTINATION share/man/man1
        )

        install(FILES ${GNSSSDR_BINARY_DIR}/volk_gnsssdr-config-info.1.gz
            DESTINATION share/man/man1
        )
    endif()
endif()
