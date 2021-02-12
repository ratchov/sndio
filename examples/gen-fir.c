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
 * This program generates coefficients for the resampling low-pass
 * filter used in sndiod and aucat.
 *
 * The coefficients correspond to the filter impulse response sampled
 * with 64 points per time unit; they are represented as 24-bit
 * fixed-point numbers.
 *
 * The filter is an ideal low-pass (sinc function), multiplied by a 8
 * time units long Blackman window. The filter cut-off frequency is
 * set at 0.75 of the Nyquist frequency (for instance, at 48kHz, the
 * cut-off will be at 18kHz, beyond audible frequencies).
 *
 * References:
 *	https://en.wikipedia.org/wiki/Sinc_filter
 *	https://en.wikipedia.org/wiki/Window_function
 *	https://ccrma.stanford.edu/~jos/resample/
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int order = 8, factor = 64, bits = 24;

/*
 * convert double to fixed-point integer
 */
int
mkfp(double x)
{
	long long r, unit;

	unit = 1LL << (bits - 1);
	r = x * unit + ((x >= 0) ? 0.5 : -0.5);
	if (r < -unit || r >= unit) {
		fprintf(stderr, "%g: sample out of range\n", x);
		exit(1);
	}
	return r;
}

/*
 * Blackman window function
 */
double
win_blackman(double a, double x)
{
	if (x < 0)
		x = -x;
	return (x >= 0.5) ? 0 :
		0.5 * (1 - a) +
		0.5 * cos(2 * M_PI * x) +
		0.5 * a * cos(4 * M_PI * x);
}

/*
 * Ideal low-pass filter response
 */
double
lowpass_sinc(double cutoff, double t)
{
	double arg;

	if (t == 0)
		return cutoff;
	arg = M_PI * t;
	return sin(cutoff * arg) / arg;
}

/*
 * Convert table index to time
 */
double
timeof(int i)
{
	return (double)(i - order * factor / 2) / factor;
}

int
main(void)
{
	double t;
	double h;
	double cutoff = 3. / 4.;
	double param = 0.16;
	int i;

	printf("{");
	for (i = 0; i <= order * factor; i++) {
		t = timeof(i);
		h = lowpass_sinc(cutoff, t) * win_blackman(param, t / order);
		if (i != 0)
			printf(",");
		printf("%s", (i % 8 == 0) ? "\n\t" : " ");
		printf("%7d", mkfp(h));
	}
	printf("\n};\n");

	return 0;
}
