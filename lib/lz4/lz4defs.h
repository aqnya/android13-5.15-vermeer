/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef __LZ4DEFS_H__
#define __LZ4DEFS_H__

/*
 * lz4defs.h -- common and architecture specific defines for the kernel usage
 *
 * LZ4 - Fast LZ compression algorithm
 * Copyright (C) 2011-2023, Yann Collet.
 * BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *	* Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * You can contact the author at :
 *	- LZ4 homepage : http://www.lz4.org
 *	- LZ4 source repository : https://github.com/lz4/lz4
 *
 *	Changed for kernel usage by:
 *	Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 *
 *	Updated to LZ4 v1.10.0 algorithm.
 */

#include <linux/types.h>
#include <linux/string.h>	 /* memset, memcpy */
#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/lz4.h>
#include <asm/unaligned.h>

/*-************************************
 *	Tuning parameters
 **************************************/
/*
 * LZ4_HEAPMODE :
 * Select how stateless compression functions like `LZ4_compress_default()`
 * allocate memory for their hash table, in memory stack (0:default, fastest),
 * or in memory heap (1:requires malloc()).
 */
#ifndef LZ4_HEAPMODE
#  define LZ4_HEAPMODE 0
#endif

#ifndef LZ4_ACCELERATION_DEFAULT
#  define LZ4_ACCELERATION_DEFAULT 1
#endif
#define LZ4_ACCELERATION_MAX 65537

/*
 * The kernel does not provide malloc()/calloc()/free(), and the high level
 * functions using them are not part of the kernel API, so they are disabled.
 */
#define LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION

/*-************************************
 *	Compiler Options
 **************************************/
#define LZ4_FORCE_INLINE static __always_inline
#define LZ4_FORCE_O2

#ifndef likely
#define likely(expr)	(__builtin_expect(!!(expr), 1))
#endif
#ifndef unlikely
#define unlikely(expr)	(__builtin_expect(!!(expr), 0))
#endif

/* Should the alignment test prove unreliable, it can be disabled here. */
#define LZ4_ALIGN_TEST 1

/*-************************************
 *	Error detection
 **************************************/
#ifndef assert
#  define assert(condition) ((void)0)
#endif

#define LZ4_STATIC_ASSERT(c)	BUILD_BUG_ON(!(c))

#if defined(LZ4_DEBUG) && (LZ4_DEBUG >= 2)
#  define DEBUGLOG(l, ...) {						\
	if (l <= LZ4_DEBUG)						\
		pr_debug(__FILE__ " %i: " __VA_ARGS__, __LINE__);	\
	}
#else
#  define DEBUGLOG(l, ...) {}	/* disabled */
#endif

/*-************************************
 *	Memory routines
 **************************************/
#define MEM_INIT(p, v, s)	memset((p), (v), (s))

/*
 * LZ4 relies on memcpy with a constant size being inlined. In freestanding
 * environments, the compiler can't assume the implementation of memcpy() is
 * standard compliant, so apply its specialized memcpy() inlining logic. When
 * possible, use __builtin_memcpy() to tell the compiler to analyze memcpy()
 * as-if it were standard compliant, so it can inline it in freestanding
 * environments. This is needed when decompressing the Linux Kernel, for example.
 */
#define LZ4_memcpy(dst, src, size) __builtin_memcpy(dst, src, size)
#define LZ4_memmove(dst, src, size) __builtin_memmove(dst, src, size)

/*-************************************
 *	Common Constants
 **************************************/
#define MINMATCH 4

#define WILDCOPYLENGTH 8
#define LASTLITERALS   5
#define MFLIMIT       12
/*
 * ensure it's possible to write 2 x wildcopyLength
 * without overflowing output buffer
 */
#define MATCH_SAFEGUARD_DISTANCE  ((2 * WILDCOPYLENGTH) - MINMATCH)
#define FASTLOOP_SAFE_DISTANCE 64
#define LZ4_minLength (MFLIMIT + 1)

#define KB *(1 << 10)
#define MB *(1 << 20)
#define GB *(1U << 30)

