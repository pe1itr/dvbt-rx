cmake_minimum_required(VERSION 3.16)

project(rbdvbt_rx VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RBDVBT_DECODER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../sdr-dvb-pipelines/components/dvbt"
    CACHE PATH "Canonical DVB-T component from sdr-dvb-pipelines")
get_filename_component(RBDVBT_DECODER_ROOT "${RBDVBT_DECODER_ROOT}" ABSOLUTE)
if(NOT EXISTS "${RBDVBT_DECODER_ROOT}/include/viterbi_neon.h")
    message(FATAL_ERROR "DVB-T component not found. Checkout sdr-dvb-pipelines next to this project, or set -DRBDVBT_DECODER_ROOT=/path/to/sdr-dvb-pipelines/components/dvbt. See README.md.")
endif()
message(STATUS "DVB-T decoder source: ${RBDVBT_DECODER_ROOT}")
set(RBDVBT_CORE_SOURCES)
foreach(module config dvbt_2k_model dvbt_outer iq_input pipeline probe_constellation receiver udp_ts_output visualizer)
    list(APPEND RBDVBT_CORE_SOURCES "${RBDVBT_DECODER_ROOT}/src/${module}.c")
endforeach()
add_executable(rbdvbt_rx "${RBDVBT_DECODER_ROOT}/src/main.c" ${RBDVBT_CORE_SOURCES})

target_include_directories(rbdvbt_rx PRIVATE "${RBDVBT_DECODER_ROOT}/include")
target_compile_definitions(rbdvbt_rx PRIVATE RBDVBT_VERSION="${PROJECT_VERSION}")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(X11)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

# Use exactly the same video gate and Full HD encoder as linrad_dvb.
option(RBDVBT_BUILD_SRT "Build the headless linrad_dvb SRT supervisor" ${UNIX})
if(RBDVBT_BUILD_SRT)
    if(NOT UNIX)
        message(FATAL_ERROR "RBDVBT_BUILD_SRT requires a POSIX platform")
    endif()
    get_filename_component(RBDVBT_PIPELINES_ROOT "${RBDVBT_DECODER_ROOT}/../.." ABSOLUTE)
    foreach(source srt_output.c srt_output.h process_control.h)
        if(NOT EXISTS "${RBDVBT_PIPELINES_ROOT}/src/${source}")
            message(FATAL_ERROR "SRT supervisor source not found: ${RBDVBT_PIPELINES_ROOT}/src/${source}. Update sdr-dvb-pipelines or configure -DRBDVBT_BUILD_SRT=OFF.")
        endif()
    endforeach()
    pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
    add_executable(rbdvbt_srt tools/rbdvbt_srt.c "${RBDVBT_PIPELINES_ROOT}/src/srt_output.c")
    target_include_directories(rbdvbt_srt PRIVATE "${RBDVBT_PIPELINES_ROOT}/src")
    target_link_libraries(rbdvbt_srt PRIVATE PkgConfig::GLIB)
    target_compile_options(rbdvbt_srt PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS rbdvbt_srt RUNTIME DESTINATION bin)
endif()

target_include_directories(rbdvbt_rx PRIVATE ${FFTW3F_INCLUDE_DIRS})
target_link_libraries(rbdvbt_rx PRIVATE m ${FFTW3F_LIBRARIES} Threads::Threads)
if(WIN32)
    target_link_libraries(rbdvbt_rx PRIVATE ws2_32)
