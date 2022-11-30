/*****************************************************************************\
 *                                                                           *\
 *  This file is part of the Verificarlo project,                            *\
 *  under the Apache License v2.0 with LLVM Exceptions.                      *\
 *  SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception.                 *\
 *  See https://llvm.org/LICENSE.txt for license information.                *\
 *                                                                           *\
 *                                                                           *\
 *  Copyright (c) 2015                                                       *\
 *     Universite de Versailles St-Quentin-en-Yvelines                       *\
 *     CMLA, Ecole Normale Superieure de Cachan                              *\
 *                                                                           *\
 *  Copyright (c) 2018                                                       *\
 *     Universite de Versailles St-Quentin-en-Yvelines                       *\
 *                                                                           *\
 *  Copyright (c) 2019-2022                                                  *\
 *     Verificarlo Contributors                                              *\
 *                                                                           *\
 ****************************************************************************/
// Changelog:
//
// 2021-10-13 Switched random number generator from TinyMT64 to the one
// provided by the libc. The backend is now re-entrant. Pthread and OpenMP
// threads are now supported.
// Generation of hook functions is now done through macros, shared accross
// backends.

#include <argp.h>
#include <err.h>
#include <errno.h>
#include <math.h>
#include <mpfr.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/float_const.h"
#include "common/float_struct.h"
#include "common/float_utils.h"
#include "common/generic_builtin.h"
#include "common/logger.h"
#include "common/options.h"
#include "interflop-stdlib/interflop.h"
#include "interflop-stdlib/interflop_stdlib.h"
#include "interflop-stdlib/rng/vfc_rng.h"
#include "interflop_bitmask.h"

typedef enum {
  KEY_PREC_B32,
  KEY_PREC_B64,
  KEY_MODE = 'm',
  KEY_OPERATOR = 'o',
  KEY_SEED = 's',
  KEY_DAZ = 'd',
  KEY_FTZ = 'f'
} key_args;

static const char key_prec_b32_str[] = "precision-binary32";
static const char key_prec_b64_str[] = "precision-binary64";
static const char key_mode_str[] = "mode";
static const char key_operator_str[] = "operator";
static const char key_seed_str[] = "seed";
static const char key_daz_str[] = "daz";
static const char key_ftz_str[] = "ftz";

/* string name of the bitmask modes */
static const char *BITMASK_MODE_STR[] = {[bitmask_mode_ieee] = "ieee",
                                         [bitmask_mode_full] = "full",
                                         [bitmask_mode_ib] = "ib",
                                         [bitmask_mode_ob] = "ob"};

/* string name of the bitmask */
static const char *BITMASK_OPERATOR_STR[] = {[bitmask_operator_zero] = "zero",
                                             [bitmask_operator_one] = "one",
                                             [bitmask_operator_rand] = "rand"};

/* define default environment variables and default parameters */
#define BITMASK_PRECISION_BINARY32_MIN 1
#define BITMASK_PRECISION_BINARY64_MIN 1
#define BITMASK_PRECISION_BINARY32_MAX FLOAT_PMAN_SIZE
#define BITMASK_PRECISION_BINARY64_MAX DOUBLE_PMAN_SIZE
#define BITMASK_PRECISION_BINARY32_DEFAULT FLOAT_PMAN_SIZE
#define BITMASK_PRECISION_BINARY64_DEFAULT DOUBLE_PMAN_SIZE
#define BITMASK_OPERATOR_DEFAULT bitmask_operator_zero
#define BITMASK_MODE_DEFAULT bitmask_mode_ob

#define GET_BINARYN_T(X)                                                       \
  _Generic(X, float : ctx->binary32_precision, double : ctx->binary64_precision)

/* possible op values */
typedef enum {
  bitmask_add = '+',
  bitmask_sub = '-',
  bitmask_mul = '*',
  bitmask_div = '/'
} bitmask_operations;

static float _bitmask_binary32_binary_op(float a, float b,
                                         const bitmask_operations op,
                                         void *context);
