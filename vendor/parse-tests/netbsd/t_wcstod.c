/* $NetBSD: t_wcstod.c,v 1.5 2017/07/14 14:09:53 joerg Exp $ */

/*-
 * Copyright (c) 2011 The NetBSD Foundation, Inc.
 * All rights reserved.
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

/*-
 * Copyright (c)2005 Citrus Project,
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "../cases.h"

const struct float_case netbsd_wcstod[] = {
	{S("IN"),		F64, SYNTAX, 0, 0},
	{S("+IN"),		F64, SYNTAX, 0, 0},
	{S("-IN"),		F64, SYNTAX, 1, 0},
	{S("INX"),		F64, SYNTAX, 0, 0},
	{S("-INX"),		F64, SYNTAX, 1, 0},
	{S("INF"),		F64, SYNTAX, 0, 0},
	{S("-INF"),		F64, SYNTAX, 1, 0},
	{S("INFX"),		F64, SYNTAX, 0, 0},
	{S("-INFX"),		F64, SYNTAX, 1, 0},
	{S("     IN"),		F64, SYNTAX, 0, 0},
	{S("     +IN"),		F64, SYNTAX, 0, 0},
	{S("+     INF"),	F64, SYNTAX, 0, 0},
	{S("-     INF"),	F64, SYNTAX, 1, 0},

	/* NAN */
	{S("NA"),		F64, SYNTAX, 0, 0},
	{S("-NA"),		F64, SYNTAX, 1, 0},
	{S("NAX"),		F64, SYNTAX, 0, 0},
	{S("-NAX"),		F64, SYNTAX, 1, 0},
	{S("NAN"),		F64, SYNTAX, 0, 0},
	{S("-NAN"),		F64, SYNTAX, 1, 0},
	{S("NANX"),		F64, SYNTAX, 0, 0},
	{S("-NANX"),		F64, SYNTAX, 1, 0},

	{S("0"),		F64, OK,	1, 0},
	{S("-0"),		F64, OK,	2, -0.0},

	{S("."),		F64, SYNTAX, 0, 0},
	{S(".0"),		F64, SYNTAX, 0, 0},
	{S(".00"),		F64, SYNTAX, 0, 0},
	{S(".000"),		F64, SYNTAX, 0, 0},

	{S("0."),		F64, OK,	1, 0},
	{S("-0."),		F64, OK,	2, -0.0},

	{S("0.0"),		F64, OK,	3, 0},
	{S("-0.0"),		F64, OK,	4, -0.0},

	{S("000"),		F64, OK,	3, 0},
	{S("-000"),		F64, OK,	4, -0.0},

	{S("000."),		F64, OK,	3, 0},
	{S("-000."),		F64, OK,	4, -0.0},

	{S("000.0"),		F64, OK,	5, 0},
	{S("-000.0"),		F64, OK,	6, -0.0},

	{S("0.0."),		F64, OK,	3, 0},
	{S("-0.0."),		F64, OK,	4, -0.0},

	{S("0.0.0"),		F64, OK,	3, 0},
	{S("-0.0.0"),		F64, OK,	4, -0.0},

	{S("0X"),		F64, OK,	1, 0},
	{S("-0X"),		F64, OK,	2, -0.0},

	{S("0X."),		F64, OK,	1, 0},
	{S("-0X."),		F64, OK,	2, -0.0},

	{S("0X.0"),		F64, OK,	1, 0},
	{S("-0X.0"),		F64, OK,	2, -0.0},

	{S("0X.0P"),		F64, OK,	1, 0},
	{S("-0X.0P"),		F64, OK,	2, -0.0},

	{S("0X0"),		F64, OK,	1, 0},
	{S("-0X0"),		F64, OK,	2, -0.0},

	{S("00X0.0"),		F64, OK,	2, 0},
	{S("-00X0.0"),		F64, OK,	3, -0.0},

	{S("0X0P"),		F64, OK,	1, 0},
	{S("-0X0P"),		F64, OK,	2, -0.0},

	{S("0X0."),		F64, OK,	1, 0},
	{S("-0X0."),		F64, OK,	2, -0.0},

	{S("0X0.0"),		F64, OK,	1, 0},
	{S("-0X0.0"),		F64, OK,	2, -0.0},

	{S("0X0.P"),		F64, OK,	1, 0},
	{S("-0X0.P"),		F64, OK,	2, -0.0},

	{S("0.12345678"),	F64, OK,	10, 0.12345678},
	{S("-0.12345678"),	F64, OK,	11, -0.12345678},

	{S("0.12345E67"),	F64, OK,	10, 0.12345E67},
	{S("-0.12345E67"),	F64, OK,	11, -0.12345E67},

	{S("0.12345E+6"),	F64, OK,	10, 0.12345E+6},
	{S("-0.12345E+6"),	F64, OK,	11, -0.12345E+6},

	{S("0.98765E-4"),	F64, OK,	10, 0.98765E-4},
	{S("-0.98765E-4"),	F64, OK,	11, -0.98765E-4},

	{S("12345678E9"),	F64, OK,	10, 12345678E9},
	{S("-12345678E9"),	F64, OK,	11, -12345678E9},

	{S("0x1P+2"),		F64, OK,	1, 0},
	{S("-0x1P+2"),		F64, OK,	2, -0.0},

	{S("0x1.0P+2"),		F64, OK,	1, 0},
	{S("-0x1.0P+2"),	F64, OK,	2, -0.0},

	{S("0x1P-2"),		F64, OK,	1, 0},
	{S("-0x1P-2"),		F64, OK,	2, -0.0},

	{S("0x1.0P-2"),		F64, OK,	1, 0},
	{S("-0x1.0P-2"),	F64, OK,	2, -0.0},
};
