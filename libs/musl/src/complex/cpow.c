#include "complex_impl.h"

/* pow(z, c) = exp(c log(z)), See C99 G.6.4.1 */

_Dcomplex __cdecl cpow(_Dcomplex z, _Dcomplex c)
{
	_Dcomplex l;

	/* Windows returns pow() for a real, non-negative base and a real exponent. */
	if (cimag(z) == 0 && cimag(c) == 0 && creal(z) >= 0)
		return CMPLX(pow(creal(z), creal(c)), 0.0);

	l = clog(z);

	return cexp(CMPLX(creal(c) * creal(l) - cimag(c) * cimag(l),
	                  creal(c) * cimag(l) + cimag(c) * creal(l)));
}