static double _bitmask_binary64_binary_op(double a, double b,
                                          const bitmask_operations op,
                                          void *context);

static uint32_t binary32_bitmask = FLOAT_MASK_ONE;
static uint64_t binary64_bitmask = DOUBLE_MASK_ONE;

#define GET_BITMASK(X)                                                         \
  _Generic(X, \
				float: binary32_bitmask,    \
				double:binary64_bitmask,    \
				float*:&binary32_bitmask,   \
				double*:&binary64_bitmask)

/******************** BITMASK CONTROL FUNCTIONS *******************
 * The following functions are used to set virtual precision and
 * BITMASK mode of operation.
 ***************************************************************/

static void _set_bitmask_mode(const bitmask_mode mode, void *context) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;
  if (mode >= _bitmask_mode_end_) {
    logger_error("--%s invalid value provided, must be one of: "
                 "{ieee, full, ib, ob}.",
                 key_mode_str);
  }
  ctx->mode = mode;
}

static void _set_bitmask_operator(const bitmask_operator bitmask,
                                  void *context) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;
  if (bitmask > _bitmask_operator_end_) {
    logger_error("--%s invalid value provided, must be one of: "
                 "{zero, one, rand}.",
                 key_operator_str);
  }
  ctx->operator= bitmask;
}

#define _set_bitmask_precision(precision, VIRTUAL_PRECISION, Y, X)             \
  {                                                                            \
    typeof(Y) *bitmask = GET_BITMASK((typeof(X) *)0);                          \
    const int32_t PREC = GET_PREC(X);                                          \
    typeof(Y) MASK_ONE = GET_MASK_ONE(X);                                      \
    *bitmask = (VIRTUAL_PRECISION <= PREC)                                     \
                   ? MASK_ONE << (PREC - VIRTUAL_PRECISION)                    \
                   : MASK_ONE;                                                 \
  }

static void _set_bitmask_precision_binary32(const int precision,
                                            void *context) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;
  _set_precision(BITMASK, precision, ctx->binary32_precision, (float)0);
  _set_bitmask_precision(precision, ctx->binary32_precision,
                         (typeof(binary32_bitmask))0, (float)0);
}

static void _set_bitmask_precision_binary64(const int precision,
                                            void *context) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;
  _set_precision(BITMASK, precision, ctx->binary64_precision, (double)0);
  _set_bitmask_precision(precision, ctx->binary64_precision,
                         (typeof(binary64_bitmask))0, (double)0);
}

/******************** BITMASK RANDOM FUNCTIONS ********************
 * The following functions are used to calculate the random bitmask
 ***************************************************************/

/* global thread identifier */
static pid_t global_tid = 0;

/* helper data structure to centralize the data used for random number
 * generation */
static __thread rng_state_t rng_state;

static uint64_t get_random_mask(void) {
  return get_rand_uint64(&rng_state, &global_tid);
}

/* Returns a 32-bits random mask */
static uint32_t get_random_binary32_mask(void) {
  binary64 mask;
  mask.u64 = get_random_mask();
  return mask.u32[0];
}

/* Returns a 64-bits random mask */
static uint64_t get_random_binary64_mask(void) {
  uint64_t mask = get_random_mask();
  return mask;
}

/* Returns a random mask depending on the type of X */
#define GET_RANDOM_MASK(X)                                                     \
  _Generic(X, float                                                            \
           : get_random_binary32_mask, double                                  \
           : get_random_binary64_mask)()

/******************** BITMASK ARITHMETIC FUNCTIONS ********************
 * The following set of functions perform the BITMASK operation. Operands
 * They apply a bitmask to the result
 *******************************************************************/

/* perform_bin_op: applies the binary operator (op) to (a) and (b) */
/* and stores the result in (res) */
#define PERFORM_BIN_OP(OP, RES, A, B)                                          \
  switch (OP) {                                                                \
  case bitmask_add:                                                            \
    RES = (A) + (B);                                                           \
    break;                                                                     \
  case bitmask_mul:                                                            \
    RES = (A) * (B);                                                           \
    break;                                                                     \
  case bitmask_sub:                                                            \
    RES = (A) - (B);                                                           \
    break;                                                                     \
  case bitmask_div:                                                            \
    RES = (A) / (B);                                                           \
    break;                                                                     \
  default:                                                                     \
    logger_error("invalid operator %c", OP);                                   \
  };

