# SPDX-License-Identifier: Apache-2.0
#
# Shared build glue for all AgRV2K board variants (agrv2k_103,
# agrv2k_303, agrv2k_407, agrv2k_test). Each per-board board.cmake
# MUST set the following variables BEFORE including this file:
#
#   BOARD_DIR            absolute path to the per-board directory
#                        (this file does NOT set it itself, since
#                        CMAKE_CURRENT_LIST_DIR at include-time points
#                        to the per-board file's dir, not this one).
#   AGRV_FLASH_SIZE      on-die FLASH size, hex string ("0x100000",
#                        "0x40000", ...). Used in support/openocd.cfg
#                        and in flash bank definitions.
#   AGRV_FLASH_SIZE_KB   same in KB (1024 for 407, 256 for 103/303/test)
#                        for the check_device_id proc arg.
#   AGRV_BITSTREAM_ADDR  FPGA bitstream FLASH address
#                        ("0x800e7000" for 1 MB boards,
#                         "0x80027000" for 256 KB boards).
#
# The per-board board.cmake then does:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../agrv2k/shared/board_common.cmake)
#
# Everything after that point (bitstream build, openocd runner
# registration, flash + flash-logic targets, support/openocd.cfg
# generation) is shared and lives in this file.

# ---- Locate hal_ag32 tools (Supra wrapper) --------------------------
# We require BOTH build_bitstream.sh AND generate_board_ve.py to be
# present in the same directory. Older checkouts may have build_bitstream.sh
# without generate_board_ve.py (the dist→board.ve flow was added
# ), in that case we keep walking the discovery chain so the
# cmake flow still resolves to a directory that has the new script.
#
# With Option 2 the boards/ tree lives inside hal_ag32 itself
# (hal_ag32/boards/agm/agrv2k_407/), so `${BOARD_DIR}/../../tools`
# resolves to hal_ag32/tools — the new primary path. We keep the
# older workspace-relative + standalone-checkout candidates as
# fallbacks so a user who has a stale workspace symlink or an
# out-of-tree `ZEPHYR_HAL_AGM_HOME` override still gets a hit.
set(_agm_required build_bitstream.sh generate_board_ve.py)
set(_agm_candidates
	"$ENV{ZEPHYR_HAL_AGM_HOME}/tools"
	"${BOARD_DIR}/../../../tools"
	"${BOARD_DIR}/../../../../modules/hal_ag32/tools"
	"${BOARD_DIR}/../../../../../zephyr-hal-ag32/tools")
set(ZEPHYR_HAL_AGM_TOOLS "")
foreach(_cand IN LISTS _agm_candidates)
	if(NOT _cand)
		continue()
	endif()
	set(_agm_ok TRUE)
	foreach(_f IN LISTS _agm_required)
		if(NOT EXISTS "${_cand}/${_f}")
			set(_agm_ok FALSE)
			break()
		endif()
	endforeach()
	if(_agm_ok)
		set(ZEPHYR_HAL_AGM_TOOLS "${_cand}")
		break()
	endif()
endforeach()
if(NOT ZEPHYR_HAL_AGM_TOOLS)
	message(FATAL_ERROR
		"hal_ag32 tools not found. Each candidate must contain BOTH "
		"build_bitstream.sh AND generate_board_ve.py. Tried, in order: "
		"\$ZEPHYR_HAL_AGM_HOME/tools (= '$ENV{ZEPHYR_HAL_AGM_HOME}/tools'), "
		"\${BOARD_DIR}/../../../tools (= '${BOARD_DIR}/../../../tools'), "
		"\${BOARD_DIR}/../../../../modules/hal_ag32/tools (= '${BOARD_DIR}/../../../../modules/hal_ag32/tools'), "
		"\${BOARD_DIR}/../../../../../zephyr-hal-ag32/tools (= '${BOARD_DIR}/../../../../../zephyr-hal-ag32/tools'). "
		"Set ZEPHYR_HAL_AGM_HOME to a hal_ag32 checkout that has the "
		"new dist→board.ve flow.")
endif()

