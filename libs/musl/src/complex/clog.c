#include "complex_impl.h"

// FIXME

/* log(z) = log(|z|) + i arg(z) */

_Dcomplex __cdecl clog(_Dcomplex z)
{
	double r, phi;

	r = cabs(z);
	phi = carg(z);
	/* log(0) through errno only, without the _matherr callback, as on Windows */
	if (r == 0) {
		errno = ERANGE;
		return CMPLX(-INFINITY, phi);
	}
	return CMPLX(log(r), phi);
}