#define _MUST_NOT_BE_NOISED(X, VIRTUAL_PRECISION, MODE)                        \
  /* if mode ieee, do not introduce noise */                                   \
  (MODE == bitmask_mode_ieee) ||				\
  /* Check that we are not in a special case */				\
  (FPCLASSIFY(X) != FP_NORMAL && FPCLASSIFY(X) != FP_SUBNORMAL) ||	\
  /* In RR if the number is representable in current virtual precision, */ \
  /* do not add any noise if */						\
  (MODE == bitmask_mode_ob && _IS_REPRESENTABLE(X, VIRTUAL_PRECISION))

#define _INEXACT(CTX, B)                                                       \
  do {                                                                         \
    const typeof(B.u) sign_size = GET_SIGN_SIZE(B.type);                       \
    const typeof(B.u) exp_size = GET_EXP_SIZE(B.type);                         \
    const typeof(B.u) pman_size = GET_PMAN_SIZE(B.type);                       \
    const typeof(B.u) mask_one = GET_MASK_ONE(B.type);                         \
    const int binary_t = GET_BINARYN_T(B.type);                                \
    typeof(B.u) bitmask = GET_BITMASK(B.type);                                 \
    _init_rng_state_struct(                                                    \
        &rng_state, ((bitmask_context_t *)CTX)->choose_seed,                   \
        (unsigned long long)(((bitmask_context_t *)CTX)->seed), false);        \
    if (FPCLASSIFY(*x) == FP_SUBNORMAL) {                                      \
      /* We must use the CLZ2 variant since bitfield type                      \
           are incompatible with _Generic feature */                           \
      const typeof(B.u) leading_0 =                                            \
          CLZ2(B.u, B.ieee.mantissa) - (sign_size + exp_size);                 \
      if (pman_size < (leading_0 + binary_t)) {                                \
        bitmask = mask_one;                                                    \
      } else {                                                                 \
        bitmask |= (mask_one << (pman_size - (leading_0 + binary_t)));         \
      }                                                                        \
    }                                                                          \
    if (ctx->operator== bitmask_operator_rand) {                               \
      const typeof(B.u) rand_mask = GET_RANDOM_MASK(B.type);                   \
      B.ieee.mantissa ^= ~bitmask & rand_mask;                                 \
    } else if (ctx->operator== bitmask_operator_one) {                         \
      B.u |= ~bitmask;                                                         \
    } else if (ctx->operator== bitmask_operator_zero) {                        \
      B.u &= bitmask;                                                          \
    } else {                                                                   \
      __builtin_unreachable();                                                 \
    }                                                                          \
    *x = B.type;                                                               \
  } while (0);

static void _inexact_binary32(void *context, float *x) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;
  if (_MUST_NOT_BE_NOISED(*x, ctx->binary32_precision, ctx->mode)) {
    return;
  } else {
    binary32 b32 = {.f32 = *x};
    _INEXACT(context, b32)
  }
}

static void _inexact_binary64(void *context, double *x) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;
  if (_MUST_NOT_BE_NOISED(*x, ctx->binary64_precision, ctx->mode)) {
    return;
  } else {
    binary64 b64 = {.f64 = *x};
    _INEXACT(context, b64);
  }
}

#define _INEXACT_BINARYN(CTX, X)                                               \
  _Generic(X, float * : _inexact_binary32, double * : _inexact_binary64)(CTX, X)

