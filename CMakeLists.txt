cmake_minimum_required(VERSION 3.16)

project(rbdvbt_rx VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rbdvbt_rx
    src/main.c
    src/config.c
    src/dvbt_2k_model.c
    src/dvbt_outer.c
    src/iq_input.c
    src/pipeline.c
    src/probe_constellation.c
    src/receiver.c
)

target_include_directories(rbdvbt_rx PRIVATE include)
target_compile_definitions(rbdvbt_rx PRIVATE RBDVBT_VERSION="${PROJECT_VERSION}")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(X11)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

target_include_directories(rbdvbt_rx PRIVATE ${FFTW3F_INCLUDE_DIRS})
target_link_libraries(rbdvbt_rx PRIVATE m ${FFTW3F_LIBRARIES} Threads::Threads)
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
