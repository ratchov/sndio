/*
 * Copyright (c) 2021 Alexandre Ratchov <alex@caoua.org>
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

/*
 * This program generates a log-scale level-to-amplitude table
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int bits = 24;

/*
 * Convert double to fixed-point integer
 */
int
mkfp(double x)
{
	long long r, unit;

	unit = 1LL << (bits - 1);
	r = x * unit + ((x >= 0) ? 0.5 : -0.5);
	return r;
}

/*
 * Map an integer to the corresponding double using the log-scale
 * such that:
 *		imin -> dmin
 *		imax -> dmax
 */
double
logscale(int i, double dmin, double dmax, int imin, int imax)
{
	return dmin * pow(dmax / dmin, (double)(i - imin) / (imax - imin));
}

int
main(void)
{
	double a;
	int i;

	printf("{");
	for (i = 0; i < 128; i++) {
		if (i == 0)
			a = 0;
		else
			a = logscale(i, 0.5, 1., 109, 127);
		if (i != 0)
			printf(",");
		printf("%s", (i % 4 == 0) ? "\n\t" : " ");
		printf("%9d", mkfp(a));
	}
	printf("\n};\n");

	return 0;
}
