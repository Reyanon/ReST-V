# ReST-V emulator core (YabaSanshiro libretro branch + Kronos ST-V port). Shared by Vita and PC harness.
set(RSTV_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})

set(RSTV_CORE_SOURCES
  bios.c cheat.c coffelf.c cs0.c cs1.c cs2.c cdbase.c debug.c error.c junzip.c memory.c movie.c
  netlink.c peripheral.c profile.c scspdsp.c scu.c scsp.c sh2cache.c sh2core.c sh2d.c sh2iasm.c
  sh2int.c smpc.c m68kcore.c m68kd.c m68kmusashi.c snddummy.c vidshared.c vidsoft.c yabause.c
  osdcore.c japmodem.c titan/titan.c vdp1.cpp vdp2.cpp stv.c stvio.c eeprom.c
  musashi/m68kcpu.c musashi/m68kops.c musashi/m68kopac.c musashi/m68kopdm.c musashi/m68kopnz.c
  thr-restv.c soft_glue.c Counter.cpp sega5881.c sega5838.c rstv_slave.c rstv_zip.c rstv_dspjit.c rstv_scujit.c rstv_rax.cpp
)
# SH-2 ARMv7 dynamic recompiler (devmiyax, ADR-010). Only on 32-bit ARM targets (Vita, qemu harness)
if(RSTV_DYNAREC)
  list(APPEND RSTV_CORE_SOURCES rstv_jit.c
    sh2_dynarec_devmiyax/DynarecSh2.cpp sh2_dynarec_devmiyax/DynarecSh2CInterface.cpp
    sh2_dynarec_devmiyax/dynalib_arm.s sh2_dynarec_devmiyax/rstv_memfast_arm.s)
endif()
list(TRANSFORM RSTV_CORE_SOURCES PREPEND ${RSTV_CORE_DIR}/)

add_library(rstv_core STATIC ${RSTV_CORE_SOURCES})
target_include_directories(rstv_core PUBLIC ${RSTV_CORE_DIR} ${RSTV_CORE_DIR}/musashi)
target_compile_definitions(rstv_core PUBLIC HAVE_CONFIG_H=1 HAVE_MUSASHI=1 HAVE_STDINT_H=1
  HAVE_C99_VARIADIC_MACROS=1 HAVE_BUILTIN_BSWAP16=1 HAVE_BUILTIN_BSWAP32=1 HAVE_FLOORF=1 HAVE_LROUND=1
  NO_CLI=1 RSTV=1 VERSION="ReST-V" stricmp=strcasecmp)
if(RSTV_DYNAREC)
  target_compile_definitions(rstv_core PUBLIC DYNAREC_DEVMIYAX=1 RSTV_DYNAREC=1)
endif()
target_compile_options(rstv_core PRIVATE -w $<$<COMPILE_LANGUAGE:C>:-fpermissive>)

# Sega System C / C2 (ADR-107): 68000 (Musashi, shared with the ST-V core) + VDP 315-5313 + YM3438 (ymfm) + SN76489 + uPD7759
set(RSTV_C2_DIR ${RSTV_CORE_DIR}/../c2)
add_library(rstv_c2 STATIC
  ${RSTV_C2_DIR}/c2.c ${RSTV_C2_DIR}/c2_vdp.c ${RSTV_C2_DIR}/c2_sound.c ${RSTV_C2_DIR}/c2_ym.cpp
  ${RSTV_C2_DIR}/sn76489.c ${RSTV_C2_DIR}/upd7759.c
  ${RSTV_C2_DIR}/ymfm/ymfm_opn.cpp ${RSTV_C2_DIR}/ymfm/ymfm_adpcm.cpp ${RSTV_C2_DIR}/ymfm/ymfm_ssg.cpp)
target_include_directories(rstv_c2 PUBLIC ${RSTV_C2_DIR} PRIVATE ${RSTV_C2_DIR}/ymfm)
target_link_libraries(rstv_c2 PUBLIC rstv_core)
set_source_files_properties(${RSTV_C2_DIR}/c2_ym.cpp ${RSTV_C2_DIR}/ymfm/ymfm_opn.cpp ${RSTV_C2_DIR}/ymfm/ymfm_adpcm.cpp
  ${RSTV_C2_DIR}/ymfm/ymfm_ssg.cpp PROPERTIES COMPILE_OPTIONS "-std=c++14;-fno-exceptions;-fno-rtti")
