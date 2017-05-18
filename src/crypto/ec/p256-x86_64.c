/* Copyright (c) 2014, Intel Corporation.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

/* Developers and authors:
 * Shay Gueron (1, 2), and Vlad Krasnov (1)
 * (1) Intel Corporation, Israel Development Center
 * (2) University of Haifa
 * Reference:
 * S.Gueron and V.Krasnov, "Fast Prime Field Elliptic Curve Cryptography with
 *                          256 Bit Primes" */

#include <openssl/ec.h>

#include <stdint.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>

#include "../bn/internal.h"
#include "../ec/internal.h"
#include "../internal.h"


#if !defined(OPENSSL_NO_ASM) && defined(OPENSSL_X86_64) && \
    !defined(OPENSSL_SMALL)

#if BN_BITS2 != 64
#define TOBN(hi, lo) lo, hi
#else
#define TOBN(hi, lo) ((BN_ULONG)hi << 32 | lo)
#endif

#if defined(__GNUC__)
#define ALIGN32 __attribute((aligned(32)))
#elif defined(_MSC_VER)
#define ALIGN32 __declspec(align(32))
#else
#define ALIGN32
#endif

#define ALIGNPTR(p, N) ((uint8_t *)p + N - (size_t)p % N)
#define P256_LIMBS (256 / BN_BITS2)

typedef struct {
  BN_ULONG X[P256_LIMBS];
  BN_ULONG Y[P256_LIMBS];
  BN_ULONG Z[P256_LIMBS];
} P256_POINT;

typedef struct {
  BN_ULONG X[P256_LIMBS];
  BN_ULONG Y[P256_LIMBS];
} P256_POINT_AFFINE;

typedef P256_POINT_AFFINE PRECOMP256_ROW[64];

/* Functions implemented in assembly */

/* Modular mul by 2: res = 2*a mod P */
void ecp_nistz256_mul_by_2(BN_ULONG res[P256_LIMBS],
                           const BN_ULONG a[P256_LIMBS]);
/* Modular div by 2: res = a/2 mod P */
void ecp_nistz256_div_by_2(BN_ULONG res[P256_LIMBS],
                           const BN_ULONG a[P256_LIMBS]);
/* Modular mul by 3: res = 3*a mod P */
void ecp_nistz256_mul_by_3(BN_ULONG res[P256_LIMBS],
                           const BN_ULONG a[P256_LIMBS]);
/* Modular add: res = a+b mod P */
void ecp_nistz256_add(BN_ULONG res[P256_LIMBS], const BN_ULONG a[P256_LIMBS],
                      const BN_ULONG b[P256_LIMBS]);
/* Modular sub: res = a-b mod P */
void ecp_nistz256_sub(BN_ULONG res[P256_LIMBS], const BN_ULONG a[P256_LIMBS],
                      const BN_ULONG b[P256_LIMBS]);
/* Modular neg: res = -a mod P */
void ecp_nistz256_neg(BN_ULONG res[P256_LIMBS], const BN_ULONG a[P256_LIMBS]);
/* Montgomery mul: res = a*b*2^-256 mod P */
void ecp_nistz256_mul_mont(BN_ULONG res[P256_LIMBS],
                           const BN_ULONG a[P256_LIMBS],
                           const BN_ULONG b[P256_LIMBS]);
/* Montgomery sqr: res = a*a*2^-256 mod P */
void ecp_nistz256_sqr_mont(BN_ULONG res[P256_LIMBS],
                           const BN_ULONG a[P256_LIMBS]);
/* Convert a number from Montgomery domain, by multiplying with 1 */
void ecp_nistz256_from_mont(BN_ULONG res[P256_LIMBS],
                            const BN_ULONG in[P256_LIMBS]);
/* Convert a number to Montgomery domain, by multiplying with 2^512 mod P*/
void ecp_nistz256_to_mont(BN_ULONG res[P256_LIMBS],
                          const BN_ULONG in[P256_LIMBS]);
