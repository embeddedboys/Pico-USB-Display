/*
 * Not part of miniz: a stand-in for upstream's amalgamated miniz.h.
 *
 * Only the inflate half (tinfl) is vendored here, and upstream's miniz_tinfl.c
 * includes "miniz.h" -- which would pull in deflate, the zlib API and the zip
 * reader too.  This header gives tinfl the platform settings that miniz.h would
 * have worked out, and nothing else, so the vendored files stay byte-identical
 * to the 3.0.2 tag.
 *
 * Cortex-M33 (RP2350) and Cortex-M0+ (RP2040) are both little endian and 32-bit.
 * Unaligned loads are left off: the M0+ faults on them, and upstream only turns
 * them on for x86 anyway.
 */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_MALLOC

#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_HAS_64BIT_REGISTERS 0

#include "miniz_common.h"
#include "miniz_tinfl.h"
