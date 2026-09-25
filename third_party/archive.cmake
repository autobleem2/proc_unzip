# proc_unzip: the two vendored libraries behind .7z and .rar, each trimmed to what reading those needs and
# built with a configuration written by hand (no configure checks - the same on every toolchain):
#
#   xz/          liblzma 5.8.4 (0BSD): decoders only - LZMA, LZMA2, delta and the branch filters a 7z uses
#   libarchive/  libarchive 3.8.9 (BSD-2-Clause): the read core and the 7-Zip, RAR (1.5-4.x) and RAR5 readers
#
# Zips stay with miniz. Everything here is compiled warnings-off, like miniz.

set(XZ ${CMAKE_CURRENT_LIST_DIR}/xz)
add_library(lzma STATIC
    ${XZ}/src/liblzma/check/check.c
    ${XZ}/src/liblzma/check/crc32_fast.c
    ${XZ}/src/liblzma/common/block_util.c
    ${XZ}/src/liblzma/common/common.c
    ${XZ}/src/liblzma/common/filter_common.c
    ${XZ}/src/liblzma/common/filter_decoder.c
    ${XZ}/src/liblzma/common/filter_flags_decoder.c
    ${XZ}/src/liblzma/common/vli_decoder.c
    ${XZ}/src/liblzma/common/vli_size.c
    ${XZ}/src/liblzma/delta/delta_common.c
    ${XZ}/src/liblzma/delta/delta_decoder.c
    ${XZ}/src/liblzma/lz/lz_decoder.c
    ${XZ}/src/liblzma/lzma/lzma_decoder.c
    ${XZ}/src/liblzma/lzma/lzma2_decoder.c
    ${XZ}/src/liblzma/lzma/lzma_encoder_presets.c
    ${XZ}/src/liblzma/simple/simple_coder.c
    ${XZ}/src/liblzma/simple/simple_decoder.c
    ${XZ}/src/liblzma/simple/arm.c
    ${XZ}/src/liblzma/simple/arm64.c
    ${XZ}/src/liblzma/simple/armthumb.c
    ${XZ}/src/liblzma/simple/ia64.c
    ${XZ}/src/liblzma/simple/powerpc.c
    ${XZ}/src/liblzma/simple/riscv.c
    ${XZ}/src/liblzma/simple/sparc.c
    ${XZ}/src/liblzma/simple/x86.c
)
target_include_directories(lzma PUBLIC ${XZ}/src/liblzma/api)
target_include_directories(lzma PRIVATE
    ${XZ}/src/common ${XZ}/src/liblzma/common ${XZ}/src/liblzma/check ${XZ}/src/liblzma/lz
    ${XZ}/src/liblzma/rangecoder ${XZ}/src/liblzma/lzma ${XZ}/src/liblzma/delta ${XZ}/src/liblzma/simple)
target_compile_definitions(lzma PUBLIC LZMA_API_STATIC)
target_compile_definitions(lzma PRIVATE
    HAVE_STDBOOL_H HAVE__BOOL HAVE_STDINT_H HAVE_INTTYPES_H HAVE_CHECK_CRC32 TUKLIB_SYMBOL_PREFIX=lzma_
    HAVE_DECODERS HAVE_DECODER_LZMA1 HAVE_DECODER_LZMA2 HAVE_DECODER_DELTA HAVE_DECODER_X86
    HAVE_DECODER_POWERPC HAVE_DECODER_IA64 HAVE_DECODER_ARM HAVE_DECODER_ARMTHUMB HAVE_DECODER_ARM64
    HAVE_DECODER_SPARC HAVE_DECODER_RISCV)
set_target_properties(lzma PROPERTIES C_STANDARD 99)
target_compile_options(lzma PRIVATE -w)

set(LA ${CMAKE_CURRENT_LIST_DIR}/libarchive)
add_library(archive STATIC
    ${LA}/ab_names.c
    ${LA}/libarchive/archive_acl.c
    ${LA}/libarchive/archive_blake2s_ref.c
    ${LA}/libarchive/archive_blake2sp_ref.c
    ${LA}/libarchive/archive_check_magic.c
    ${LA}/libarchive/archive_entry.c
    ${LA}/libarchive/archive_entry_sparse.c
    ${LA}/libarchive/archive_entry_xattr.c
    ${LA}/libarchive/archive_options.c
    ${LA}/libarchive/archive_ppmd7.c
    ${LA}/libarchive/archive_random.c
    ${LA}/libarchive/archive_rb.c
    ${LA}/libarchive/archive_read.c
    ${LA}/libarchive/archive_read_set_options.c
    ${LA}/libarchive/archive_read_support_format_7zip.c
    ${LA}/libarchive/archive_read_support_format_rar.c
    ${LA}/libarchive/archive_read_support_format_rar5.c
    ${LA}/libarchive/archive_string.c
    ${LA}/libarchive/archive_string_sprintf.c
    ${LA}/libarchive/archive_time.c
    ${LA}/libarchive/archive_util.c
    ${LA}/libarchive/archive_virtual.c
)
if (WIN32)
    target_sources(archive PRIVATE ${LA}/libarchive/archive_windows.c)
    target_link_libraries(archive PUBLIC bcrypt) # archive_util.c: BCryptGenRandom
endif()
target_include_directories(archive PUBLIC ${LA} ${LA}/libarchive)
target_compile_definitions(archive PUBLIC LIBARCHIVE_STATIC)
target_compile_definitions(archive PRIVATE PLATFORM_CONFIG_H="ab_config.h")
target_link_libraries(archive PUBLIC lzma)
set_target_properties(archive PROPERTIES C_STANDARD 99)
target_compile_options(archive PRIVATE -w)