# The module root, for the few shared files that live outside this directory:
# the pins dtsi is dts/riscv/agm/agrv2k-pins.dtsi (it used to be reachable as a
# symlink next to this file, which checkouts that cannot create symlinks -- and
# every sync that copies files instead of links -- turned into a stale copy).
get_filename_component(ZEPHYR_HAL_AGM_MODULE_DIR "${ZEPHYR_HAL_AGM_TOOLS}" DIRECTORY)

# ---- AgRV SDK root -------------------------------------------------
# One knob for the whole PlatformIO AgRV install; the components below
# derive from it. Discovery order for the root itself:
#   1. -DAGRV_SDK_PATH=<dir>
#   2. $AGRV_SDK_PATH environment variable
#   3. ~/AgRV_pio
# The same variable is honoured by the west runners and by tools/*.sh,
# so one export configures build, flash and debug alike.
if(NOT AGRV_SDK_PATH)
	if(DEFINED ENV{AGRV_SDK_PATH} AND NOT "$ENV{AGRV_SDK_PATH}" STREQUAL "")
		set(AGRV_SDK_PATH "$ENV{AGRV_SDK_PATH}")
	else()
		set(AGRV_SDK_PATH "$ENV{HOME}/AgRV_pio")
	endif()
endif()
set(AGRV_SDK_PATH "${AGRV_SDK_PATH}" CACHE PATH
	"AgRV PlatformIO SDK root (holds packages/ and platforms/AgRV)")

# ---- AgRV-patched openocd -----------------------------------------
# Required for the `agrv` flash driver. Discovery order:
#   1. -DAGRV_OPENOCD=<path>
#   2. $AGRV_OPENOCD env var
#   3. <AGRV_SDK_PATH>/packages/tool-agrv_openocd/bin/openocd_cmd
#   4. ~/.platformio/packages/tool-agrv_openocd/bin/openocd_cmd
foreach(_cand AGRV_OPENOCD "$ENV{AGRV_OPENOCD}"
		"${AGRV_SDK_PATH}/packages/tool-agrv_openocd/bin/openocd_cmd"
		"$ENV{HOME}/.platformio/packages/tool-agrv_openocd/bin/openocd_cmd")
	if(_cand AND EXISTS "${_cand}")
		set(OPENOCD "${_cand}" CACHE FILEPATH "AgRV-patched openocd" FORCE)
		set(_agm_openocd "${_cand}")
		break()
	endif()
endforeach()

# ---- agrv32flash (UART ROM bootloader) -----------------------------
# The only path that can bring up a board whose FPGA fabric is not
# configured yet (no probe involved). Discovery order:
#   1. -DAGRV32FLASH=<path>
#   2. $AGRV32FLASH env var
#   3. <AGRV_SDK_PATH>/packages/tool-agrv_flashloader/bin/agrv32flash
foreach(_cand AGRV32FLASH "$ENV{AGRV32FLASH}"
		"${AGRV_SDK_PATH}/packages/tool-agrv_flashloader/bin/agrv32flash")
	if(_cand AND EXISTS "${_cand}")
		set(AGRV32FLASH "${_cand}" CACHE FILEPATH
			"AgRV ROM-bootloader flash tool (tool-agrv_flashloader)" FORCE)
		break()
	endif()
endforeach()

