# SPDX-License-Identifier: GPL-3.0-only
# Chipbox (Synthwave edition): the Dinghy family glue over cores/Chipbox (this repository's chipbox/ folder), on its own
# host shim (tests/host.c, CHIPBOX_CORE). Its main loop returns after each pass (CHIPBOX_STEP), so it builds as an .rfm1.
# The headers in firmware/gen are committed (NO_GEN). The .pool section attribute becomes an alignment (FLAGS).
include(${CMAKE_SOURCE_DIR}/core-glue/dinghy/dinghy.cmake)
fm1_add_core(chipbox ${CMAKE_CURRENT_LIST_DIR} SOURCE ${CMAKE_SOURCE_DIR}/cores/Chipbox
             UNIT core_chipbox.c DESC core_chipbox_desc.c FAMILY dinghy LICENSE GPL-3.0-only
             CREDITS "Chipbox by pbarilla, Synthwave edition, on Dinghy by zvenson (Felucca by Leo Kuroshita, Hügelton Instruments, with SLOOP's changes by isod89), GPL-3.0-only. emu2413 (Mitsutaka Okazaki), MIT. Terminus Font (Dimitar Toshkov Zhekov), SIL OFL 1.1. The full notices: LICENSING.md and assets/licenses/."
             NO_GEN VERSION 1.6.1-synthwave
             INCLUDES ${CMAKE_SOURCE_DIR}/cores/Chipbox/firmware/src ${CMAKE_SOURCE_DIR}/cores/Chipbox/firmware/gen ${CMAKE_SOURCE_DIR}/core-glue/dinghy ${CMAKE_CURRENT_LIST_DIR}
             FLAGS "-Dsection(x)=aligned(4)")
dinghy_core(chipbox core_chipbox.c)
fm1_add_rfm1(chipbox AUTHORS "pbarilla" "ghjob360" MEMORY_MIB 16)
# the golden check: the same script on Chipbox's own host sim (tests/host.c), the screens compared at 2500 ms
dinghy_golden_check(chipbox SCRIPT ../chipbox/golden_chipbox.txt AT 2500 SOURCE ${CMAKE_SOURCE_DIR}/cores/Chipbox/firmware/src/chipbox.c
                    DEFINES CHIPBOX_HOST APP_VER="1.6.1-synthwave"
                    INCLUDES ${CMAKE_SOURCE_DIR}/cores/Chipbox/firmware/src ${CMAKE_SOURCE_DIR}/cores/Chipbox/firmware/gen
                    FLAGS "-Dsection(x)=aligned(4)")
