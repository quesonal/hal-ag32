# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(boot_agm_rsa)

target_sources(app PRIVATE src/main.c)

# Embed the signed-image fixtures (generated once with `imgtool sign`)
# as C arrays: the test pushes them into a simulated flash
# through the driver's own upload API, exactly like a host would.
function(agm_embed_bin out_c sym file size)
	# (${out_c} goes to the app target when it defines a symbol the driver also
	# defines weakly -- see the call for agm_boot_pubkey below.)
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
	set(AGM_LAST_GENERATED "${out_c}" PARENT_SCOPE)
endfunction()

# The driver defines `agm_boot_pubkey` weakly, and a strong definition inside
# libzephyr would not replace it: this one has to live in the app target. The
# RSA profile keeps the 270-byte PKCS#1 DER (AGM_BOOT_PUBKEY_LEN).
function(agm_embed_pubkey sym file)
	agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_pubkey.c ${sym} ${file} 270)
	target_sources(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fx_pubkey.c")
endfunction()

set(FIX ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_signed_ok.c agm_signed_ok ${FIX}/signed_ok.bin 0)
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_signed_other_key.c agm_signed_other_key ${FIX}/signed_other_key.bin 0)
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_signed_other_slot.c agm_signed_other_slot ${FIX}/signed_other_slot.bin 0)
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_raw.c agm_image_raw ${FIX}/image.bin 0)
# The ECDSA suite's signed_ok.bin, byte for byte: the *other* profile's
# container, which an RSA build has to refuse.
agm_embed_bin(${CMAKE_CURRENT_BINARY_DIR}/fx_ecdsa_signed.c agm_ecdsa_signed ${FIX}/ecdsa_signed.bin 0)
# The trusted key the driver verifies with.
agm_embed_pubkey(agm_boot_pubkey ${FIX}/pubkey.bin)
