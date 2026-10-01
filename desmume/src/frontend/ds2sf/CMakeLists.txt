cmake_minimum_required(VERSION 3.16)

# libds2sf: headless DeSmuME core + the ds2sf audio-rendering API (see ds2sf.h).
#
# Phase A ("headless"): the full emulation core as listed in frontend/posix/meson.build, minus every
# frontend/host dependency (SDL, GL, glib, pcap, OpenAL/ALSA, soundtouch) and minus the JIT. GPU
# rendering is skipped at runtime rather than removed. Phase B will shrink SOURCES further.

project(ds2sf C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ds2sf.cpp

    ${SRC}/armcpu.cpp
    ${SRC}/arm_instructions.cpp
    ${SRC}/bios.cpp
    ${SRC}/cp15.cpp
    ${SRC}/common.cpp
    ${SRC}/debug.cpp
    ${SRC}/driver.cpp
    ${SRC}/Database.cpp
    ${SRC}/emufile.cpp
    ${SRC}/encrypt.cpp
    ${SRC}/FIFO.cpp
    ${SRC}/firmware.cpp
    ${SRC}/GPU.cpp
    ${SRC}/mc.cpp
    ${SRC}/mic.cpp
    ${SRC}/path.cpp
    ${SRC}/readwrite.cpp
    ${SRC}/wifi.cpp
    ${SRC}/MMU.cpp
    ${SRC}/NDSSystem.cpp
    ${SRC}/ROMReader.cpp
    ${SRC}/render3D.cpp
    ${SRC}/rtc.cpp
    ${SRC}/saves.cpp
    ${SRC}/slot1.cpp
    ${SRC}/slot2.cpp
    ${SRC}/SPU.cpp
    ${SRC}/matrix.cpp
    ${SRC}/gfx3d.cpp
    ${SRC}/thumb_instructions.cpp
    ${SRC}/movie.cpp
    ${SRC}/cheatSystem.cpp
    ${SRC}/texcache.cpp
    ${SRC}/rasterize.cpp
    ${SRC}/version.cpp
    ${SRC}/metaspu/metaspu.cpp
    ${SRC}/frontend/modules/Disassembler.cpp

    ${SRC}/utils/advanscene.cpp
    ${SRC}/utils/datetime.cpp
    ${SRC}/utils/guid.cpp
    ${SRC}/utils/emufat.cpp
    ${SRC}/utils/fsnitro.cpp
    ${SRC}/utils/xstring.cpp
    ${SRC}/utils/decrypt/crc.cpp
    ${SRC}/utils/decrypt/decrypt.cpp
    ${SRC}/utils/decrypt/header.cpp
    ${SRC}/utils/task.cpp
    ${SRC}/utils/vfat.cpp
    ${SRC}/utils/dlditool.cpp
    ${SRC}/utils/libfat/cache.cpp
    ${SRC}/utils/libfat/directory.cpp
    ${SRC}/utils/libfat/disc.cpp
    ${SRC}/utils/libfat/fatdir.cpp
    ${SRC}/utils/libfat/fatfile.cpp
    ${SRC}/utils/libfat/filetime.cpp
    ${SRC}/utils/libfat/file_allocation_table.cpp
    ${SRC}/utils/libfat/libfat.cpp
    ${SRC}/utils/libfat/libfat_public_api.cpp
    ${SRC}/utils/libfat/lock.cpp
    ${SRC}/utils/libfat/partition.cpp
    ${SRC}/utils/tinyxml/tinystr.cpp
    ${SRC}/utils/tinyxml/tinyxml.cpp
    ${SRC}/utils/tinyxml/tinyxmlerror.cpp
    ${SRC}/utils/tinyxml/tinyxmlparser.cpp
    ${SRC}/utils/colorspacehandler/colorspacehandler.cpp

    ${SRC}/addons/slot2_auto.cpp
    ${SRC}/addons/slot2_mpcf.cpp
    ${SRC}/addons/slot2_paddle.cpp
    ${SRC}/addons/slot2_gbagame.cpp
    ${SRC}/addons/slot2_none.cpp
    ${SRC}/addons/slot2_rumblepak.cpp
    ${SRC}/addons/slot2_guitarGrip.cpp
    ${SRC}/addons/slot2_hcv1000.cpp
    ${SRC}/addons/slot2_expMemory.cpp
    ${SRC}/addons/slot2_piano.cpp
    ${SRC}/addons/slot2_passme.cpp
    ${SRC}/addons/slot1_none.cpp
    ${SRC}/addons/slot1_r4.cpp
    ${SRC}/addons/slot1_retail_nand.cpp
    ${SRC}/addons/slot1_retail_auto.cpp
    ${SRC}/addons/slot1_retail_mcrom.cpp
    ${SRC}/addons/slot1_retail_mcrom_debug.cpp
    ${SRC}/addons/slot1comp_mc.cpp
    ${SRC}/addons/slot1comp_rom.cpp
    ${SRC}/addons/slot1comp_protocol.cpp

    ${SRC}/filter/2xsai.cpp
    ${SRC}/filter/bilinear.cpp
    ${SRC}/filter/deposterize.cpp
    ${SRC}/filter/epx.cpp
    ${SRC}/filter/hq2x.cpp
    ${SRC}/filter/hq3x.cpp
    ${SRC}/filter/hq4x.cpp
    ${SRC}/filter/lq2x.cpp
    ${SRC}/filter/scanline.cpp
    ${SRC}/filter/videofilter.cpp
    ${SRC}/filter/xbrz.cpp

    ${SRC}/libretro-common/compat/compat_getopt.c
    ${SRC}/libretro-common/file/file_path.c
    ${SRC}/libretro-common/compat/compat_strl.c
    ${SRC}/libretro-common/features/features_cpu.c
    ${SRC}/libretro-common/file/retro_dirent.c
    ${SRC}/libretro-common/file/retro_stat.c
    ${SRC}/libretro-common/rthreads/async_job.c
    ${SRC}/libretro-common/rthreads/rsemaphore.c
    ${SRC}/libretro-common/rthreads/rthreads.c
    ${SRC}/libretro-common/encodings/encoding_utf.c
)

