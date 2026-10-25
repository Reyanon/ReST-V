cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set.")
  endif()
endif()

project(restv C CXX ASM)
# Vita build. Uses the shared core (src/) at the repo root. Build: tools/build.sh vita (→ build-vita/)
set(RSTV_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "ReST-V")
set(VITA_TITLEID  "RSTV00001")
set(VITA_VERSION  "00.01")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(RSTV_ARCH_FLAGS "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${RSTV_ARCH_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${RSTV_ARCH_FLAGS} -Wall")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -ffast-math")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# UDP logger: ON by default during development (OFF for release VPK)
option(RSTV_LOG "Include UDP logger" ON)
set(RSTV_LOG_HOST "" CACHE STRING "PC IP that receives logs (empty = broadcast)")
set(RSTV_ARM_MHZ 444 CACHE STRING "ARM clock to request at startup")

# Emu core (same defines as the PC harness). Vita includes the SH-2 ARMv7 recompiler (ADR-010)
option(RSTV_DYNAREC "SH-2 dynamic recompiler" ON)
include(${RSTV_ROOT}/src/core/core.cmake)

add_executable(${PROJECT_NAME}
  src/frontend/main.c
  src/frontend/sampler.c
  src/frontend/ui.c
  src/frontend/menu.c
  src/frontend/cheats.c
  src/frontend/update.c
  src/frontend/emu.c
  src/frontend/audio.c
  src/frontend/resample.c
  src/video/gx.c
  src/video/video_gxm.c
  src/video/gvdp2.c
  src/video/nbgq.c
  src/video/rbgq.c
  src/video/v1gpu.c
  src/platform/clock.c
  src/platform/jit.c
  src/platform/log.c
)

target_include_directories(${PROJECT_NAME} PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
# Version (options About, same v0.1.x scheme as Reirem): rstv_version.h is regenerated on every build (tools/git_version.cmake)
add_custom_target(rstv_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${RSTV_ROOT} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rstv_version.h -P ${RSTV_ROOT}/tools/git_version.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/rstv_version.h
  VERBATIM)
add_dependencies(${PROJECT_NAME} rstv_version)
target_compile_definitions(${PROJECT_NAME} PRIVATE RSTV_ARM_MHZ=${RSTV_ARM_MHZ})
target_compile_definitions(rstv_core PRIVATE RSTV_VDP1_STATS=1) # VDP1 time per path (perf log v1[k])
option(RSTV_V1WATCH "diag: log suspicious VDP1 register changes to ux0:data/restv/v1watch.txt" OFF)
if(RSTV_V1WATCH)
  target_compile_definitions(rstv_core PRIVATE RSTV_V1WATCH_FILE="ux0:data/restv/v1watch.txt")
endif()
# debug-only tools (fast boot ...). Release builds: -DRSTV_DEBUG_TOOLS=OFF (owner: not in published builds)
option(RSTV_DEBUG_TOOLS "Include debug-only tools (fast boot)" ON)
if(RSTV_DEBUG_TOOLS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE RSTV_DEBUG_TOOLS)
endif()
if(RSTV_LOG)
  target_compile_definitions(${PROJECT_NAME} PRIVATE RSTV_ENABLE_LOG)
  if(RSTV_LOG_HOST)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RSTV_LOG_HOST="${RSTV_LOG_HOST}")
  endif()
endif()

# ADR-195: kernel helper module (reads the PS button), in the VPK as module/restv_kctrl.skprx. Its user stubs are made
# here directly (vita_create_stubs runs make on an unquoted path, and this tree's path has a space)
add_executable(restv_kctrl kmod/restv_kctrl.c)
set_target_properties(restv_kctrl PROPERTIES LINK_FLAGS "-nostdlib")
target_compile_options(restv_kctrl PRIVATE -fno-builtin)
target_link_libraries(restv_kctrl SceCtrlForDriver_stub SceThreadmgrForDriver_stub SceIofilemgrForDriver_stub SceSysmemForDriver_stub)
# (vita_create_self splits its flags on spaces, so the export config path cannot be passed through it)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/restv_kctrl.skprx"
  COMMAND vita-elf-create -e "${CMAKE_SOURCE_DIR}/kmod/exports.yml" $<TARGET_FILE:restv_kctrl> "${CMAKE_BINARY_DIR}/restv_kctrl.velf"
  COMMAND vita-make-fself -c "${CMAKE_BINARY_DIR}/restv_kctrl.velf" "${CMAKE_BINARY_DIR}/restv_kctrl.skprx"
  DEPENDS restv_kctrl "${CMAKE_SOURCE_DIR}/kmod/exports.yml"
  VERBATIM)
