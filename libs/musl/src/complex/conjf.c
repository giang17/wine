#include "complex_impl.h"

_Fcomplex __cdecl conjf(_Fcomplex z)
{
	return CMPLXF(crealf(z), -cimagf(z));
}