/* Functions that perform constant time access to the precomputed tables */
void ecp_nistz256_select_w5(P256_POINT *val, const P256_POINT *in_t, int index);
void ecp_nistz256_select_w7(P256_POINT_AFFINE *val,
                            const P256_POINT_AFFINE *in_t, int index);

/* One converted into the Montgomery domain */
static const BN_ULONG ONE[P256_LIMBS] = {
    TOBN(0x00000000, 0x00000001), TOBN(0xffffffff, 0x00000000),
    TOBN(0xffffffff, 0xffffffff), TOBN(0x00000000, 0xfffffffe),
};

/* Precomputed tables for the default generator */
#include "p256-x86_64-table.h"

/* Recode window to a signed digit, see ecp_nistputil.c for details */
static unsigned booth_recode_w5(unsigned in) {
  unsigned s, d;

  s = ~((in >> 5) - 1);
  d = (1 << 6) - in - 1;
  d = (d & s) | (in & ~s);
  d = (d >> 1) + (d & 1);

  return (d << 1) + (s & 1);
}

static unsigned booth_recode_w7(unsigned in) {
  unsigned s, d;

  s = ~((in >> 7) - 1);
  d = (1 << 8) - in - 1;
  d = (d & s) | (in & ~s);
  d = (d >> 1) + (d & 1);

  return (d << 1) + (s & 1);
}

static void copy_conditional(BN_ULONG dst[P256_LIMBS],
                             const BN_ULONG src[P256_LIMBS], BN_ULONG move) {
  BN_ULONG mask1 = -move;
  BN_ULONG mask2 = ~mask1;

  dst[0] = (src[0] & mask1) ^ (dst[0] & mask2);
  dst[1] = (src[1] & mask1) ^ (dst[1] & mask2);
  dst[2] = (src[2] & mask1) ^ (dst[2] & mask2);
  dst[3] = (src[3] & mask1) ^ (dst[3] & mask2);
  if (P256_LIMBS == 8) {
    dst[4] = (src[4] & mask1) ^ (dst[4] & mask2);
    dst[5] = (src[5] & mask1) ^ (dst[5] & mask2);
    dst[6] = (src[6] & mask1) ^ (dst[6] & mask2);
    dst[7] = (src[7] & mask1) ^ (dst[7] & mask2);
  }
}

static BN_ULONG is_zero(BN_ULONG in) {
  in |= (0 - in);
  in = ~in;
  in &= BN_MASK2;
  in >>= BN_BITS2 - 1;
  return in;
}

static BN_ULONG is_equal(const BN_ULONG a[P256_LIMBS],
                         const BN_ULONG b[P256_LIMBS]) {
  BN_ULONG res;

  res = a[0] ^ b[0];
  res |= a[1] ^ b[1];
  res |= a[2] ^ b[2];
  res |= a[3] ^ b[3];
  if (P256_LIMBS == 8) {
    res |= a[4] ^ b[4];
    res |= a[5] ^ b[5];
    res |= a[6] ^ b[6];
    res |= a[7] ^ b[7];
  }

  return is_zero(res);
}

static BN_ULONG is_one(const BN_ULONG a[P256_LIMBS]) {
  BN_ULONG res;

  res = a[0] ^ ONE[0];
  res |= a[1] ^ ONE[1];
  res |= a[2] ^ ONE[2];
  res |= a[3] ^ ONE[3];
  if (P256_LIMBS == 8) {
    res |= a[4] ^ ONE[4];
    res |= a[5] ^ ONE[5];
    res |= a[6] ^ ONE[6];
  }

  return is_zero(res);
}

void ecp_nistz256_point_double(P256_POINT *r, const P256_POINT *a);
void ecp_nistz256_point_add(P256_POINT *r, const P256_POINT *a,
                            const P256_POINT *b);
void ecp_nistz256_point_add_affine(P256_POINT *r, const P256_POINT *a,
                                   const P256_POINT_AFFINE *b);

