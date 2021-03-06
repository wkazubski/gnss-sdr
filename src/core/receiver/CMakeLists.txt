# Copyright (C) 2012-2020  (see AUTHORS file for a list of contributors)
#
# GNSS-SDR is a software-defined Global Navigation Satellite Systems receiver
#
# This file is part of GNSS-SDR.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#


set(GNSS_RECEIVER_SOURCES
    control_thread.cc
    file_configuration.cc
    gnss_block_factory.cc
    gnss_flowgraph.cc
    in_memory_configuration.cc
    tcp_cmd_interface.cc
)

set(GNSS_RECEIVER_HEADERS
    control_thread.h
    file_configuration.h
    gnss_block_factory.h
    gnss_flowgraph.h
    in_memory_configuration.h
    tcp_cmd_interface.h
    concurrent_map.h
    concurrent_queue.h
)

list(SORT GNSS_RECEIVER_HEADERS)
list(SORT GNSS_RECEIVER_SOURCES)

set(GNSS_RECEIVER_INTERFACE_HEADERS
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/acquisition_interface.h
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/channel_interface.h
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/configuration_interface.h
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/gnss_block_interface.h
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/observables_interface.h
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/pvt_interface.h
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/telemetry_decoder_interface.h
    ${CMAKE_SOURCE_DIR}/src/core/interfaces/tracking_interface.h
)

list(SORT GNSS_RECEIVER_INTERFACE_HEADERS)

if(USE_CMAKE_TARGET_SOURCES)
    add_library(core_receiver STATIC)
    target_sources(core_receiver
        PRIVATE
            ${GNSS_RECEIVER_SOURCES}
        PUBLIC
            ${GNSS_RECEIVER_HEADERS}
        INTERFACE
            ${GNSS_RECEIVER_INTERFACE_HEADERS}
    )
else()
    source_group(Headers FILES ${GNSS_RECEIVER_HEADERS}
        ${GNSS_RECEIVER_INTERFACE_HEADERS}
    )
    add_library(core_receiver
        ${GNSS_RECEIVER_SOURCES}
        ${GNSS_RECEIVER_HEADERS}
        ${GNSS_RECEIVER_INTERFACE_HEADERS}
    )
endif()

if(ENABLE_FPGA)
    target_compile_definitions(core_receiver PUBLIC -DENABLE_FPGA=1)
endif()

if(ENABLE_RAW_UDP)
    target_compile_definitions(core_receiver PRIVATE -DRAW_UDP=1)
endif()

if(GNURADIO_IS_38_OR_GREATER)
    target_compile_definitions(core_receiver PRIVATE -DGR_GREATER_38=1)
endif()

if(ENABLE_UHD AND GNURADIO_UHD_LIBRARIES_gnuradio-uhd)
    target_compile_definitions(core_receiver PRIVATE -DUHD_DRIVER=1)
endif()

if(ENABLE_PLUTOSDR)
    target_compile_definitions(core_receiver PRIVATE -DPLUTOSDR_DRIVER=1)
endif()

if(ENABLE_FMCOMMS2)
    target_compile_definitions(core_receiver PRIVATE -DFMCOMMS2_DRIVER=1)
endif()

if(ENABLE_AD9361)
    target_compile_definitions(core_receiver PRIVATE -DAD9361_DRIVER=1)
endif()

if(ENABLE_OSMOSDR)
    if(GROSMOSDR_FOUND)
        target_compile_definitions(core_receiver PRIVATE -DOSMOSDR_DRIVER=1)
    endif()
endif()

if(ENABLE_GN3S)
    target_compile_definitions(core_receiver PRIVATE -DGN3S_DRIVER=1)
endif()

if(ENABLE_GNMAX)
    target_compile_definitions(core_receiver PRIVATE -DGNMAX_DRIVER=1)
endif()

if(ENABLE_ARRAY)
    target_compile_definitions(core_receiver PRIVATE -DRAW_ARRAY_DRIVER=1)
endif()

if(ENABLE_FLEXIBAND)
    target_compile_definitions(core_receiver PRIVATE -DFLEXIBAND_DRIVER=1)
endif()

