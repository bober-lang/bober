/*	$OpenBSD: strtonumtest.c,v 1.1 2004/08/03 20:38:36 otto Exp $	*/
/*
 * Copyright (c) 2004 Otto Moerbeek <otto@drijf.net>
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

#include <limits.h>
#include "../cases.h"

const struct int_case openbsd_strtonum[] = {
	{S("-9223372036854775808"),		I64, 10, OK,	20, LLONG_MIN},
	{S("9223372036854775807"),		I64, 10, OK,	19, LLONG_MAX},
	{S("-9223372036854775809"),		I64, 10, RANGE,	0, 0},
	{S("9223372036854775808"),		I64, 10, RANGE,	0, 0},
	{S("1000000000000000000000000"),	I64, 10, RANGE,	0, 0},
	{S("-1000000000000000000000000"),	I64, 10, RANGE,	0, 0},
};
