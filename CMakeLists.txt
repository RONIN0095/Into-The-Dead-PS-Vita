cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(so_loader C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "Into the Dead")
set(VITA_TITLEID "ITDD00001")
set(VITA_VPKNAME "intothedead")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.0.198" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions
set(DATA_PATH "ux0:data/intothedead/") # Path to data (with trailing /)
set(PACKAGE_NAME "com.sidheinteractive.sif.DR")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DGAME_PATH="${DATA_PATH}" -DPACKAGE_NAME="${PACKAGE_NAME}")

# Release: the diagnostics of the port's development are left out (log only
# for warnings and errors, no frame and hitch log, no draw ring, no
# L+R+DOWN trace, no watchdog). -DNA_RELEASE=OFF builds them in again.
option(NA_RELEASE "Leave out the development diagnostics" OFF)
if (NA_RELEASE)
  add_definitions(-DNA_RELEASE)
endif()

# Port type
option(NDK_PORT "This should be on if the game is done with Android NDK (has an android_main entry point)" OFF)
if (NDK_PORT)
  add_definitions(-DNDK_PORT)
endif()

# Off: the libraries pass bionic FILE pointers around (stdout/stderr, __sF),
# which na_dynlib.c maps to newlib's streams.
option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" OFF)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL") # Preferred shader format (one of 'GLSL', 'CG', 'GXP')
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" OFF)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
  # Uncomment this for OpenGL logging:
  #add_definitions(-DDEBUG_OPENGL)
  # Uncomment this for so_util logging:
  #add_definitions(-DSO_UTIL_VERBOSE=1)
  # Uncomment this for verbose FalsoNDK logging (separate things):
  #add_definitions(-DDEBUG_POLL_AND_WAKE=1)
  #add_definitions(-DDEBUG_EPOLL=1)
  #add_definitions(-DDEBUG_PIPEFD=1)
  #add_definitions(-DDEBUG_CALLBACKS=1)
  # Space-separated flags passed to vitaGL
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 TEST=1 LOG_ERRORS=1")
else()
  # Space-separated flags passed to vitaGL
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 NO_DEBUG=1 SAFE_UNIFORMS=1 STORE_DEPTH_STENCIL=1")
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu++20 -Wno-write-strings -Wno-psabi")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math")
endif()

set(SOURCES
    source/main.c
    source/na_aaudio.c
    source/na_android.c
    source/na_apk.c
    source/na_dynlib.c
    source/na_egl.c
    source/na_gles3.c
    source/na_input.c
    source/na_java.c
    source/na_looper.c
    source/na_threads.c
    source/na_hud.c
    source/na_filecache.c
    source/na_modules.c
    source/na_shaders.c
    source/na_sles.c
    source/na_unity.c
    source/jni_objects.c
    source/utils/debugScreen.c
    source/dynlib.c
    source/java.c
    source/patch.c
    source/reimpl/bits/_ctype.c
    source/reimpl/errno.c
    source/reimpl/io.c
    source/reimpl/log.c
    source/reimpl/mem.c
    source/reimpl/pthr.c
    source/reimpl/sys.c
    source/reimpl/time64.c
    source/utils/dialog.c
    source/utils/glutil.c
    source/utils/init.c
    source/utils/logger.c
    source/utils/settings.c
    source/utils/utils.c
    lib/falso_jni/FalsoJNI.c
    lib/falso_jni/FalsoJNI_ImplBridge.c
    lib/falso_jni/FalsoJNI_Logger.c
    lib/falso_jni/converter.c
    lib/sha1/sha1.c
    lib/fios/fios.c
    lib/so_util/so_util.c)

if(NOT NDK_PORT)
list(APPEND SOURCES
  source/reimpl/asset_manager.cpp
  source/reimpl/controls.c
  source/reimpl/controls.h)
endif()

add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

if(NDK_PORT)
  add_subdirectory(lib/falso_ndk)
  add_dependencies(${CMAKE_PROJECT_NAME} FalsoNDK)
endif()

# VitaGL built from lib/vitagl via its own Makefile.
# Made this way so that testing quick adjustments to VitaGL is possible,
# as well as changing build flags in a fast and efficient way.
separate_arguments(VITAGL_MAKE_ARGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS}")

set(VITAGL_DIR ${CMAKE_SOURCE_DIR}/lib/vitagl)