add_library(ds2sf SHARED ${SOURCES})

target_include_directories(ds2sf SYSTEM PRIVATE
    ${SRC}
    ${SRC}/libretro-common/include
    ${SRC}/frontend
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(ds2sf PRIVATE HOST_64)
else()
    target_compile_definitions(ds2sf PRIVATE HOST_32)
endif()

# HAVE_JIT is deliberately not defined: the interpreter is the accuracy reference.
target_compile_definitions(ds2sf PRIVATE DS2SF_BUILD NDEBUG DESMUME_NO_GLIB DESMUME_NO_PCAP)

target_compile_options(ds2sf PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-narrowing>)

# The vendored core is built warning-free the way upstream's frontends build it; keep warnings for
# our own frontend code.
set(CORE_SOURCES ${SOURCES})
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ds2sf.cpp)
set_source_files_properties(${CORE_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/ds2sf.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

target_link_libraries(ds2sf PRIVATE ZLIB::ZLIB Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Fail at link time, not at dlopen time, if the core needs a symbol no frontend file provides.
    target_link_options(ds2sf PRIVATE -Wl,--no-undefined)

    # Route the core's console output through ds2sf.cpp (silent unless DS2SF_VERBOSE is set).
    # Fortify is disabled so GCC doesn't turn these into __printf_chk & co., which would bypass the wrap.
    target_link_options(ds2sf PRIVATE -Wl,--wrap=printf,--wrap=puts,--wrap=putchar,--wrap=vprintf)
    target_compile_options(ds2sf PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
endif()

# Smoke test: cmake -DDS2SF_BUILD_TESTS=ON ... && cmake --build ... && ctest --test-dir <build>
# Fixtures are generated from tests/*.s, which needs Python 3 and llvm-mc/llvm-objcopy/llvm-nm.
option(DS2SF_BUILD_TESTS "Build the ds2sf smoke test and its synthetic 2SF fixtures" OFF)

if(DS2SF_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(ds2sf_smoketest tests/ds2sf_smoketest.c)
    target_link_libraries(ds2sf_smoketest PRIVATE ds2sf ZLIB::ZLIB m)

    set(FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
    add_custom_command(
        OUTPUT ${FIXTURES}/ds2sf_test.2sflib ${FIXTURES}/ds2sf_test.mini2sf
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_test_2sf.py ${FIXTURES}
        DEPENDS tests/make_test_2sf.py tests/arm9.s tests/arm7.s
        COMMENT "Assembling ds2sf test fixtures")
    add_custom_target(ds2sf_fixtures ALL DEPENDS ${FIXTURES}/ds2sf_test.2sflib ${FIXTURES}/ds2sf_test.mini2sf)

    enable_testing()
    add_test(NAME ds2sf_lib_440hz_native_rate
        COMMAND ds2sf_smoketest ${FIXTURES}/ds2sf_test.2sflib --expect-hz 439.95)
    add_test(NAME ds2sf_mini2sf_880hz_44100
        COMMAND ds2sf_smoketest ${FIXTURES}/ds2sf_test.mini2sf --rate 44100 --expect-hz 880.09)
endif()