if(ENABLE_OPENCL)
    message(STATUS "Adding processing blocks implemented using OpenCL")
    target_link_libraries(core_receiver PUBLIC algorithms_libs OpenCL::OpenCL)
    target_compile_definitions(core_receiver PRIVATE -DOPENCL_BLOCKS=1)
else()
    target_compile_definitions(core_receiver PRIVATE -DOPENCL_BLOCKS=0)
endif()

if(ENABLE_CUDA)
    if(NOT CMAKE_VERSION VERSION_GREATER 3.11)
        target_include_directories(core_receiver PUBLIC ${CUDA_INCLUDE_DIRS})
    endif()
    target_compile_definitions(core_receiver PRIVATE -DCUDA_GPU_ACCEL=1)
endif()

if(USE_BOOST_ASIO_IO_CONTEXT)
    target_compile_definitions(core_receiver
        PRIVATE
            -DUSE_BOOST_ASIO_IO_CONTEXT=1
    )
endif()

target_link_libraries(core_receiver
    PUBLIC
        core_libs
    PRIVATE
        core_monitor
        signal_source_adapters
        data_type_adapters
        input_filter_adapters
        conditioner_adapters
        resampler_adapters
        acquisition_adapters
        tracking_adapters
        channel_adapters
        telemetry_decoder_adapters
        obs_adapters
        pvt_adapters
        Boost::headers
        Gflags::gflags
        Glog::glog
        Armadillo::armadillo
)

target_include_directories(core_receiver PRIVATE ${CMAKE_SOURCE_DIR}/src/algorithms/libs)

if(ENABLE_ARMA_NO_DEBUG)
    target_compile_definitions(core_receiver
        PRIVATE -DARMA_NO_BOUND_CHECKING=1
    )
endif()

if(USE_GENERIC_LAMBDAS)
    set(has_generic_lambdas HAS_GENERIC_LAMBDA=1)
    set(no_has_generic_lambdas HAS_GENERIC_LAMBDA=0)
    target_compile_definitions(core_receiver
        PRIVATE
            "$<$<COMPILE_FEATURES:cxx_generic_lambdas>:${has_generic_lambdas}>"
            "$<$<NOT:$<COMPILE_FEATURES:cxx_generic_lambdas>>:${no_has_generic_lambdas}>"
    )
else()
    target_compile_definitions(core_receiver
        PRIVATE
            -DHAS_GENERIC_LAMBDA=0
    )
endif()

# Fix for Boost Asio < 1.70
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    if((CMAKE_CXX_COMPILER_ID MATCHES "Clang") AND (Boost_VERSION_STRING VERSION_LESS 1.70.0))
        if(${has_string_view})
            target_compile_definitions(core_receiver
                PUBLIC
                    -DBOOST_ASIO_HAS_STD_STRING_VIEW=1
            )
        else()
            target_compile_definitions(core_receiver
                PUBLIC
                    -DBOOST_ASIO_HAS_STD_STRING_VIEW=0
            )
        endif()
    endif()
endif()

if(ENABLE_FPGA)
    target_link_libraries(core_receiver
        PUBLIC
            Boost::thread
        PRIVATE
            Boost::chrono
    )
endif()

if(ENABLE_FLEXIBAND AND TELEORBIT_FOUND)
    target_link_libraries(core_receiver
        PRIVATE
            Gnuradio::teleorbit
    )
endif()

if(ENABLE_GN3S AND GRGN3S_FOUND)
    target_link_libraries(core_receiver
        PRIVATE
            Gnuradio::gn3s
    )
endif()

if(ENABLE_GNMAX AND GRGNMAX_FOUND)
    target_link_libraries(core_receiver
        PRIVATE
            Gnuradio::gnmax
    )
endif()

if(ENABLE_ARRAY AND GRDBFCTTC_FOUND)
    target_link_libraries(core_receiver
        PRIVATE
            Gnuradio::dbfcttc
    )
endif()

if(ENABLE_CLANG_TIDY)
    if(CLANG_TIDY_EXE)
        set_target_properties(core_receiver
            PROPERTIES
                CXX_CLANG_TIDY "${DO_CLANG_TIDY}"
        )
    endif()
endif()

set_property(TARGET core_receiver APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/core/receiver>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/core/interfaces>
)