# ---- Quartus-ready logic directory generation ----------------------
# The full bitstream build needs Quartus (not bundled, Windows/Linux
# only, not auto-runnable from hal_ag32 tools). We instead generate the
# Quartus-ready `logic/` directory from the per-board's preprocessed
# devicetree: build_bitstream.sh first runs generate_board_ve.py
# (which reads the agm,agrv2k-pins node out of
# ${PROJECT_BINARY_DIR}/zephyr.dts → board.ve), then gen_vlog emits
# board.vx/.hx, Supra pre_logic.tcl emits board.asf/.qsf +
# af_quartus.tcl. User takes logic/ to a Quartus workstation, runs
# synthesis, then runs Supra gen_logic.tcl to compile the .vqm into
# board.bin, and drops the result at ${AGM_BITSTREAM_BIN}
# (default ${CMAKE_BINARY_DIR}/zephyr/board.bin).
#
# Sample-level pin overrides use the standard Zephyr devicetree overlay
# mechanism (<sample>/boards/<board>.overlay) — devicetree.cmake merges
# it into zephyr.dts automatically, so generate_board_ve.py just reads
# the final merged dts. No cmake-side plumbing needed.
#
# DEPENDS list intentionally tracks the preprocessed dts (the source
# of truth) rather than the generated board.ve, so editing any dts
# source (board.dts / pins.dtsi / sample overlay) triggers regen of
# logic/. board.ve is generated fresh each build and is gitignored —
# never edit it by hand.
# The firmware and the bitstream are two chains off one pin list (the dts):
# the firmware needs the generated pinctrl cells, the bitstream needs
# board.ve → Quartus. Only the second half needs the vendor SDK. Turning
# this off keeps `west build` a firmware-only build for a machine that has
# no AgRV install (CI, a customer porting board support) — the logic/
# directory then has to be produced elsewhere (`west build -t logic`, or
# tools/build_bitstream.sh with AGM_DTS=auto, which needs no Zephyr build).
option(AGM_LOGIC_TARGET
	"Generate the Quartus-ready logic/ directory on every build (needs the AgRV SDK tools)"
	ON)

get_filename_component(_agm_board_name "${BOARD_DIR}" NAME)

# ---- Where the generated artifacts go --------------------------------
# board.ve, board.generated.asf, the openocd cfg and the whole Quartus-ready
# logic/ tree are *build* outputs, so they live in the build directory:
# ${CMAKE_BINARY_DIR}/logic. They used to be written into the source tree
# (boards/agm/<board>/), which made two parallel twister jobs configure_file()
# and regenerate the same files at once -- one job then failed with
# "CMake Error ... configure_file: No such file or directory". Nothing
# generated here
# is read from the source tree any more; the module's tools resolve the cfg
# through AGM_OPENOCD_CFG or AGM_BUILD_DIR.
set(AGM_LOGIC_DIR ${CMAKE_BINARY_DIR}/logic)
file(MAKE_DIRECTORY ${AGM_LOGIC_DIR})

# Zephyr's openocd runner compares every --config path against the board's
# `support/` directory (path.samefile(path.dirname(cfg), support)) and raises
# FileNotFoundError when that directory is absent -- it does not only read the
# file, it needs the directory to exist. Nothing generated lives there any more
# (the cfg is in ${AGM_LOGIC_DIR}), so create the empty directory the runner
# expects; the shared template and the SDK-free minimal cfg stay where they
# are (shared/support/).
file(MAKE_DIRECTORY ${BOARD_DIR}/support)

# A hand-written <board_dir>/board.asf (board-level ASF assignments the dts
# does not model: drive strength, CFG_KEEP, WKUP config, ...) is fed to
# pre_logic.tcl by build_bitstream.sh, so editing it has to re-run the logic
# target. Only add it when it exists — ninja errors on a missing input.
set(_agm_asf_dep "")
if(EXISTS ${BOARD_DIR}/board.asf)
	list(APPEND _agm_asf_dep ${BOARD_DIR}/board.asf)
endif()

if(AGM_LOGIC_TARGET)
# Your own Verilog IP, if any: -DAGM_USER_RTL=<files> or the environment
# variable of the same name (space- or semicolon-separated). Passed to
# build_bitstream.sh, which stages the files into <build>/logic and registers
# them in the Quartus project.
#
# Or declare the IP *by name* (AGM_LOGIC_IP=<name>, the shape the vendor's
# platformio.ini uses): the sources are then taken from the application's
# own ip/ directory, <app>/ip/<name>.v and <app>/ip/<name>_core.v. An explicit
# AGM_USER_RTL always wins.
if(NOT DEFINED AGM_USER_RTL AND DEFINED AGM_LOGIC_IP AND
   NOT "${AGM_LOGIC_IP}" STREQUAL "")
	foreach(_ip_v "${AGM_LOGIC_IP}.v" "${AGM_LOGIC_IP}_core.v")
		if(EXISTS "${APPLICATION_SOURCE_DIR}/ip/${_ip_v}")
			list(APPEND AGM_USER_RTL
				"${APPLICATION_SOURCE_DIR}/ip/${_ip_v}")
		endif()
	endforeach()
	if(NOT DEFINED AGM_USER_RTL)
		message(FATAL_ERROR
			"AGM_LOGIC_IP=${AGM_LOGIC_IP} but no "
			"${APPLICATION_SOURCE_DIR}/ip/${AGM_LOGIC_IP}.v "
			"(or _core.v) exists")
	endif()