#define _BITMASK_BINARY_OP(A, B, OP, CTX)                                      \
  {                                                                            \
    bitmask_context_t *TMP_CTX = (bitmask_context_t *)CTX;                     \
    typeof(A) RES = 0;                                                         \
    if (TMP_CTX->daz) {                                                        \
      A = DAZ(A);                                                              \
      B = DAZ(B);                                                              \
    }                                                                          \
    if (TMP_CTX->mode == bitmask_mode_ib ||                                    \
        TMP_CTX->mode == bitmask_mode_full) {                                  \
      _INEXACT_BINARYN(CTX, &A);                                               \
      _INEXACT_BINARYN(CTX, &B);                                               \
    }                                                                          \
    PERFORM_BIN_OP(OP, RES, A, B);                                             \
    if (TMP_CTX->mode == bitmask_mode_ob ||                                    \
        TMP_CTX->mode == bitmask_mode_full) {                                  \
      _INEXACT_BINARYN(CTX, &RES);                                             \
    }                                                                          \
    if (TMP_CTX->ftz) {                                                        \
      RES = FTZ(RES);                                                          \
    }                                                                          \
    return RES;                                                                \
  }

static float _bitmask_binary32_binary_op(float a, float b,
                                         const bitmask_operations op,
                                         void *context) {
  _BITMASK_BINARY_OP(a, b, op, context);
}

static double _bitmask_binary64_binary_op(double a, double b,
                                          const bitmask_operations op,
                                          void *context) {
  _BITMASK_BINARY_OP(a, b, op, context);
}

/******************** BITMASK COMPARE FUNCTIONS ********************
 * Compare operations do not require BITMASK
 ****************************************************************/

/************************* FPHOOKS FUNCTIONS *************************
 * These functions correspond to those inserted into the source code
 * during source to source compilation and are replacement to floating
 * point operators
 **********************************************************************/

void INTERFLOP_BITMASK_API(add_float)(float a, float b, float *res,
                                      void *context) {
  *res = _bitmask_binary32_binary_op(a, b, bitmask_add, context);
}

void INTERFLOP_BITMASK_API(sub_float)(float a, float b, float *res,
                                      void *context) {
  *res = _bitmask_binary32_binary_op(a, b, bitmask_sub, context);
}

void INTERFLOP_BITMASK_API(mul_float)(float a, float b, float *res,
                                      void *context) {
  *res = _bitmask_binary32_binary_op(a, b, bitmask_mul, context);
}

void INTERFLOP_BITMASK_API(div_float)(float a, float b, float *res,
                                      void *context) {
  *res = _bitmask_binary32_binary_op(a, b, bitmask_div, context);
}

void INTERFLOP_BITMASK_API(add_double)(double a, double b, double *res,
                                       void *context) {
  *res = _bitmask_binary64_binary_op(a, b, bitmask_add, context);
}

void INTERFLOP_BITMASK_API(sub_double)(double a, double b, double *res,
                                       void *context) {
  *res = _bitmask_binary64_binary_op(a, b, bitmask_sub, context);
}

void INTERFLOP_BITMASK_API(mul_double)(double a, double b, double *res,
                                       void *context) {
  *res = _bitmask_binary64_binary_op(a, b, bitmask_mul, context);
}

void INTERFLOP_BITMASK_API(div_double)(double a, double b, double *res,
                                       void *context) {
  *res = _bitmask_binary64_binary_op(a, b, bitmask_div, context);
}

