#include "complex_impl.h"

double __cdecl cabs(_Dcomplex z)
{
	return hypot(creal(z), cimag(z));
}