/* r = in^-1 mod p */
static void ecp_nistz256_mod_inverse(BN_ULONG r[P256_LIMBS],
                                     const BN_ULONG in[P256_LIMBS]) {
  /* The poly is ffffffff 00000001 00000000 00000000 00000000 ffffffff ffffffff
     ffffffff
     We use FLT and used poly-2 as exponent */
  BN_ULONG p2[P256_LIMBS];
  BN_ULONG p4[P256_LIMBS];
  BN_ULONG p8[P256_LIMBS];
  BN_ULONG p16[P256_LIMBS];
  BN_ULONG p32[P256_LIMBS];
  BN_ULONG res[P256_LIMBS];
  int i;

  ecp_nistz256_sqr_mont(res, in);
  ecp_nistz256_mul_mont(p2, res, in); /* 3*p */

  ecp_nistz256_sqr_mont(res, p2);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_mul_mont(p4, res, p2); /* f*p */

  ecp_nistz256_sqr_mont(res, p4);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_mul_mont(p8, res, p4); /* ff*p */

  ecp_nistz256_sqr_mont(res, p8);
  for (i = 0; i < 7; i++) {
    ecp_nistz256_sqr_mont(res, res);
  }
  ecp_nistz256_mul_mont(p16, res, p8); /* ffff*p */

  ecp_nistz256_sqr_mont(res, p16);
  for (i = 0; i < 15; i++) {
    ecp_nistz256_sqr_mont(res, res);
  }
  ecp_nistz256_mul_mont(p32, res, p16); /* ffffffff*p */

  ecp_nistz256_sqr_mont(res, p32);
  for (i = 0; i < 31; i++) {
    ecp_nistz256_sqr_mont(res, res);
  }
  ecp_nistz256_mul_mont(res, res, in);

  for (i = 0; i < 32 * 4; i++) {
    ecp_nistz256_sqr_mont(res, res);
  }
  ecp_nistz256_mul_mont(res, res, p32);

  for (i = 0; i < 32; i++) {
    ecp_nistz256_sqr_mont(res, res);
  }
  ecp_nistz256_mul_mont(res, res, p32);

  for (i = 0; i < 16; i++) {
    ecp_nistz256_sqr_mont(res, res);
  }
  ecp_nistz256_mul_mont(res, res, p16);

  for (i = 0; i < 8; i++) {
    ecp_nistz256_sqr_mont(res, res);
  }
  ecp_nistz256_mul_mont(res, res, p8);

  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_mul_mont(res, res, p4);

  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_mul_mont(res, res, p2);

  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_sqr_mont(res, res);
  ecp_nistz256_mul_mont(res, res, in);

  memcpy(r, res, sizeof(res));
}

/* ecp_nistz256_bignum_to_field_elem copies the contents of |in| to |out| and
 * returns one if it fits. Otherwise it returns zero. */
static int ecp_nistz256_bignum_to_field_elem(BN_ULONG out[P256_LIMBS],
                                             const BIGNUM *in) {
  if (in->top > P256_LIMBS) {
    return 0;
  }

  memset(out, 0, sizeof(BN_ULONG) * P256_LIMBS);
  memcpy(out, in->d, sizeof(BN_ULONG) * in->top);
  return 1;
}

