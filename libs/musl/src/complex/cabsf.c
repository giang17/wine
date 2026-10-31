#include "complex_impl.h"

float __cdecl cabsf(_Fcomplex z)
{
	return hypotf(crealf(z), cimagf(z));
}