add_custom_target(kctrl_skprx ALL DEPENDS "${CMAKE_BINARY_DIR}/restv_kctrl.skprx")
set(RSTV_KSTUB_DIR "${CMAKE_BINARY_DIR}/kctrl_stubs")
set(RSTV_KSTUB "${RSTV_KSTUB_DIR}/librestv_kctrl_v7_stub_weak.a")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/kctrl.yml"
  COMMAND vita-elf-export kernel $<TARGET_FILE:restv_kctrl> "${CMAKE_SOURCE_DIR}/kmod/exports.yml" "${CMAKE_BINARY_DIR}/kctrl.yml"
  DEPENDS restv_kctrl "${CMAKE_SOURCE_DIR}/kmod/exports.yml"
  VERBATIM)
add_custom_command(OUTPUT "${RSTV_KSTUB}"
  COMMAND vita-libs-gen "${CMAKE_BINARY_DIR}/kctrl.yml" "${RSTV_KSTUB_DIR}"
  COMMAND make -C "${RSTV_KSTUB_DIR}"
  DEPENDS "${CMAKE_BINARY_DIR}/kctrl.yml"
  VERBATIM)
add_custom_target(kctrl_stubs DEPENDS "${RSTV_KSTUB}")
add_dependencies(${PROJECT_NAME} kctrl_stubs)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/kmod)

target_link_libraries(${PROJECT_NAME}
  "${RSTV_KSTUB}"
  rstv_c2
  rstv_core
  vitashark
  SceShaccCgExt
  taihen_stub
  SceShaccCg_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceDisplay_stub
  SceAppMgr_stub
  SceShellSvc_stub
  SceCommonDialog_stub
  SceIme_stub
  SceAudio_stub
  SceSysmodule_stub
  ScePgf_stub
  SceCtrl_stub
  SceTouch_stub
  SceMotion_stub
  ScePower_stub
  SceNet_stub
  SceNetCtl_stub
  SceLibKernel_stub
  stdc++
  pthread
  png
  jpeg
  mbedtls
  mbedx509
  mbedcrypto
  z
  m
  c
)

# Keep the unstripped .elf as artifacts/<commit hash>.elf on every build (core dump analysis)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DELF=$<TARGET_FILE:${PROJECT_NAME}> -DSRC=${RSTV_ROOT}
          -P ${RSTV_ROOT}/tools/archive_elf.cmake
  VERBATIM)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)

# VPK: the path has a space ("Vita Port"), so define it directly with VERBATIM instead of vita_create_vpk (same as nbbatman)
set(RSTV_SCE_SYS_FILES
  sce_sys/icon0.png
  sce_sys/livearea/contents/bg.png
  sce_sys/livearea/contents/startup.png
  sce_sys/livearea/contents/template.xml
)
# assets/blankArtwork.png: the border artwork of games without their own (owner)
set(RSTV_VPK_ADD_ARGS -a "${CMAKE_BINARY_DIR}/restv_kctrl.skprx=module/restv_kctrl.skprx"
  -a "${CMAKE_SOURCE_DIR}/sce_sys_src/blankArtwork.png=assets/blankArtwork.png"
  -a "${CMAKE_SOURCE_DIR}/res/cacert.pem=cacert.pem")
set(RSTV_VPK_DEPS "${CMAKE_BINARY_DIR}/restv_kctrl.skprx" "${CMAKE_SOURCE_DIR}/sce_sys_src/blankArtwork.png"
  "${CMAKE_SOURCE_DIR}/res/cacert.pem")
foreach(f ${RSTV_SCE_SYS_FILES})
  list(APPEND RSTV_VPK_ADD_ARGS -a "${CMAKE_SOURCE_DIR}/${f}=${f}")
  list(APPEND RSTV_VPK_DEPS "${CMAKE_SOURCE_DIR}/${f}")
endforeach()

set(RSTV_SFO "${CMAKE_BINARY_DIR}/param.sfo")
set(RSTV_VPK "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.vpk")
add_custom_command(OUTPUT "${RSTV_SFO}"
  COMMAND vita-mksfoex -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID}
          "${VITA_APP_NAME}" "${RSTV_SFO}"
  VERBATIM)
add_custom_command(OUTPUT "${RSTV_VPK}"
  COMMAND vita-pack-vpk -s "${RSTV_SFO}" -b "${CMAKE_BINARY_DIR}/eboot.bin"
          ${RSTV_VPK_ADD_ARGS} "${RSTV_VPK}"
  DEPENDS "${RSTV_SFO}" "${CMAKE_BINARY_DIR}/eboot.bin" ${RSTV_VPK_DEPS}
  VERBATIM)
add_custom_target(vpk ALL DEPENDS "${RSTV_VPK}")
