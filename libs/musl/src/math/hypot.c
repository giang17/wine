#include <math.h>
#include <stdint.h>
#include <float.h>
#include "libm.h"

#if FLT_EVAL_METHOD > 1U && LDBL_MANT_DIG == 64
#define SPLIT (0x1p32 + 1)
#else
#define SPLIT (0x1p27 + 1)
#endif

static void sq(double_t *hi, double_t *lo, double x)
{
	double_t xh, xl, xc;

	xc = (double_t)x*SPLIT;
	xh = x - xc + xc;
	xl = x - xh;
	*hi = (double_t)x*x;
	*lo = xh*xh - *hi + 2*xh*xl + xl*xl;
}

static double hypot_nocheck(double x, double y)
{
	union {double f; uint64_t i;} ux = {x}, uy = {y}, ut;
	int ex, ey;
	double_t hx, lx, hy, ly, z;

	/* arrange |x| >= |y| */
	ux.i &= -1ULL>>1;
	uy.i &= -1ULL>>1;
	if (ux.i < uy.i) {
		ut = ux;
		ux = uy;
		uy = ut;
	}

	/* special cases */
	ex = ux.i>>52;
	ey = uy.i>>52;
	x = ux.f;
	y = uy.f;
	/* note: hypot(inf,nan) == inf */
	if (ey == 0x7ff)
		return y;
	if (ex == 0x7ff || uy.i == 0)
		return x;
	/* note: hypot(x,y) ~= x + y*y/x/2 with inexact for small y/x */
	/* 64 difference is enough for ld80 double_t */
	if (ex - ey > 64)
		return x + y;

	/* precise sqrt argument in nearest rounding mode without overflow */
	/* xh*xh must not overflow and xl*xl must not underflow in sq */
	z = 1;
	if (ex > 0x3ff+510) {
		z = 0x1p700;
		x *= 0x1p-700;
		y *= 0x1p-700;
	} else if (ey < 0x3ff-450) {
		z = 0x1p-700;
		x *= 0x1p700;
		y *= 0x1p700;
	}
	sq(&hx, &lx, x);
	sq(&hy, &ly, y);
	return z*sqrt(ly+lx+hy+hx);
}

/* Windows (ucrtbase and msvcrt.dll, Windows 10) reports an overflow through
 * _matherr and errno on both architectures and, on i386 only, an underflow
 * through _matherr for every subnormal result, an exact one included.  The
 * _matherr record carries the magnitudes scaled the way the C runtime scales
 * them before it squares: by 2^-600 on x86_64, by the larger one on i386.
 * Inf and NaN arguments are not reported. */
double __cdecl _hypot(double x, double y)
{
	/* round to double before testing: on i386 the x87 keeps the finite
	 * 2^700 * sqrt(...) product in the register, where it is not inf yet */
	double ax = fabs(x), ay = fabs(y), r = fp_barrier(hypot_nocheck(x, y));
#ifdef __i386__
	double m = ax > ay ? ax : ay;

	if (r == INFINITY && isfinite(ax) && isfinite(ay))
		return math_error(_OVERFLOW, "_hypot", ax / m, ay / m, r);
	if (r && r < DBL_MIN)
		return math_error(_UNDERFLOW, "_hypot", ax / m, ay / m, r);
#else
	if (r == INFINITY && isfinite(ax) && isfinite(ay))
		return math_error(_OVERFLOW, "_hypot", ax * 0x1p-600, ay * 0x1p-600, r);
#endif
	return r;
}