static struct argp_option options[] = {
    {key_prec_b32_str, KEY_PREC_B32, "PRECISION", 0,
     "select precision for binary32 (PRECISION > 0)", 0},
    {key_prec_b64_str, KEY_PREC_B64, "PRECISION", 0,
     "select precision for binary64 (PRECISION > 0)", 0},
    {key_mode_str, KEY_MODE, "MODE", 0,
     "select MCA mode among {ieee, mca, pb, rr}", 0},
    {key_operator_str, KEY_OPERATOR, "OPERATOR", 0,
     "select BITMASK operator among {zero, one, rand}", 0},
    {key_seed_str, KEY_SEED, "SEED", 0, "fix the random generator seed", 0},
    {key_daz_str, KEY_DAZ, 0, 0,
     "denormals-are-zero: sets denormals inputs to zero", 0},
    {key_ftz_str, KEY_FTZ, 0, 0, "flush-to-zero: sets denormal output to zero",
     0},
    {0}};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
  bitmask_context_t *ctx = (bitmask_context_t *)state->input;
  char *endptr;
  switch (key) {
  case KEY_PREC_B32:
    /* precision for binary32 */
    errno = 0;
    int val = strtol(arg, &endptr, 10);
    if (errno != 0 || val <= 0) {
      logger_error("--%s invalid "
                   "value provided, must be a "
                   "positive integer.",
                   key_prec_b32_str);
    } else {
      _set_bitmask_precision_binary32(val, ctx);
    }
    break;
  case KEY_PREC_B64:
    /* precision for binary64 */
    errno = 0;
    val = strtol(arg, &endptr, 10);
    if (errno != 0 || val <= 0) {
      logger_error("--%s invalid "
                   "value provided, must be a "
                   "positive integer.",
                   key_prec_b64_str);
    } else {
      _set_bitmask_precision_binary64(val, ctx);
    }
    break;
  case KEY_MODE:
    /* mode */
    if (strcasecmp(BITMASK_MODE_STR[bitmask_mode_ieee], arg) == 0) {
      _set_bitmask_mode(bitmask_mode_ieee, ctx);
    } else if (strcasecmp(BITMASK_MODE_STR[bitmask_mode_full], arg) == 0) {
      _set_bitmask_mode(bitmask_mode_full, ctx);
    } else if (strcasecmp(BITMASK_MODE_STR[bitmask_mode_ib], arg) == 0) {
      _set_bitmask_mode(bitmask_mode_ib, ctx);
    } else if (strcasecmp(BITMASK_MODE_STR[bitmask_mode_ob], arg) == 0) {
      _set_bitmask_mode(bitmask_mode_ob, ctx);
    } else {
      logger_error("--%s invalid value provided, must be one of: "
                   "{ieee, full, ib, ob}.",
                   key_mode_str);
    }
    break;
  case KEY_OPERATOR:
    /* operator */
    if (strcasecmp(BITMASK_OPERATOR_STR[bitmask_operator_zero], arg) == 0) {
      _set_bitmask_operator(bitmask_operator_zero, ctx);
    } else if (strcasecmp(BITMASK_OPERATOR_STR[bitmask_operator_one], arg) ==
               0) {
      _set_bitmask_operator(bitmask_operator_one, ctx);
    } else if (strcasecmp(BITMASK_OPERATOR_STR[bitmask_operator_rand], arg) ==
               0) {
      _set_bitmask_operator(bitmask_operator_rand, ctx);
    } else {
      logger_error("--%s invalid value provided, must be "
                   "one of: "
                   "{zero, one, rand}.",
                   key_operator_str);
    }
    break;
  case KEY_SEED:
    /* set seed */
    errno = 0;
    ctx->choose_seed = true;
    ctx->seed = strtoull(arg, &endptr, 10);
    if (errno != 0) {
      logger_error("--%s invalid value provided, must be an "
                   "integer",
                   key_seed_str);
    }
    break;
  case KEY_DAZ:
    /* denormal-are-zero */
    ctx->daz = true;
    break;
  case KEY_FTZ:
    /* flush-to-zero */
    ctx->ftz = true;
    break;
  default:
    return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static struct argp argp = {options, parse_opt, "", "", NULL, NULL, NULL};

void INTERFLOP_BITMASK_API(CLI)(int argc, char **argv, void *context) {
  /* parse backend arguments */
  bitmask_context_t *ctx = (bitmask_context_t *)context;
  if (interflop_argp_parse != NULL) {
    interflop_argp_parse(&argp, argc, argv, 0, 0, ctx);
  } else {
    interflop_panic("Interflop backend error: argp_parse not implemented\n"
                    "Provide implementation or use interflop_configure to "
                    "configure the backend\n");
  }
}

static void init_context(bitmask_context_t *ctx) {
  ctx->mode = BITMASK_MODE_DEFAULT;
  ctx->operator= BITMASK_OPERATOR_DEFAULT;
  ctx->binary32_precision = BITMASK_PRECISION_BINARY32_DEFAULT;
  ctx->binary64_precision = BITMASK_PRECISION_BINARY64_DEFAULT;
  ctx->choose_seed = false;
  ctx->seed = 0ULL;
  ctx->daz = false;
  ctx->ftz = false;
}

#define CHECK_IMPL(name)                                                       \
  if (interflop_##name == Null) {                                              \
    interflop_panic("Interflop backend error: " #name " not implemented\n");   \
  }

void _bitmask_check_stdlib(void) {
  CHECK_IMPL(malloc);
  CHECK_IMPL(exit);
  CHECK_IMPL(fopen);
  CHECK_IMPL(fprintf);
  CHECK_IMPL(getenv);
  CHECK_IMPL(gettid);
  CHECK_IMPL(sprintf);
  CHECK_IMPL(strcasecmp);
  CHECK_IMPL(strerror);
  CHECK_IMPL(vfprintf);
  CHECK_IMPL(vwarnx);
}

void INTERFLOP_BITMASK_API(pre_init)(File *stream, interflop_panic_t panic,
                                     void **context) {
  interflop_set_handler("panic", panic);
  _bitmask_check_stdlib();

  /* Initialize the logger */
  logger_init(stream);

  /* allocate the context */
  bitmask_context_t *ctx =
      (bitmask_context_t *)interflop_malloc(sizeof(bitmask_context_t));
  init_context(ctx);
  *context = ctx;
}

static void print_information_header(void *context) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;

  logger_info(
      "load backend with "
      "%s = %d, "
      "%s = %d, "
      "%s = %s, "
      "%s = %s, "
      "%s = %s and "
      "%s = %s"
      "\n",
      key_prec_b32_str, ctx->binary32_precision, key_prec_b64_str,
      ctx->binary64_precision, key_mode_str, BITMASK_MODE_STR[ctx->mode],
      key_operator_str, BITMASK_OPERATOR_STR[ctx->operator], key_daz_str,
      ctx->daz ? "true" : "false", key_ftz_str, ctx->ftz ? "true" : "false");
}

struct interflop_backend_interface_t
INTERFLOP_BITMASK_API(init)(void *context) {
  bitmask_context_t *ctx = (bitmask_context_t *)context;

  print_information_header(ctx);

  struct interflop_backend_interface_t interflop_backend_bitmask = {
    interflop_add_float : INTERFLOP_BITMASK_API(add_float),
    interflop_sub_float : INTERFLOP_BITMASK_API(sub_float),
    interflop_mul_float : INTERFLOP_BITMASK_API(mul_float),
    interflop_div_float : INTERFLOP_BITMASK_API(div_float),
    interflop_cmp_float : NULL,
    interflop_add_double : INTERFLOP_BITMASK_API(add_double),
    interflop_sub_double : INTERFLOP_BITMASK_API(sub_double),
    interflop_mul_double : INTERFLOP_BITMASK_API(mul_double),
    interflop_div_double : INTERFLOP_BITMASK_API(div_double),
    interflop_cmp_double : NULL,
    interflop_cast_double_to_float : NULL,
    interflop_madd_float : NULL,
    interflop_madd_double : NULL,
    interflop_enter_function : NULL,
    interflop_exit_function : NULL,
    interflop_user_call : NULL,
    interflop_finalize : NULL
  };

  /* The seed for the RNG is initialized upon the first request for a random
     number */

  _init_rng_state_struct(&rng_state, ctx->choose_seed, ctx->seed, false);

  return interflop_backend_bitmask;
}

struct interflop_backend_interface_t interflop_init(void *context)
    __attribute__((weak, alias("interflop_bitmask_init")));

void interflop_pre_init(File *stream, interflop_panic_t panic, void **context)
    __attribute__((weak, alias("interflop_bitmask_pre_init")));

void interflop_CLI(int argc, char **argv, void *context)
    __attribute__((weak, alias("interflop_bitmask_CLI")));
