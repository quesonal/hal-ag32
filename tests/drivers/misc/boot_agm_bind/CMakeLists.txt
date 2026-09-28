# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(boot_agm_bind)

target_sources(app PRIVATE src/main.c)

# The containers come from the signed-image suite's fixture directory: the
# binding fixtures *are* those containers with a BIND TLV appended
# (tools/agm_bind.py embed), which is the whole shape of the feature -- a
# signed image that only one chip may run. Sharing them keeps the two suites
# from drifting apart, and it is why app.overlay has to describe the same
# layout the fixtures were signed for (slot address 0x80030000).
set(FIX ${CMAKE_CURRENT_SOURCE_DIR}/../boot_agm_ecdsa/fixtures)

function(agm_embed_bin out_c sym file size)
	file(READ "${file}" _hex HEX)
	string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
	if(size GREATER 0)
		set(_decl "const unsigned char ${sym}[${size}]")
	else()
		string(LENGTH "${_hex}" _hex_len)
		math(EXPR _n "${_hex_len} / 2")
		set(_decl "const unsigned char ${sym}[${_n}]")
	endif()
	file(WRITE "${out_c}"
"/* Generated from ${file} -- do not edit. */
#include <stddef.h>
${_decl} = {
${_bytes}
};
const size_t ${sym}_len = sizeof(${sym});
")
	zephyr_library_sources("${out_c}")
endfunction()

# The driver defines `agm_boot_pubkey` weakly, and a strong definition inside
# libzephyr would not replace it: this one has to live in the app target.
function(agm_embed_pubkey sym file)
	agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_pubkey_${sym}.c ${sym} ${file} 64)
	target_sources(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fx_pubkey_${sym}.c")
endfunction()

agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_ok.c agm_signed_ok ${FIX}/signed_ok.bin 0)
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_bound.c agm_signed_bound ${FIX}/signed_bound.bin 0)
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_bound_other.c agm_signed_bound_other_chip
	${FIX}/signed_bound_other_chip.bin 0)
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_v2.c agm_signed_v2 ${FIX}/signed_v2.bin 0)
# 2.0.0 carrying the tag that was computed for 1.0.0: a container that was
# re-signed for a new release without being re-bound.
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_v2_stale.c agm_signed_v2_stale_bind
	${FIX}/signed_v2_stale_bind.bin 0)
# The 4 KiB sector image tools/agm_bind.py writes for the dev board salt (00..0f):
# the device-side reader is asserted against the host tool's bytes, not against
# a second copy of the format.
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_salt_sector.c agm_bind_salt_sector
	${FIX}/bind_salt_sector.bin 0)
agm_embed_pubkey(agm_boot_pubkey ${FIX}/pubkey.bin)