endif()

set(_agm_user_rtl "${AGM_USER_RTL}")
if("${_agm_user_rtl}" STREQUAL "" AND DEFINED ENV{AGM_USER_RTL})
	set(_agm_user_rtl "$ENV{AGM_USER_RTL}")
endif()
if(NOT "${_agm_user_rtl}" STREQUAL "")
	string(REPLACE ";" " " _agm_user_rtl "${_agm_user_rtl}")
	set(_agm_user_rtl_env "AGM_USER_RTL=${_agm_user_rtl}")
	list(APPEND _agm_user_rtl_dep ${AGM_USER_RTL})
	message(STATUS "AGM: user RTL staged into <build>/logic: ${_agm_user_rtl}")
endif()

# Which of those files is *the* IP (the one gen_vlog instantiates with -m and
# the IP-prepare step in <build>/logic_ip/ is named after). Without this,
# build_bitstream.sh falls back to the first file in AGM_USER_RTL -- right for
# the one-file case, implicit for a multi-IP wrapper (samples/dual_ip).
if(DEFINED AGM_LOGIC_IP AND NOT "${AGM_LOGIC_IP}" STREQUAL "")
	set(_agm_logic_ip_env "AGM_LOGIC_IP=${AGM_LOGIC_IP}")
endif()

add_custom_command(
	OUTPUT  ${AGM_LOGIC_DIR}/board.qsf
	COMMAND ${CMAKE_COMMAND} -E env
		AGM_DTS=${PROJECT_BINARY_DIR}/zephyr.dts
		${_agm_user_rtl_env}
		${_agm_logic_ip_env}
		${ZEPHYR_HAL_AGM_TOOLS}/build_bitstream.sh
			${BOARD_DIR}
			${AGM_LOGIC_DIR}
	DEPENDS
		${PROJECT_BINARY_DIR}/zephyr.dts
		${ZEPHYR_HAL_AGM_MODULE_DIR}/dts/riscv/agm/agrv2k-pins.dtsi
		${BOARD_DIR}/${_agm_board_name}.dts
		${ZEPHYR_HAL_AGM_TOOLS}/generate_board_ve.py
		${ZEPHYR_HAL_AGM_TOOLS}/build_bitstream.sh
		${_agm_asf_dep}
		${_agm_user_rtl_dep}
	COMMENT "Generating Quartus-ready logic/ directory from ${_agm_board_name}.dts + agrv2k-pins.dtsi"
	VERBATIM
)
add_custom_target(logic ALL DEPENDS ${AGM_LOGIC_DIR}/board.qsf)

else()
message(STATUS "AGM_LOGIC_TARGET=OFF: firmware-only build, no logic/ generation")
endif()

# ---- Bitstream path resolution -------------------------------------
# Where `west flash` looks for the FPGA bitstream. The name matches the
# Quartus TOP_LEVEL_ENTITY in <BOARD_DIR>/logic/board.qsf -- this is
# what compile_bitstream.sh writes into <LOGIC_DIR>/, and what
# `west build -t bitstream` copies into the build tree.
#
#   1. $AGM_BITSTREAM_BIN env         (explicit override; any path --
#                                      point straight at the AgRV SDK
#                                      reference bitstream without copy.
#                                      Wins over the cache so an existing
#                                      build dir can be retargeted)
#   2. -DAGM_BITSTREAM_BIN=<path> / cached value
#                                     (explicit at configure time; pass
#                                      -DAGM_BITSTREAM_BIN= (empty) to fall
#                                      back to the build-dir default)
#   3. ${CMAKE_BINARY_DIR}/zephyr/board.bin
#                                     (default; west build -t bitstream
#                                      writes here, next to zephyr.bin)
#
# Note: there is NO in-tree fallback for <BOARD_DIR>/logic/board.bin.
# That file is a Supra working output (under boards/<b>/logic/ which is
# gitignored) -- always transient, always overwritten on the next
# `west build -t bitstream`. Pointing `west flash` at it would be a
# false friend: a future dts change rebuilds logic and overwrites it
# without touching the FLASH, leaving the board on stale fabric.
#
# The result is written with FORCE: the env override has to beat a value a
# previous configure already cached, otherwise `AGM_BITSTREAM_BIN=... west
# build/flash` silently keeps writing the old bitstream: a build dir
# configured with the canonical path ignores the env var even after a
# forced cmake re-run.
if(DEFINED ENV{AGM_BITSTREAM_BIN} AND NOT "$ENV{AGM_BITSTREAM_BIN}" STREQUAL "")
	set(_agm_bs "$ENV{AGM_BITSTREAM_BIN}")
