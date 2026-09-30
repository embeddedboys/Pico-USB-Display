/* Not part of miniz: upstream generates this header from its CMake build to mark
 * the exported symbols.  The firmware links tinfl statically, so there is
 * nothing to export -- but the macro sits on every tinfl function, which makes
 * it the placement hook: CMakeLists.txt defines it to a section attribute when
 * the decode loops go to SRAM (PUD_CODEC_IN_RAM). */
#pragma once

#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif
