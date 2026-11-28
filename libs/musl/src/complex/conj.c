#include "complex_impl.h"

_Dcomplex __cdecl conj(_Dcomplex z)
{
	return CMPLX(creal(z), -cimag(z));
}