elseif(DEFINED AGM_BITSTREAM_BIN AND NOT "${AGM_BITSTREAM_BIN}" STREQUAL "")
	set(_agm_bs "${AGM_BITSTREAM_BIN}")
elseif(EXISTS "${CMAKE_BINARY_DIR}/zephyr/board.bin")
	set(_agm_bs "${CMAKE_BINARY_DIR}/zephyr/board.bin")
else()
	# Bitstream not yet generated. Pick the default build path so the
	# warning below has a concrete location, and the agrv_openocd runner
	# can warn + downgrade --skip-bitstream on west flash instead of
	# letting openocd fail mid-session.
	set(_agm_bs "${CMAKE_BINARY_DIR}/zephyr/board.bin")
endif()

if(NOT EXISTS "${_agm_bs}")
	message(STATUS
		"AGM: bitstream not yet generated -- expected at ${_agm_bs}. "
		"Run 'west build -t bitstream' (after Quartus synthesis of "
		"<build>/logic on a workstation); 'west flash' will fall "
		"back to firmware-only until it exists.")
endif()
set(AGM_BITSTREAM_BIN "${_agm_bs}" CACHE FILEPATH
	"FPGA bitstream (Supra output of 'west build -t bitstream')" FORCE)

# bitstream -- Quartus-routed netlist -> board.bin via Supra gen_logic.
# DEPENDS logic so dts / pin map changes regenerate; the user-driven
# Quartus synthesis step is the one this target does NOT do (Quartus
# runs only on the workstation). If af_quartus.tcl has not been run
# produced simulation/modelsim/<design>.vo, compile_bitstream.sh
# exits with its standard "go run quartus first" hint.
#
# Always runs when invoked (Supra is sub-second on the 407 fabric; a
# cached board.bin in the build dir gets overwritten). Users normally
# invoke this after they have re-Quartus'd, then `west flash` picks
# the result up via AGM_BITSTREAM_BIN (resolved just below).
#
# Output path is whatever the bitstream-path resolution below sets
# (default: ${CMAKE_BINARY_DIR}/zephyr/board.bin).
add_custom_target(bitstream
	COMMAND ${CMAKE_COMMAND} -E env
		AGM_BITSTREAM_BIN=${AGM_BITSTREAM_BIN}
		${ZEPHYR_HAL_AGM_TOOLS}/compile_bitstream.sh
			${AGM_LOGIC_DIR}
			${AGM_BITSTREAM_BIN}
	DEPENDS logic
	USES_TERMINAL
	COMMENT "Compiling FPGA bitstream from ${AGM_LOGIC_DIR} via Supra gen_logic (Quartus-synthesized netlist required)"
)