/* r = sum(scalar[i]*point[i]) */
static void ecp_nistz256_windowed_mul(const EC_GROUP *group, P256_POINT *r,
                                      const BIGNUM **scalar,
                                      const EC_POINT **point, int num,
                                      BN_CTX *ctx) {
  static const unsigned kWindowSize = 5;
  static const unsigned kMask = (1 << (5 /* kWindowSize */ + 1)) - 1;

  void *table_storage = OPENSSL_malloc(num * 16 * sizeof(P256_POINT) + 64);
  uint8_t(*p_str)[33] = OPENSSL_malloc(num * 33 * sizeof(uint8_t));
  const BIGNUM **scalars = OPENSSL_malloc(num * sizeof(BIGNUM *));

  if (table_storage == NULL ||
      p_str == NULL ||
      scalars == NULL) {
    OPENSSL_PUT_ERROR(EC, ERR_R_MALLOC_FAILURE);
    goto err;
  }

  P256_POINT(*table)[16] = (void *)ALIGNPTR(table_storage, 64);

  int i;
  for (i = 0; i < num; i++) {
    P256_POINT *row = table[i];

    if (BN_num_bits(scalar[i]) > 256 || BN_is_negative(scalar[i])) {
      BIGNUM *mod = BN_CTX_get(ctx);
      if (mod == NULL) {
        goto err;
      }

      if (!BN_nnmod(mod, scalar[i], &group->order, ctx)) {
        OPENSSL_PUT_ERROR(EC, ERR_R_BN_LIB);
        goto err;
      }
      scalars[i] = mod;
    } else {
      scalars[i] = scalar[i];
    }

    int j;
    for (j = 0; j < scalars[i]->top * BN_BYTES; j += BN_BYTES) {
      BN_ULONG d = scalars[i]->d[j / BN_BYTES];

      p_str[i][j + 0] = d & 0xff;
      p_str[i][j + 1] = (d >> 8) & 0xff;
      p_str[i][j + 2] = (d >> 16) & 0xff;
      p_str[i][j + 3] = (d >>= 24) & 0xff;
      if (BN_BYTES == 8) {
        d >>= 8;
        p_str[i][j + 4] = d & 0xff;
        p_str[i][j + 5] = (d >> 8) & 0xff;
        p_str[i][j + 6] = (d >> 16) & 0xff;
        p_str[i][j + 7] = (d >> 24) & 0xff;
      }
    }

    for (; j < 33; j++) {
      p_str[i][j] = 0;
    }

    /* table[0] is implicitly (0,0,0) (the point at infinity), therefore it is
     * not stored. All other values are actually stored with an offset of -1 in
     * table. */

    if (!ecp_nistz256_bignum_to_field_elem(row[1 - 1].X, &point[i]->X) ||
        !ecp_nistz256_bignum_to_field_elem(row[1 - 1].Y, &point[i]->Y) ||
        !ecp_nistz256_bignum_to_field_elem(row[1 - 1].Z, &point[i]->Z)) {
      OPENSSL_PUT_ERROR(EC, EC_R_COORDINATES_OUT_OF_RANGE);
      goto err;
    }

    ecp_nistz256_point_double(&row[2 - 1], &row[1 - 1]);
    ecp_nistz256_point_add(&row[3 - 1], &row[2 - 1], &row[1 - 1]);
    ecp_nistz256_point_double(&row[4 - 1], &row[2 - 1]);
    ecp_nistz256_point_double(&row[6 - 1], &row[3 - 1]);
    ecp_nistz256_point_double(&row[8 - 1], &row[4 - 1]);
    ecp_nistz256_point_double(&row[12 - 1], &row[6 - 1]);
    ecp_nistz256_point_add(&row[5 - 1], &row[4 - 1], &row[1 - 1]);
    ecp_nistz256_point_add(&row[7 - 1], &row[6 - 1], &row[1 - 1]);
    ecp_nistz256_point_add(&row[9 - 1], &row[8 - 1], &row[1 - 1]);
    ecp_nistz256_point_add(&row[13 - 1], &row[12 - 1], &row[1 - 1]);
    ecp_nistz256_point_double(&row[14 - 1], &row[7 - 1]);
    ecp_nistz256_point_double(&row[10 - 1], &row[5 - 1]);
    ecp_nistz256_point_add(&row[15 - 1], &row[14 - 1], &row[1 - 1]);
    ecp_nistz256_point_add(&row[11 - 1], &row[10 - 1], &row[1 - 1]);
    ecp_nistz256_point_add(&row[16 - 1], &row[15 - 1], &row[1 - 1]);
  }

  BN_ULONG tmp[P256_LIMBS];
  ALIGN32 P256_POINT h;
  unsigned index = 255;
  unsigned wvalue = p_str[0][(index - 1) / 8];
  wvalue = (wvalue >> ((index - 1) % 8)) & kMask;

  ecp_nistz256_select_w5(r, table[0], booth_recode_w5(wvalue) >> 1);

  while (index >= 5) {
    for (i = (index == 255 ? 1 : 0); i < num; i++) {
      unsigned off = (index - 1) / 8;

      wvalue = p_str[i][off] | p_str[i][off + 1] << 8;
      wvalue = (wvalue >> ((index - 1) % 8)) & kMask;

      wvalue = booth_recode_w5(wvalue);

      ecp_nistz256_select_w5(&h, table[i], wvalue >> 1);

      ecp_nistz256_neg(tmp, h.Y);
      copy_conditional(h.Y, tmp, (wvalue & 1));

      ecp_nistz256_point_add(r, r, &h);
    }

    index -= kWindowSize;

    ecp_nistz256_point_double(r, r);
    ecp_nistz256_point_double(r, r);
    ecp_nistz256_point_double(r, r);
    ecp_nistz256_point_double(r, r);
    ecp_nistz256_point_double(r, r);
  }

  /* Final window */
  for (i = 0; i < num; i++) {
    wvalue = p_str[i][0];
    wvalue = (wvalue << 1) & kMask;

    wvalue = booth_recode_w5(wvalue);

    ecp_nistz256_select_w5(&h, table[i], wvalue >> 1);

    ecp_nistz256_neg(tmp, h.Y);
    copy_conditional(h.Y, tmp, wvalue & 1);

    ecp_nistz256_point_add(r, r, &h);
  }

err:
  OPENSSL_free(table_storage);
  OPENSSL_free(p_str);
  OPENSSL_free(scalars);
}

