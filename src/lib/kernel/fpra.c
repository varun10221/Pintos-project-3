#include "fpra.h"

static int ONE = 1 << POINT;

/* Convert n to floating point. */
fp
int_to_fp (int n)
{
  return n*ONE;
}

/* Convert floating point to int using the specified round_mode. */
int
fp_to_int (fp x, enum round_mode mode)
{
  if(mode == TO_ZERO)
    return x/ONE;
  else
    return (0 <= x ? (x+(ONE/2))/ONE : (x-(ONE/2))/ONE ); 
}

/* x+y, floating point. */
fp
fp_fp_add (fp x, fp y)
{
  return x + y;
}

/* x-y, floating point. */
fp
fp_fp_subtract (fp x, fp y)
{
  return x - y;
}

/* Add floating point and int. */
fp
fp_int_add (fp x, int n)
{
  return x + int_to_fp(n);
}

/* x-n, floating point and int. */
fp
fp_int_subtract (fp x, int n)
{
  return x - int_to_fp(n);
}

/* x*y, floating point. */
fp
fp_fp_mult (fp x, fp y)
{
  return ((int64_t) x * y) / ONE;
}

/* x*n, floating point and int. */
fp
fp_int_mult (fp x, int n)
{
  return x*n;
}

/* x/y, floating point. */
fp
fp_fp_div (fp x, fp y)
{
  return ((int64_t) x * ONE) / y;
}

/* x/n, floating point and int. */
fp
fp_int_div (fp x, int n)
{
  return x/n;
}
