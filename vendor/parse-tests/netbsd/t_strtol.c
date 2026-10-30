/*	$NetBSD: t_strtol.c,v 1.8 2024/07/22 16:41:05 christos Exp $ */

/*-
 * Copyright (c) 2011 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jukka Ruohonen.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "../cases.h"

const struct int_case netbsd_strtol[] = {
	{S("111010110111100110100010101"),	I64, 2,	OK,	27, 123456789},
	{S("22121022020212200"),		I64, 3,	OK,	17, 123456789},
	{S("13112330310111"),			I64, 4,	OK,	14, 123456789},
	{S("223101104124"),			I64, 5,	OK,	12, 123456789},
	{S("20130035113"),			I64, 6,	OK,	11, 123456789},
	{S("3026236221"),			I64, 7,	OK,	10, 123456789},
	{S("726746425"),			I64, 8,	OK,	9, 123456789},
	{S("277266780"),			I64, 9,	OK,	9, 123456789},
	{S("123456789"),			I64, 10, OK,	9, 123456789},
	{S("63762A05"),				I64, 11, OK,	8, 123456789},
	{S("35418A99"),				I64, 12, OK,	8, 123456789},
	{S("1C767471"),				I64, 13, OK,	8, 123456789},
	{S("12579781"),				I64, 14, OK,	8, 123456789},
	{S("AC89BC9"),				I64, 15, OK,	7, 123456789},
	{S("75BCD15"),				I64, 16, OK,	7, 123456789},
	{S("1234567"),				I64, 8,	OK,	7, 342391},
	{S("01234567"),				I64, 10, OK,	8, 1234567},
	{S("0123456789"),			I64, 10, OK,	10, 123456789},
	{S("0x75bcd15"),			I64, 10, OK,	1, 0},
	{S(" 0xX"),				I64, 16, SYNTAX, 0, 0},
	{S(" 0XX"),				I64, 16, SYNTAX, 0, 0},

	{S("111010110111100110100010101"),	U64, 2,	OK,	27, 123456789},
	{S("22121022020212200"),		U64, 3,	OK,	17, 123456789},
	{S("13112330310111"),			U64, 4,	OK,	14, 123456789},
	{S("223101104124"),			U64, 5,	OK,	12, 123456789},
	{S("20130035113"),			U64, 6,	OK,	11, 123456789},
	{S("3026236221"),			U64, 7,	OK,	10, 123456789},
	{S("726746425"),			U64, 8,	OK,	9, 123456789},
	{S("277266780"),			U64, 9,	OK,	9, 123456789},
	{S("123456789"),			U64, 10, OK,	9, 123456789},
	{S("63762A05"),				U64, 11, OK,	8, 123456789},
	{S("35418A99"),				U64, 12, OK,	8, 123456789},
	{S("1C767471"),				U64, 13, OK,	8, 123456789},
	{S("12579781"),				U64, 14, OK,	8, 123456789},
	{S("AC89BC9"),				U64, 15, OK,	7, 123456789},
	{S("75BCD15"),				U64, 16, OK,	7, 123456789},
	{S("1234567"),				U64, 8,	OK,	7, 342391},
	{S("01234567"),				U64, 10, OK,	8, 1234567},
	{S("0123456789"),			U64, 10, OK,	10, 123456789},
	{S("0x75bcd15"),			U64, 10, OK,	1, 0},
	{S(" 0xX"),				U64, 16, SYNTAX, 0, 0},
	{S(" 0XX"),				U64, 16, SYNTAX, 0, 0},

	{S("abcd"),				I64, 16, OK,	4, 0xabcd},
	{S("     dcba"),			I64, 16, SYNTAX, 0, 0},
	{S("abcd dcba"),			I64, 16, OK,	4, 0xabcd},
	{S("abc0x123"),				I64, 16, OK,	4, 0xabc0},
	{S("abcd\0x123"),			I64, 16, OK,	4, 0xabcd},
	{S("ABCD"),				I64, 16, OK,	4, 0xabcd},
	{S("aBcD"),				I64, 16, OK,	4, 0xabcd},
	{S("0xABCD"),				I64, 16, OK,	1, 0},
	{S("0xABCDX"),				I64, 16, OK,	1, 0},

	{S("1000000000000000000000"),		I64, 8,	RANGE,	0, 0},
	{S("9223372036854775808"),		I64, 10, RANGE,	0, 0},
	{S("8000000000000000"),			I64, 16, RANGE,	0, 0},
	{S("20000000000"),			I32, 8,	RANGE,	0, 0},
	{S("2147483648"),			I32, 10, RANGE,	0, 0},
	{S("80000000"),				I32, 16, RANGE,	0, 0},

	{S("1"),				I64, 10, OK,	1, 1},
	{S(" 2"),				I64, 10, SYNTAX, 0, 0},
	{S("  3"),				I64, 10, SYNTAX, 0, 0},
	{S(" -3"),				I64, 10, SYNTAX, 0, 0},
	{S("--1"),				I64, 10, SYNTAX, 1, 0},
	{S("+-2"),				I64, 10, SYNTAX, 0, 0},
	{S("++3"),				I64, 10, SYNTAX, 0, 0},
	{S("+9"),				I64, 10, SYNTAX, 0, 0},
	{S("+123"),				I64, 10, SYNTAX, 0, 0},
	{S("-1 3"),				I64, 10, OK,	2, -1},
	{S("-1.3"),				I64, 10, OK,	2, -1},
	{S("-  3"),				I64, 10, SYNTAX, 1, 0},
	{S("+33."),				I64, 10, SYNTAX, 0, 0},
	{S("30x0"),				I64, 10, OK,	2, 30},
};