/* Coordinates of G, for which we have precomputed tables */
const static BN_ULONG def_xG[P256_LIMBS] = {
    TOBN(0x79e730d4, 0x18a9143c), TOBN(0x75ba95fc, 0x5fedb601),
    TOBN(0x79fb732b, 0x77622510), TOBN(0x18905f76, 0xa53755c6),
};

const static BN_ULONG def_yG[P256_LIMBS] = {
    TOBN(0xddf25357, 0xce95560a), TOBN(0x8b4ab8e4, 0xba19e45c),
    TOBN(0xd2e88688, 0xdd21f325), TOBN(0x8571ff18, 0x25885d85)
};

/* ecp_nistz256_is_affine_G returns one if |generator| is the standard, P-256
 * generator. */
static int ecp_nistz256_is_affine_G(const EC_POINT *generator) {
  return (generator->X.top == P256_LIMBS) && (generator->Y.top == P256_LIMBS) &&
         (generator->Z.top == (P256_LIMBS - P256_LIMBS / 8)) &&
         is_equal(generator->X.d, def_xG) && is_equal(generator->Y.d, def_yG) &&
         is_one(generator->Z.d);
}

/* r = scalar*G + sum(scalars[i]*points[i]) */
static int ecp_nistz256_points_mul(
    const EC_GROUP *group, EC_POINT *r, const BIGNUM *scalar, size_t num,
    const EC_POINT *points[], const BIGNUM *scalars[], BN_CTX *ctx) {
  static const unsigned kWindowSize = 7;
  static const unsigned kMask = (1 << (7 /* kWindowSize */ + 1)) - 1;

  int ret = 0, no_precomp_for_generator = 0, p_is_infinity = 0;
  ALIGN32 union {
    P256_POINT p;
    P256_POINT_AFFINE a;
  } t, p;

  if (scalar == NULL && num == 0) {
    return EC_POINT_set_to_infinity(group, r);
  }

  /* Need 256 bits for space for all coordinates. */
  bn_wexpand(&r->X, P256_LIMBS);
  bn_wexpand(&r->Y, P256_LIMBS);
  bn_wexpand(&r->Z, P256_LIMBS);
  r->X.top = P256_LIMBS;
  r->Y.top = P256_LIMBS;
  r->Z.top = P256_LIMBS;

  const EC_POINT *generator = NULL;
  if (scalar) {
    generator = EC_GROUP_get0_generator(group);
    if (generator == NULL) {
      OPENSSL_PUT_ERROR(EC, EC_R_UNDEFINED_GENERATOR);
      goto err;
    }

    if (ecp_nistz256_is_affine_G(generator)) {
      if (BN_num_bits(scalar) > 256 || BN_is_negative(scalar)) {
        BIGNUM *tmp_scalar = BN_CTX_get(ctx);
        if (tmp_scalar == NULL) {
          goto err;
        }

        if (!BN_nnmod(tmp_scalar, scalar, &group->order, ctx)) {
          OPENSSL_PUT_ERROR(EC, ERR_R_BN_LIB);
          goto err;
        }
        scalar = tmp_scalar;
      }

      uint8_t p_str[33] = {0};
      int i;
      for (i = 0; i < scalar->top * BN_BYTES; i += BN_BYTES) {
        BN_ULONG d = scalar->d[i / BN_BYTES];

        p_str[i + 0] = d & 0xff;
        p_str[i + 1] = (d >> 8) & 0xff;
        p_str[i + 2] = (d >> 16) & 0xff;
        p_str[i + 3] = (d >>= 24) & 0xff;
        if (BN_BYTES == 8) {
          d >>= 8;
          p_str[i + 4] = d & 0xff;
          p_str[i + 5] = (d >> 8) & 0xff;
          p_str[i + 6] = (d >> 16) & 0xff;
          p_str[i + 7] = (d >> 24) & 0xff;
        }
      }

      for (; i < (int) sizeof(p_str); i++) {
        p_str[i] = 0;
      }

      /* First window */
      unsigned wvalue = (p_str[0] << 1) & kMask;
      unsigned index = kWindowSize;

      wvalue = booth_recode_w7(wvalue);

      const PRECOMP256_ROW *const precomputed_table =
          (const PRECOMP256_ROW *)ecp_nistz256_precomputed;
      ecp_nistz256_select_w7(&p.a, precomputed_table[0], wvalue >> 1);

      ecp_nistz256_neg(p.p.Z, p.p.Y);
      copy_conditional(p.p.Y, p.p.Z, wvalue & 1);

      memcpy(p.p.Z, ONE, sizeof(ONE));

      for (i = 1; i < 37; i++) {
        unsigned off = (index - 1) / 8;
        wvalue = p_str[off] | p_str[off + 1] << 8;
        wvalue = (wvalue >> ((index - 1) % 8)) & kMask;
        index += kWindowSize;

        wvalue = booth_recode_w7(wvalue);

        ecp_nistz256_select_w7(&t.a, precomputed_table[i], wvalue >> 1);

        ecp_nistz256_neg(t.p.Z, t.a.Y);
        copy_conditional(t.a.Y, t.p.Z, wvalue & 1);

        ecp_nistz256_point_add_affine(&p.p, &p.p, &t.a);
      }
    } else {
      p_is_infinity = 1;
      no_precomp_for_generator = 1;
    }
  } else {
    p_is_infinity = 1;
  }

  if (no_precomp_for_generator) {
    /* Without a precomputed table for the generator, it has to be handled like
     * a normal point. */
    const BIGNUM **new_scalars;
    const EC_POINT **new_points;

    /* Bound |num| so that all the possible overflows in the following can be
     * excluded. */
    if (0xffffff < num) {
      OPENSSL_PUT_ERROR(EC, ERR_R_MALLOC_FAILURE);
      return 0;
    }

    new_scalars = OPENSSL_malloc((num + 1) * sizeof(BIGNUM *));
    if (new_scalars == NULL) {
      OPENSSL_PUT_ERROR(EC, ERR_R_MALLOC_FAILURE);
      return 0;
    }

    new_points = OPENSSL_malloc((num + 1) * sizeof(EC_POINT *));
    if (new_points == NULL) {
      OPENSSL_free(new_scalars);
      OPENSSL_PUT_ERROR(EC, ERR_R_MALLOC_FAILURE);
      return 0;
    }

    memcpy(new_scalars, scalars, num * sizeof(BIGNUM *));
    new_scalars[num] = scalar;
    memcpy(new_points, points, num * sizeof(EC_POINT *));
    new_points[num] = generator;

    scalars = new_scalars;
    points = new_points;
    num++;
  }

  if (num) {
    P256_POINT *out = &t.p;
    if (p_is_infinity) {
      out = &p.p;
    }

    ecp_nistz256_windowed_mul(group, out, scalars, points, num, ctx);

    if (!p_is_infinity) {
      ecp_nistz256_point_add(&p.p, &p.p, out);
    }
  }

  if (no_precomp_for_generator) {
    OPENSSL_free(points);
    OPENSSL_free(scalars);
  }

  memcpy(r->X.d, p.p.X, sizeof(p.p.X));
  memcpy(r->Y.d, p.p.Y, sizeof(p.p.Y));
  memcpy(r->Z.d, p.p.Z, sizeof(p.p.Z));
  bn_correct_top(&r->X);
  bn_correct_top(&r->Y);
  bn_correct_top(&r->Z);

  ret = 1;

err:
  return ret;
}

