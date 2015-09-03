#ifndef __LIB_KERNEL_FPRA_H
#define __LIB_KERNEL_FPRA_H

#include <stdint.h>

/* Floating-point number. */
typedef int32_t fp;

/* fp's are divided into an integral and a fractional portion. 
   We allot 1 bit for sign, POINT bits for fractional, and the rest of the 
   bits (32 - 1 - POINT) to integral. */
#define POINT 14

enum round_mode {
  TO_ZERO,
  TO_NEAREST
};

fp int_to_fp (int n);
int fp_to_int (fp x, enum round_mode mode);
fp fp_fp_add (fp x, fp y);
fp fp_fp_subtract (fp x, fp y);
fp fp_int_add (fp x, int n);
fp fp_int_subtract (fp x, int n);
fp fp_fp_mult (fp x, fp y);
fp fp_int_mult (fp x, int n);
fp fp_fp_div (fp x, fp y);
fp fp_int_div (fp x, int n);

#endif /* lib/kernel/fpra.h */