# ---- Flash runner selection ---------------------------------------
# Default: agrv_openocd -- the SWD runner is the only one with the full
# Plan A three-state model (`west flash` writes firmware + bitstream,
# `--skip-bitstream` / `--bitstream-only` pick one), so the common dev board
# commands need no `--runner` argument at all.
#
# If the AgRV-patched openocd is not installed, fall back to
# agrv32flash (UART ROM bootloader, no probe involved -- the bring-up
# path for a board whose fabric is not configured yet; firmware-only by
# default). If neither tool is present, keep agrv32flash and say what to
# install instead of failing later with a bare "program not found".
#
# Both -DAGRV_FLASH_RUNNER=<runner> and -DBOARD_FLASH_RUNNER=<runner>
# still win (neither is set if already defined), and
# `west flash --runner <name>` overrides per invocation -- e.g.
# `--runner agrv32flash` when there is no probe at hand.
if(NOT DEFINED AGRV_FLASH_RUNNER)
	if(_agm_openocd)
		set(AGRV_FLASH_RUNNER "agrv_openocd")
	elseif(AGRV32FLASH)
		set(AGRV_FLASH_RUNNER "agrv32flash")
		message(STATUS
			"hal_ag32: AgRV-patched openocd not found under '${AGRV_SDK_PATH}'; "
			"defaulting `west flash` to the ROM-bootloader runner "
			"(agrv32flash, firmware only).")
	else()
		set(AGRV_FLASH_RUNNER "agrv32flash")
		message(WARNING
			"hal_ag32: found neither agrv32flash nor the AgRV-patched openocd "
			"under '${AGRV_SDK_PATH}'. Flashing will fail until the AgRV "
			"PlatformIO SDK is installed and/or AGRV_SDK_PATH points at it "
			"(or AGRV32FLASH / AGRV_OPENOCD name the tools directly).")
	endif()
endif()
# The value below lands in the CMake cache, which means a build dir
# configured before this default changed keeps using the old runner.
# Say so instead of quietly ignoring the switch.
if(DEFINED BOARD_FLASH_RUNNER AND
		NOT "${BOARD_FLASH_RUNNER}" STREQUAL "${AGRV_FLASH_RUNNER}")
	message(STATUS
		"hal_ag32: BOARD_FLASH_RUNNER is cached as '${BOARD_FLASH_RUNNER}' "
		"but the board default is now '${AGRV_FLASH_RUNNER}'. Reconfigure "
		"with -DBOARD_FLASH_RUNNER=${AGRV_FLASH_RUNNER} (or a pristine "
		"build) to switch; `west flash --runner <name>` overrides per "
		"invocation.")
endif()
set(BOARD_FLASH_RUNNER "${AGRV_FLASH_RUNNER}"
	CACHE STRING "Default flash runner for agrv2k_<V>")

# ---- agrv32flash runner args --------------------------------------
# Per-board cache scope — the agrv32flash.board.cmake lives in the
# per-board dir (it's a 1-line include of shared/agrv32flash.board.cmake).
# Its board_set_flasher_ifnset(agrv32flash) is a no-op now that
# agrv_openocd is picked above; it only takes effect as the fallback.
board_runner_args(agrv32flash)
include(${BOARD_DIR}/agrv32flash.board.cmake)

# ---- support/openocd.cfg generation --------------------------------
# Render openocd.cfg.in into per-board support/openocd.cfg with
# per-board FLASH_SIZE and BITSTREAM_ADDR substituted. Generated
# at configure time; rebuilt whenever openocd.cfg.in changes.
#
# The per-board support/ directory holds only that generated file (the
# template lives here, in shared/support/), and git does not track
# directories -- so create it first. Without this, configure_file() fails
# with "No such file or directory" on a fresh checkout (`git clean -xfd`),
# which takes every build of that board down with it.
#
# Aliases for configure_file @VAR@ substitution (template uses the
# short names; per-board sets the AGRV_-prefixed names).
set(FLASH_SIZE      "${AGRV_FLASH_SIZE}")
set(BITSTREAM_ADDR  "${AGRV_BITSTREAM_ADDR}")
configure_file(
	${CMAKE_CURRENT_LIST_DIR}/support/openocd.cfg.in
	${AGM_LOGIC_DIR}/openocd.cfg
	@ONLY
)