static int ecp_nistz256_get_affine(const EC_GROUP *group, const EC_POINT *point,
                                   BIGNUM *x, BIGNUM *y, BN_CTX *ctx) {
  BN_ULONG z_inv2[P256_LIMBS];
  BN_ULONG z_inv3[P256_LIMBS];
  BN_ULONG x_aff[P256_LIMBS];
  BN_ULONG y_aff[P256_LIMBS];
  BN_ULONG point_x[P256_LIMBS], point_y[P256_LIMBS], point_z[P256_LIMBS];

  if (EC_POINT_is_at_infinity(group, point)) {
    OPENSSL_PUT_ERROR(EC, EC_R_POINT_AT_INFINITY);
    return 0;
  }

  if (!ecp_nistz256_bignum_to_field_elem(point_x, &point->X) ||
      !ecp_nistz256_bignum_to_field_elem(point_y, &point->Y) ||
      !ecp_nistz256_bignum_to_field_elem(point_z, &point->Z)) {
    OPENSSL_PUT_ERROR(EC, EC_R_COORDINATES_OUT_OF_RANGE);
    return 0;
  }

  ecp_nistz256_mod_inverse(z_inv3, point_z);
  ecp_nistz256_sqr_mont(z_inv2, z_inv3);
  ecp_nistz256_mul_mont(x_aff, z_inv2, point_x);

  if (x != NULL) {
    bn_wexpand(x, P256_LIMBS);
    x->top = P256_LIMBS;
    ecp_nistz256_from_mont(x->d, x_aff);
    bn_correct_top(x);
  }

  if (y != NULL) {
    ecp_nistz256_mul_mont(z_inv3, z_inv3, z_inv2);
    ecp_nistz256_mul_mont(y_aff, z_inv3, point_y);
    bn_wexpand(y, P256_LIMBS);
    y->top = P256_LIMBS;
    ecp_nistz256_from_mont(y->d, y_aff);
    bn_correct_top(y);
  }

  return 1;
}

const EC_METHOD *EC_GFp_nistz256_method(void) {
  static const EC_METHOD ret = {
      ec_GFp_mont_group_init,
      ec_GFp_mont_group_finish,
      ec_GFp_mont_group_clear_finish,
      ec_GFp_mont_group_copy,
      ec_GFp_mont_group_set_curve,
      ecp_nistz256_get_affine,
      ecp_nistz256_points_mul,
      0, /* precompute_mult */
      ec_GFp_mont_field_mul,
      ec_GFp_mont_field_sqr,
      ec_GFp_mont_field_encode,
      ec_GFp_mont_field_decode,
      ec_GFp_mont_field_set_to_one,
  };

  return &ret;
}

#endif /* !defined(OPENSSL_NO_ASM) && defined(OPENSSL_X86_64) && \
          !defined(OPENSSL_SMALL) */