endif()
if(X11_FOUND)
    target_compile_definitions(rbdvbt_rx PRIVATE RBDVBT_HAVE_X11=1)
    target_include_directories(rbdvbt_rx PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(rbdvbt_rx PRIVATE ${X11_LIBRARIES})
endif()

target_compile_options(rbdvbt_rx PRIVATE
    -O3
    -Wall
    -Wextra
    -Wpedantic
)

set(PORTSDOWN_DVBT_STACK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/other_parties/portsdown4/src/dvb_t_stack/src)
set(PORTSDOWN_DVBT_STACK_SOURCES
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_conv.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_encode.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_interleave.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_rs_encoder.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_bits.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_enc.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_i.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_linux_fft.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_lpf.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_mod.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_qam_tab.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_stab.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_sym.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvb_t_tp.cpp
    ${PORTSDOWN_DVBT_STACK_DIR}/dvbt_modulator.cpp
)

if(EXISTS "${PORTSDOWN_DVBT_STACK_DIR}/dvb_conv.cpp")
    add_executable(portsdown_iq_dump
        tools/portsdown_iq_dump.cpp
        ${PORTSDOWN_DVBT_STACK_SOURCES}
    )

    target_include_directories(portsdown_iq_dump PRIVATE
        ${PORTSDOWN_DVBT_STACK_DIR}
        ${FFTW3F_INCLUDE_DIRS}
    )
    target_link_libraries(portsdown_iq_dump PRIVATE m ${FFTW3F_LIBRARIES})

    target_compile_options(portsdown_iq_dump PRIVATE
        -Wall
        -Wextra
    )
else()
    message(STATUS "Skipping portsdown_iq_dump; external Portsdown DVB-T stack not found")
endif()

add_executable(rbdvbt_status_watch
    tools/rbdvbt_status_watch.c
)

target_compile_options(rbdvbt_status_watch PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(dvbt_fec_snr_plot
    tools/dvbt_fec_snr_plot.c
)

target_link_libraries(dvbt_fec_snr_plot PRIVATE m)

target_compile_options(dvbt_fec_snr_plot PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(iq_airscatter_channel
    tools/iq_airscatter_channel.c
)

target_link_libraries(iq_airscatter_channel PRIVATE m)

target_compile_options(iq_airscatter_channel PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(iq_replay_rate
    tools/iq_replay_rate.c
)

target_compile_options(iq_replay_rate PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

option(RBDVBT_BUILD_GUI "Build the Qt6 RTL-SDR receiver interface" ON)
if(RBDVBT_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Network Widgets)
    # Qt6 requires C++17; the C decoder and existing C++ helpers retain C11/C++11.
    add_library(rbdvbt_ui STATIC gui/receiver_window.cpp gui/plots.cpp gui/video.cpp)
    target_include_directories(rbdvbt_ui PUBLIC gui)
    target_compile_features(rbdvbt_ui PUBLIC cxx_std_17)
    target_link_libraries(rbdvbt_ui PUBLIC Qt6::Network Qt6::Widgets)
    target_compile_options(rbdvbt_ui PRIVATE -Wall -Wextra -Wpedantic)
    pkg_check_modules(LIBVLC QUIET IMPORTED_TARGET libvlc)
    if(LIBVLC_FOUND)
        target_link_libraries(rbdvbt_ui PRIVATE PkgConfig::LIBVLC)
        target_compile_definitions(rbdvbt_ui PRIVATE RBDVBT_HAVE_VLC=1)
    else()
        message(STATUS "libvlc not found: video available through an external player")
    endif()
    add_executable(rbdvbt_gui WIN32 gui/main.cpp)
    target_link_libraries(rbdvbt_gui PRIVATE rbdvbt_ui)
    target_compile_definitions(rbdvbt_gui PRIVATE RBDVBT_GUI_VERSION="${PROJECT_VERSION}")
    add_dependencies(rbdvbt_gui rbdvbt_rx)
endif()

include(CTest)
if(BUILD_TESTING AND TARGET rbdvbt_srt AND EXISTS "${RBDVBT_PIPELINES_ROOT}/tests/test_srt_output.py")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME srt_output COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_srt_output.py" $<TARGET_FILE:rbdvbt_srt>
        "${RBDVBT_PIPELINES_ROOT}/tests/test_srt_output.py")
    set_tests_properties(srt_output PROPERTIES TIMEOUT 60)
endif()
if(BUILD_TESTING)
    # Exercise the same upstream sources; never copy tests or decoder code here.
    get_filename_component(RBDVBT_PIPELINES_ROOT "${RBDVBT_DECODER_ROOT}/../.." ABSOLUTE)
    foreach(check stream psi frontend)
        set(test_source "${RBDVBT_PIPELINES_ROOT}/tests/test_dvbt_${check}.c")
        if(EXISTS "${test_source}")
            set(test_core ${RBDVBT_CORE_SOURCES})
            if(check STREQUAL "psi")
                list(REMOVE_ITEM test_core "${RBDVBT_DECODER_ROOT}/src/dvbt_outer.c")
            else()
                list(REMOVE_ITEM test_core "${RBDVBT_DECODER_ROOT}/src/probe_constellation.c")
            endif()
            add_executable(test_dvbt_${check} "${test_source}" ${test_core})
            target_include_directories(test_dvbt_${check} PRIVATE "${RBDVBT_DECODER_ROOT}/include" ${FFTW3F_INCLUDE_DIRS})
            target_compile_options(test_dvbt_${check} PRIVATE -O3 -UNDEBUG)
            target_link_libraries(test_dvbt_${check} PRIVATE m ${FFTW3F_LIBRARIES} Threads::Threads)
            if(WIN32)
                target_link_libraries(test_dvbt_${check} PRIVATE ws2_32)
            endif()
            add_test(NAME dvbt_${check} COMMAND test_dvbt_${check})
            set_tests_properties(dvbt_${check} PROPERTIES TIMEOUT 60)
        else()
            message(STATUS "Skipping upstream DVB-T ${check} test: ${test_source} not found")
        endif()
    endforeach()
    if(TARGET test_dvbt_stream)
        add_test(NAME dvbt_stream_scalar COMMAND test_dvbt_stream)
        set_tests_properties(dvbt_stream_scalar PROPERTIES ENVIRONMENT "LINRAD_DVBT_VITERBI_SCALAR=1" TIMEOUT 60)
    endif()
endif()
if(BUILD_TESTING AND RBDVBT_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    foreach(fixture rtl decoder)
        add_executable(test_${fixture}_fixture tests/pipeline_fixture.cpp)
        target_compile_features(test_${fixture}_fixture PRIVATE cxx_std_17)
        target_link_libraries(test_${fixture}_fixture PRIVATE Qt6::Network)
        set_target_properties(test_${fixture}_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ui-tests")
    endforeach()
    set_target_properties(test_decoder_fixture PROPERTIES OUTPUT_NAME rbdvbt_rx)
    add_executable(test_receiver_ui tests/test_receiver_ui.cpp)
    set_target_properties(test_receiver_ui PROPERTIES AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ui-tests")
    target_link_libraries(test_receiver_ui PRIVATE rbdvbt_ui Qt6::Test)
    add_dependencies(test_receiver_ui test_rtl_fixture test_decoder_fixture)
    add_test(NAME receiver_ui COMMAND test_receiver_ui)
    set_tests_properties(receiver_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GSETTINGS_BACKEND=memory" TIMEOUT 45)
endif()

install(TARGETS rbdvbt_rx RUNTIME DESTINATION bin)
if(RBDVBT_BUILD_GUI)
    install(TARGETS rbdvbt_gui RUNTIME DESTINATION bin)
endif()
install(FILES LICENSE NOTICE licenses/DECODER-GPL-3.0.txt DESTINATION share/rbdvbt_rx)