# ---- Fallback: agrv_openocd ----------------------------------------
# Subclass of upstream openocd runner with on-board CMSIS-DAP probe
# warmup. support/openocd.cfg owns adapter/target/flash setup (via
# AgRV SDK cfg); we only set the per-invocation CLI flags here.
#
# Why agrv_openocd and not vanilla openocd?
#   The on-board probe (FW 2.1.0) does not respond to
#   GET_DESCRIPTOR(STRING) within openocd's hard-coded libusb 1s
#   timeout once it has been used at least once. The agrv_openocd
#   runner subclasses OpenOcdBinaryRunner and runs one slow pyusb
#   control transfer (5s timeout) BEFORE openocd starts, which
#   restores the probe's responsiveness for the rest of the session.
#   See tools/openocd_warmup.py.
#
# File-type: BIN. Why not ELF?
#   Upstream OpenOcdBinaryRunner HARD-CODES `load_image` for ELF files
#   (openocd.py:390) and ignores our `--cmd-load`. `load_image` writes
#   segments to their virtual addresses — but our zephyr.elf is linked
#   without CONFIG_XIP, so the single LOAD segment is in SRAM
#   (0x20000000) and nothing ever goes to FLASH. The board's ROM
#   bootloader reads firmware FROM FLASH @ 0x80000000, so we must put
#   zephyr.bin there ourselves.
#   BIN mode honours `--cmd-load` (openocd.py:417) and lets us write the
#   raw image at the FLASH base address.
#
# `--cmd-pre-load` runs AFTER cfg source + `init`, BEFORE firmware load
# — flash banks are alive and DAP is connected, so we can safely call
# check_device_id (read-only sanity check).
#
# CRITICAL ORDER: write firmware BEFORE bitstream. openocd's
# `flash write_image erase` auto-erases the entire flash bank
# (0x80000000-0x800FFFFF) to guarantee a clean landing — this
# collateral-erases any prior bitstream. We use `--cmd-post-verify`
# for the bitstream write so it runs AFTER the firmware write+verify,
# leaving the bitstream as the final state in FLASH.
#
# check_logic is intentionally NOT called here: it reads the bitstream
# at AGRV_BITSTREAM_ADDR to compute LOGIC_ALGO_SIZE_, but the bitstream
# is not yet in FLASH at pre-load time (we write it AFTER firmware).
# The proc's safety value comes from validating a pre-existing
# bitstream; since we just regenerated board.bin from board.ve via
# Supra, the check is redundant.
#
# Always register agrv_openocd (not gated by AGRV_FLASH_RUNNER), so
# `west flash --runner agrv_openocd` works regardless of which runner
# is the default. Mirrors the upstream Zephyr pattern (jlink.board.cmake
# unconditionally registers jlink).
board_runner_args(agrv_openocd
	"--file-type" "bin"
	# The board cfg is a *build* artifact now (${AGM_LOGIC_DIR}/openocd.cfg,
	# see above); without this the runner would look for it in the board
	# directory, where nothing writes it any more.
	"--config=${AGM_LOGIC_DIR}/openocd.cfg"
	# NB: must be a single token — west's runners.yaml round-trips
	# through PyYAML 1.1, which parses an unquoted bare hex literal
	# (`0x80000000`) as int and crashes argparse.
	"--flash-address=0x80000000"
	"--cmd-load" "flash write_image erase"
	"--cmd-verify" "verify_image"
	# Safety check (proc comes from SDK cfg).
	"--cmd-pre-load" "check_device_id 0x40200001:${AGRV_FLASH_SIZE_KB}KB"
	# Bitstream write — runs after firmware write+verify, so the
	# bitstream at AGRV_BITSTREAM_ADDR survives any bank-wide erase
	# the firmware step triggered. Last writer wins. NOTE: with
	# `--skip-bitstream`, the runner drops this command, leaving the
	# existing bitstream in FLASH untouched.
	"--cmd-post-verify" "flash write_image erase ${AGM_BITSTREAM_BIN} ${AGRV_BITSTREAM_ADDR}"
	# Hand the runner the same path that just went into
	# --cmd-post-verify. The runner's missing-file check gates the
	# "degrade to firmware-only" fallback, so it has to look at the
	# path that will really be written -- otherwise an explicit
	# -DAGM_BITSTREAM_BIN pointing outside the build dir would be
	# dropped as "not found".
	"--agm-bitstream-bin=${AGM_BITSTREAM_BIN}"
)
# Register agrv_openocd so west's runners list knows about it.
# openocd.board.cmake registers the upstream 'openocd' runner;
# we add the subclass separately so west flash --runner agrv_openocd
# doesn't error with "does not support runner".
board_finalize_runner_args(agrv_openocd)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