# Port patches to vitaGL (see extras/patches/). They build on each other, so
# they're applied in order to a clean checkout whenever the set of patches
# changes (a stamp file in the build folder remembers what was applied).
file(GLOB VITAGL_PATCHES ${CMAKE_SOURCE_DIR}/extras/patches/vitagl-*.patch)
list(SORT VITAGL_PATCHES)
set(vitagl_patch_sig "")
foreach(patch ${VITAGL_PATCHES})
  file(SHA1 ${patch} patch_hash)
  string(APPEND vitagl_patch_sig "${patch_hash};")
endforeach()
set(VITAGL_PATCH_STAMP ${CMAKE_BINARY_DIR}/vitagl_patches.stamp)
set(vitagl_old_sig "")
if (EXISTS ${VITAGL_PATCH_STAMP})
  file(READ ${VITAGL_PATCH_STAMP} vitagl_old_sig)
endif()
if (NOT vitagl_old_sig STREQUAL vitagl_patch_sig)
  execute_process(COMMAND git checkout -- source WORKING_DIRECTORY ${VITAGL_DIR})
  foreach(patch ${VITAGL_PATCHES})
    execute_process(COMMAND git apply ${patch}
                    WORKING_DIRECTORY ${VITAGL_DIR}
                    RESULT_VARIABLE patch_result)
    if (NOT patch_result EQUAL 0)
      message(FATAL_ERROR "Could not apply ${patch} to vitaGL")
    endif()
    message(STATUS "Applied ${patch}")
  endforeach()
  file(WRITE ${VITAGL_PATCH_STAMP} "${vitagl_patch_sig}")
  # Rebuild vitaGL with the patched sources
  file(GLOB_RECURSE vitagl_patched_objs ${VITAGL_DIR}/source/*.o)
  file(REMOVE ${vitagl_patched_objs} ${VITAGL_DIR}/libvitaGL.a)
endif()
set(VITAGL_LIB ${VITAGL_DIR}/libvitaGL.a)

# Force a full rebuild of vitaGL whenever VITAGL_MAKE_FLAGS change
set(VITAGL_FLAGS_STAMP ${CMAKE_BINARY_DIR}/vitagl_flags.stamp)
set(vitagl_old_flags "")
if (EXISTS ${VITAGL_FLAGS_STAMP})
  file(READ ${VITAGL_FLAGS_STAMP} vitagl_old_flags)
endif()
if (NOT vitagl_old_flags STREQUAL VITAGL_MAKE_FLAGS)
  file(GLOB_RECURSE vitagl_stale_objs ${VITAGL_DIR}/source/*.o)
  file(REMOVE ${vitagl_stale_objs} ${VITAGL_LIB})
  file(WRITE ${VITAGL_FLAGS_STAMP} "${VITAGL_MAKE_FLAGS}")
endif()

include(ProcessorCount)
ProcessorCount(VITAGL_NPROC)
if (VITAGL_NPROC EQUAL 0)
  set(VITAGL_NPROC 4)
endif()

add_custom_target(vitaGL_build
                  COMMAND make -C ${VITAGL_DIR} -j${VITAGL_NPROC} ${VITAGL_MAKE_ARGS}
                  BYPRODUCTS ${VITAGL_LIB}
                  COMMENT "Building VitaGL"
                  VERBATIM)

add_library(vitaGL STATIC IMPORTED)
set_target_properties(vitaGL PROPERTIES
                      IMPORTED_LOCATION ${VITAGL_LIB}
                      INTERFACE_INCLUDE_DIRECTORIES ${VITAGL_DIR}/source)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_build)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

if(NDK_PORT)
  target_link_libraries(${CMAKE_PROJECT_NAME}
                        FalsoNDK
                        )
endif()

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      OpenSLES
                      sndfile
                      mp3lame
                      mpg123
                      opus
                      vorbis
                      ogg
                      vorbisenc
                      vorbisfile
                      FLAC
                      stdc++
                      vitaGL
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
# LiveArea images in extras/livearea: original art drawn by
# tools/livearea.py (no game art is published)
set(LIVEAREA_DIR "${CMAKE_SOURCE_DIR}/extras/livearea" CACHE PATH "Folder with icon0/pic0/bg0/startup.png and template.xml")

vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/cpu_present cpu_present
                ${CMAKE_SOURCE_DIR}/extras/cpu_max_freq cpu_max_freq
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/urandom urandom
                ${LIVEAREA_DIR}/icon0.png sce_sys/icon0.png
                ${LIVEAREA_DIR}/pic0.png sce_sys/pic0.png
                ${LIVEAREA_DIR}/startup.png sce_sys/livearea/contents/startup.png
                ${LIVEAREA_DIR}/bg0.png sce_sys/livearea/contents/bg0.png
                ${LIVEAREA_DIR}/template.xml sce_sys/livearea/contents/template.xml
                )

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
