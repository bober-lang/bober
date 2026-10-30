/*	$OpenBSD: strtoltest.c,v 1.4 2017/07/15 17:08:26 jsing Exp $	*/
/*
 * Copyright (c) 2012 Joel Sing <jsing@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "../cases.h"

const struct int_case openbsd_strtol[] = {
	{S("1234567890"),	I64, 10, OK,	10, 1234567890},
	{S("0755"),		I64, 10, OK,	4, 755},
	{S("0x7fFFffFf"),	I64, 10, OK,	1, 0},
	{S("10101010"),		I64, 2,	OK,	8, 170},
	{S("755"),		I64, 8,	OK,	3, 493},
	{S("abc"),		I64, 10, SYNTAX, 0, 0},
	{S("123xyz"),		I64, 10, OK,	3, 123},
	{S("-080000000"),	I64, 16, OK,	10, -2147483648LL},
	{S("deadbeefdeadbeef"),	I64, 16, RANGE,	0, 0},
	{S("deadzbeef"),	I64, 16, OK,	4, 57005},
	{S("0xy"),		I64, 16, OK,	1, 0},
	{S("-quitebigmchuge"),	I64, 32, RANGE,	0, 0},
	{S("zzz"),		I64, 36, OK,	3, 46655},
};
