/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: floating point (strtod, printf's %f and %g, sqrt), built with the float printf (LIBC_FLOAT_float) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) { double d = strtod(argc > 1 ? argv[1] : "2.5", 0); printf("%.3f %g\n", sqrt(d), d); return 0; }