#ifndef LZ4_DISTANCE_MAX
#  define LZ4_DISTANCE_MAX 65535	/* max supported by LZ4 format */
#endif
#define LZ4_DISTANCE_ABSOLUTE_MAX 65535
#if (LZ4_DISTANCE_MAX > LZ4_DISTANCE_ABSOLUTE_MAX)
#  error "LZ4_DISTANCE_MAX is too big : must be <= 65535"
#endif

#define ML_BITS	4
#define ML_MASK	((1U << ML_BITS) - 1)
#define RUN_BITS (8 - ML_BITS)
#define RUN_MASK ((1U << RUN_BITS) - 1)

/*-************************************
 *	Types
 **************************************/
typedef	uint8_t BYTE;
typedef uint16_t U16;
typedef uint32_t U32;
typedef int32_t S32;
typedef uint64_t U64;
typedef uintptr_t uptrval;
typedef size_t reg_t;	/* native register width (kernel uses size_t) */

typedef enum {
	notLimited = 0,
	limitedOutput = 1,
	fillOutput = 2
} limitedOutput_directive;

typedef enum {
	noDict = 0,
	withPrefix64k,
	usingExtDict,
	usingDictCtx
} dict_directive;

typedef enum { noDictIssue = 0, dictSmall } dictIssue_directive;

/* defined in lz4_compress.c */
LZ4_stream_t *LZ4_initStream(void *buffer, size_t size);

/*-************************************
 *	Reading and writing into memory
 **************************************/
static __always_inline U16 LZ4_read16(const void *ptr)
{
	return get_unaligned((const U16 *)ptr);
}

static __always_inline U32 LZ4_read32(const void *ptr)
{
	return get_unaligned((const U32 *)ptr);
}

static __always_inline reg_t LZ4_read_ARCH(const void *ptr)
{
	return get_unaligned((const reg_t *)ptr);
}

static __always_inline void LZ4_write16(void *memPtr, U16 value)
{
	put_unaligned(value, (U16 *)memPtr);
}

static __always_inline void LZ4_write32(void *memPtr, U32 value)
{
	put_unaligned(value, (U32 *)memPtr);
}

static __always_inline unsigned int LZ4_isLittleEndian(void)
{
#if defined(__LITTLE_ENDIAN)
	return 1;
#else
	return 0;
#endif
}

static __always_inline U16 LZ4_readLE16(const void *memPtr)
{
	return get_unaligned_le16(memPtr);
}

static __always_inline void LZ4_writeLE16(void *memPtr, U16 value)
{
	put_unaligned_le16(value, memPtr);
}

/* customized variant of memcpy, which can overwrite up to 8 bytes beyond dstEnd */
static __always_inline void LZ4_wildCopy8(void *dstPtr, const void *srcPtr,
					  void *dstEnd)
{
	BYTE *d = (BYTE *)dstPtr;
	const BYTE *s = (const BYTE *)srcPtr;
	BYTE *const e = (BYTE *)dstEnd;

	do {
		LZ4_memcpy(d, s, 8);
		d += 8;
		s += 8;
	} while (d < e);
}

static __maybe_unused const unsigned int inc32table[8] = {
	0, 1, 2, 1, 0, 4, 4, 4
};
static __maybe_unused const int dec64table[8] = {
	0, 0, 0, -1, -4, 1, 2, 3
};

#ifndef LZ4_FAST_DEC_LOOP
#  if defined(__i386__) || defined(__x86_64__)
#    define LZ4_FAST_DEC_LOOP 1
#  elif defined(__aarch64__) && !defined(__clang__)
#    define LZ4_FAST_DEC_LOOP 1
#  else
#    define LZ4_FAST_DEC_LOOP 0
#  endif
#endif

#if LZ4_FAST_DEC_LOOP
static __always_inline void
LZ4_memcpy_using_offset_base(BYTE *dstPtr, const BYTE *srcPtr, BYTE *dstEnd,
			     const size_t offset)
{
	if (offset < 8) {
		LZ4_write32(dstPtr, 0);	/* silence an msan warning when offset==0 */
		dstPtr[0] = srcPtr[0];
		dstPtr[1] = srcPtr[1];
		dstPtr[2] = srcPtr[2];
		dstPtr[3] = srcPtr[3];
		srcPtr += inc32table[offset];
		LZ4_memcpy(dstPtr + 4, srcPtr, 4);
		srcPtr -= dec64table[offset];
		dstPtr += 8;
	} else {
		LZ4_memcpy(dstPtr, srcPtr, 8);
		dstPtr += 8;
		srcPtr += 8;
	}

	LZ4_wildCopy8(dstPtr, srcPtr, dstEnd);
}

/* customized variant of memcpy, which can overwrite up to 32 bytes beyond dstEnd
 * this version copies two times 16 bytes (instead of one time 32 bytes)
 * because it must be compatible with offsets >= 16. */
static __always_inline void LZ4_wildCopy32(void *dstPtr, const void *srcPtr,
					   void *dstEnd)
{
	BYTE *d = (BYTE *)dstPtr;
	const BYTE *s = (const BYTE *)srcPtr;
	BYTE *const e = (BYTE *)dstEnd;

	do {
		LZ4_memcpy(d, s, 16);
		LZ4_memcpy(d + 16, s + 16, 16);
		d += 32;
		s += 32;
	} while (d < e);
}

/* LZ4_memcpy_using_offset()  presumes :
 * - dstEnd >= dstPtr + MINMATCH
 * - there is at least 12 bytes available to write after dstEnd */
static __always_inline void
LZ4_memcpy_using_offset(BYTE *dstPtr, const BYTE *srcPtr, BYTE *dstEnd,
			const size_t offset)
{
	BYTE v[8];

	switch (offset) {
	case 1:
		MEM_INIT(v, *srcPtr, 8);
		break;
	case 2:
		LZ4_memcpy(v, srcPtr, 2);
		LZ4_memcpy(&v[2], srcPtr, 2);
		LZ4_memcpy(&v[4], v, 4);
		break;
	case 4:
		LZ4_memcpy(v, srcPtr, 4);
		LZ4_memcpy(&v[4], srcPtr, 4);
		break;
	default:
		LZ4_memcpy_using_offset_base(dstPtr, srcPtr, dstEnd, offset);
		return;
	}

	LZ4_memcpy(dstPtr, v, 8);
	dstPtr += 8;
	while (dstPtr < dstEnd) {
		LZ4_memcpy(dstPtr, v, 8);
		dstPtr += 8;
	}
}
#endif

/*-************************************
 *	Common functions
 **************************************/
static __always_inline unsigned int LZ4_NbCommonBytes(reg_t val)
{
#if defined(__LITTLE_ENDIAN)
	return __ffs(val) >> 3;
#else
	return (BITS_PER_LONG - 1 - __fls(val)) >> 3;
#endif
}

#define STEPSIZE sizeof(reg_t)
static __always_inline unsigned int LZ4_count(const BYTE *pIn,
					      const BYTE *pMatch,
					      const BYTE *pInLimit)
{
	const BYTE *const pStart = pIn;

	if (likely(pIn < pInLimit - (STEPSIZE - 1))) {
		reg_t const diff = LZ4_read_ARCH(pMatch) ^ LZ4_read_ARCH(pIn);

		if (!diff) {
			pIn += STEPSIZE;
			pMatch += STEPSIZE;
		} else {
			return LZ4_NbCommonBytes(diff);
		}
	}

	while (likely(pIn < pInLimit - (STEPSIZE - 1))) {
		reg_t const diff = LZ4_read_ARCH(pMatch) ^ LZ4_read_ARCH(pIn);

		if (!diff) {
			pIn += STEPSIZE;
			pMatch += STEPSIZE;
			continue;
		}
		pIn += LZ4_NbCommonBytes(diff);
		return (unsigned int)(pIn - pStart);
	}

	if ((STEPSIZE == 8) && (pIn < (pInLimit - 3))
			&& (LZ4_read32(pMatch) == LZ4_read32(pIn))) {
		pIn += 4;
		pMatch += 4;
	}
	if ((pIn < (pInLimit - 1))
			&& (LZ4_read16(pMatch) == LZ4_read16(pIn))) {
		pIn += 2;
		pMatch += 2;
	}
	if ((pIn < pInLimit) && (*pMatch == *pIn))
		pIn++;
	return (unsigned int)(pIn - pStart);
}

static __always_inline int LZ4_isAligned(const void *ptr, size_t alignment)
{
	return ((size_t)ptr & (alignment - 1)) == 0;
}

#endif
