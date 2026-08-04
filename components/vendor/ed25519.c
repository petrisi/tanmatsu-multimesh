// SPDX-License-Identifier: MIT
//
// See ed25519.h for why this is written against mbedtls_mpi rather than a
// packed-limb field implementation.

#include "ed25519.h"
#include <string.h>
#include "mbedtls/bignum.h"
#include "psa/crypto.h"

// Curve parameters (RFC 8032, section 5.1), big-endian for mbedtls_mpi.
#define STR_P      "7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFED"
#define STR_D      "52036CEE2B6FFE738CC740797779E89800700A4D4141D8AB75EB4DCA135978A3"
#define STR_SQRTM1 "2B8324804FC1DF0B2B4D00993DFBD7A72F431806AD2FE478C4EE1B274A0EA0B0"
#define STR_BX     "216936D3CD6E53FEC0A4E231FDD6DC5C692CC7609525A7B2C9562D608F25D51A"
#define STR_BY     "6666666666666666666666666666666666666666666666666666666666666658"
#define STR_L      "1000000000000000000000000000000014DEF9DEA2F79CD65812631A5CF5D3ED"

typedef struct {
    mbedtls_mpi p;       // 2^255 - 19
    mbedtls_mpi d;       // curve constant
    mbedtls_mpi sqrtm1;  // sqrt(-1) mod p, for point decompression
    mbedtls_mpi order;   // group order L
    bool        ready;
} curve_t;

static curve_t curve;

// A point in extended homogeneous coordinates: x = X/Z, y = Y/Z, X*Y = Z*T.
typedef struct {
    mbedtls_mpi X, Y, Z, T;
} point_t;

// --- small helpers -------------------------------------------------------

#define CHECK(expr)                 \
    do {                            \
        if ((expr) != 0) goto done; \
    } while (0)

static void point_init(point_t* q) {
    mbedtls_mpi_init(&q->X);
    mbedtls_mpi_init(&q->Y);
    mbedtls_mpi_init(&q->Z);
    mbedtls_mpi_init(&q->T);
}

static void point_free(point_t* q) {
    mbedtls_mpi_free(&q->X);
    mbedtls_mpi_free(&q->Y);
    mbedtls_mpi_free(&q->Z);
    mbedtls_mpi_free(&q->T);
}

static int curve_init(void) {
    if (curve.ready) return 0;

    mbedtls_mpi_init(&curve.p);
    mbedtls_mpi_init(&curve.d);
    mbedtls_mpi_init(&curve.sqrtm1);
    mbedtls_mpi_init(&curve.order);

    int ret;
    CHECK(ret = mbedtls_mpi_read_string(&curve.p, 16, STR_P));
    CHECK(ret = mbedtls_mpi_read_string(&curve.d, 16, STR_D));
    CHECK(ret = mbedtls_mpi_read_string(&curve.sqrtm1, 16, STR_SQRTM1));
    CHECK(ret = mbedtls_mpi_read_string(&curve.order, 16, STR_L));
    curve.ready = true;
    return 0;
done:
    return ret;
}

// r = a mod p, always non-negative.
static int fe_mod(mbedtls_mpi* r) {
    int ret = mbedtls_mpi_mod_mpi(r, r, &curve.p);
    if (ret == 0 && mbedtls_mpi_cmp_int(r, 0) < 0) ret = mbedtls_mpi_add_mpi(r, r, &curve.p);
    return ret;
}

static int fe_mul(mbedtls_mpi* r, const mbedtls_mpi* a, const mbedtls_mpi* b) {
    int ret = mbedtls_mpi_mul_mpi(r, a, b);
    return ret ? ret : fe_mod(r);
}

static int fe_add(mbedtls_mpi* r, const mbedtls_mpi* a, const mbedtls_mpi* b) {
    int ret = mbedtls_mpi_add_mpi(r, a, b);
    return ret ? ret : fe_mod(r);
}

static int fe_sub(mbedtls_mpi* r, const mbedtls_mpi* a, const mbedtls_mpi* b) {
    int ret = mbedtls_mpi_sub_mpi(r, a, b);
    return ret ? ret : fe_mod(r);
}

// a^e mod p.
static int fe_pow(mbedtls_mpi* r, const mbedtls_mpi* a, const mbedtls_mpi* e) {
    return mbedtls_mpi_exp_mod(r, a, e, &curve.p, NULL);
}

// Ed25519 encodes scalars and coordinates little-endian; mbedtls_mpi is
// big-endian, so every crossing needs a reversal.
static void reverse(uint8_t* out, const uint8_t* in, size_t len) {
    for (size_t i = 0; i < len; i++) out[i] = in[len - 1 - i];
}

static int mpi_from_le(mbedtls_mpi* r, const uint8_t* in, size_t len) {
    uint8_t be[64];
    if (len > sizeof(be)) return -1;
    reverse(be, in, len);
    return mbedtls_mpi_read_binary(r, be, len);
}

static int mpi_to_le(uint8_t* out, size_t len, const mbedtls_mpi* a) {
    uint8_t be[64];
    if (len > sizeof(be)) return -1;
    int ret = mbedtls_mpi_write_binary(a, be, len);
    if (ret) return ret;
    reverse(out, be, len);
    return 0;
}

// --- group arithmetic ----------------------------------------------------

static int point_set_identity(point_t* q) {
    int ret;
    CHECK(ret = mbedtls_mpi_lset(&q->X, 0));
    CHECK(ret = mbedtls_mpi_lset(&q->Y, 1));
    CHECK(ret = mbedtls_mpi_lset(&q->Z, 1));
    CHECK(ret = mbedtls_mpi_lset(&q->T, 0));
done:
    return ret;
}

static int point_copy(point_t* r, const point_t* a) {
    int ret;
    CHECK(ret = mbedtls_mpi_copy(&r->X, &a->X));
    CHECK(ret = mbedtls_mpi_copy(&r->Y, &a->Y));
    CHECK(ret = mbedtls_mpi_copy(&r->Z, &a->Z));
    CHECK(ret = mbedtls_mpi_copy(&r->T, &a->T));
done:
    return ret;
}

// Unified addition for twisted Edwards with a = -1 (RFC 8032 section 5.1.4).
static int point_add(point_t* r, const point_t* p1, const point_t* p2) {
    mbedtls_mpi A, B, C, D, E, F, G, H, t1, t2;
    mbedtls_mpi_init(&A);
    mbedtls_mpi_init(&B);
    mbedtls_mpi_init(&C);
    mbedtls_mpi_init(&D);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&F);
    mbedtls_mpi_init(&G);
    mbedtls_mpi_init(&H);
    mbedtls_mpi_init(&t1);
    mbedtls_mpi_init(&t2);
    int ret;

    CHECK(ret = fe_sub(&t1, &p1->Y, &p1->X));
    CHECK(ret = fe_sub(&t2, &p2->Y, &p2->X));
    CHECK(ret = fe_mul(&A, &t1, &t2));

    CHECK(ret = fe_add(&t1, &p1->Y, &p1->X));
    CHECK(ret = fe_add(&t2, &p2->Y, &p2->X));
    CHECK(ret = fe_mul(&B, &t1, &t2));

    CHECK(ret = fe_mul(&C, &p1->T, &p2->T));
    CHECK(ret = fe_mul(&C, &C, &curve.d));
    CHECK(ret = fe_add(&C, &C, &C));  // C = 2*d*T1*T2

    CHECK(ret = fe_mul(&D, &p1->Z, &p2->Z));
    CHECK(ret = fe_add(&D, &D, &D));  // D = 2*Z1*Z2

    CHECK(ret = fe_sub(&E, &B, &A));
    CHECK(ret = fe_sub(&F, &D, &C));
    CHECK(ret = fe_add(&G, &D, &C));
    CHECK(ret = fe_add(&H, &B, &A));

    CHECK(ret = fe_mul(&r->X, &E, &F));
    CHECK(ret = fe_mul(&r->Y, &G, &H));
    CHECK(ret = fe_mul(&r->T, &E, &H));
    CHECK(ret = fe_mul(&r->Z, &F, &G));

done:
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&C);
    mbedtls_mpi_free(&D);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&F);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&H);
    mbedtls_mpi_free(&t1);
    mbedtls_mpi_free(&t2);
    return ret;
}

static int point_double(point_t* r, const point_t* p1) {
    mbedtls_mpi A, B, C, E, F, G, H, t;
    mbedtls_mpi_init(&A);
    mbedtls_mpi_init(&B);
    mbedtls_mpi_init(&C);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&F);
    mbedtls_mpi_init(&G);
    mbedtls_mpi_init(&H);
    mbedtls_mpi_init(&t);
    int ret;

    CHECK(ret = fe_mul(&A, &p1->X, &p1->X));
    CHECK(ret = fe_mul(&B, &p1->Y, &p1->Y));
    CHECK(ret = fe_mul(&C, &p1->Z, &p1->Z));
    CHECK(ret = fe_add(&C, &C, &C));  // C = 2*Z^2

    CHECK(ret = fe_add(&H, &A, &B));
    CHECK(ret = fe_add(&t, &p1->X, &p1->Y));
    CHECK(ret = fe_mul(&t, &t, &t));
    CHECK(ret = fe_sub(&E, &H, &t));  // E = H - (X+Y)^2
    CHECK(ret = fe_sub(&G, &A, &B));
    CHECK(ret = fe_add(&F, &C, &G));

    CHECK(ret = fe_mul(&r->X, &E, &F));
    CHECK(ret = fe_mul(&r->Y, &G, &H));
    CHECK(ret = fe_mul(&r->T, &E, &H));
    CHECK(ret = fe_mul(&r->Z, &F, &G));

done:
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&C);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&F);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&H);
    mbedtls_mpi_free(&t);
    return ret;
}

// Plain double-and-add. Not constant time; see the header.
static int point_scalar_mul(point_t* r, const mbedtls_mpi* scalar, const point_t* base) {
    point_t acc;
    point_init(&acc);
    int ret;

    CHECK(ret = point_set_identity(&acc));

    size_t bits = mbedtls_mpi_bitlen(scalar);
    for (size_t i = bits; i > 0; i--) {
        CHECK(ret = point_double(&acc, &acc));
        if (mbedtls_mpi_get_bit(scalar, i - 1)) {
            CHECK(ret = point_add(&acc, &acc, base));
        }
    }
    ret = point_copy(r, &acc);

done:
    point_free(&acc);
    return ret;
}

static int point_base(point_t* q) {
    int ret;
    CHECK(ret = mbedtls_mpi_read_string(&q->X, 16, STR_BX));
    CHECK(ret = mbedtls_mpi_read_string(&q->Y, 16, STR_BY));
    CHECK(ret = mbedtls_mpi_lset(&q->Z, 1));
    CHECK(ret = fe_mul(&q->T, &q->X, &q->Y));
done:
    return ret;
}

// Compress to 32 bytes: y little-endian, with the low bit of x in the top bit.
static int point_encode(uint8_t out[32], const point_t* q) {
    mbedtls_mpi zinv, x, y;
    mbedtls_mpi_init(&zinv);
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&y);
    int ret;

    CHECK(ret = mbedtls_mpi_inv_mod(&zinv, &q->Z, &curve.p));
    CHECK(ret = fe_mul(&x, &q->X, &zinv));
    CHECK(ret = fe_mul(&y, &q->Y, &zinv));

    CHECK(ret = mpi_to_le(out, 32, &y));
    if (mbedtls_mpi_get_bit(&x, 0)) out[31] |= 0x80;

done:
    mbedtls_mpi_free(&zinv);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&y);
    return ret;
}

// Recover x from y: x^2 = (y^2 - 1) / (d*y^2 + 1), then the RFC 8032 square
// root. Returns non-zero for a point not on the curve.
static int point_decode(point_t* q, const uint8_t in[32]) {
    mbedtls_mpi y, u, v, v3, v7, t, x, check, exponent;
    mbedtls_mpi_init(&y);
    mbedtls_mpi_init(&u);
    mbedtls_mpi_init(&v);
    mbedtls_mpi_init(&v3);
    mbedtls_mpi_init(&v7);
    mbedtls_mpi_init(&t);
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&check);
    mbedtls_mpi_init(&exponent);
    int ret;

    uint8_t bytes[32];
    memcpy(bytes, in, 32);
    int sign = (bytes[31] & 0x80) ? 1 : 0;
    bytes[31] &= 0x7F;

    CHECK(ret = mpi_from_le(&y, bytes, 32));
    // A y at or above p is not a canonical encoding.
    if (mbedtls_mpi_cmp_mpi(&y, &curve.p) >= 0) {
        ret = -1;
        goto done;
    }

    CHECK(ret = fe_mul(&u, &y, &y));
    CHECK(ret = mbedtls_mpi_lset(&t, 1));
    CHECK(ret = fe_mul(&v, &u, &curve.d));
    CHECK(ret = fe_add(&v, &v, &t));  // v = d*y^2 + 1
    CHECK(ret = fe_sub(&u, &u, &t));  // u = y^2 - 1

    // x = u * v^3 * (u * v^7)^((p-5)/8)
    CHECK(ret = fe_mul(&v3, &v, &v));
    CHECK(ret = fe_mul(&v3, &v3, &v));
    CHECK(ret = fe_mul(&v7, &v3, &v3));
    CHECK(ret = fe_mul(&v7, &v7, &v));
    CHECK(ret = fe_mul(&t, &u, &v7));

    CHECK(ret = mbedtls_mpi_copy(&exponent, &curve.p));
    CHECK(ret = mbedtls_mpi_sub_int(&exponent, &exponent, 5));
    CHECK(ret = mbedtls_mpi_shift_r(&exponent, 3));  // (p-5)/8
    CHECK(ret = fe_pow(&x, &t, &exponent));
    CHECK(ret = fe_mul(&x, &x, &v3));
    CHECK(ret = fe_mul(&x, &x, &u));

    // Check v*x^2 == u, else v*x^2 == -u and x needs multiplying by sqrt(-1).
    CHECK(ret = fe_mul(&check, &x, &x));
    CHECK(ret = fe_mul(&check, &check, &v));
    if (mbedtls_mpi_cmp_mpi(&check, &u) != 0) {
        CHECK(ret = fe_add(&t, &check, &u));
        CHECK(ret = fe_mod(&t));
        if (mbedtls_mpi_cmp_int(&t, 0) != 0) {
            ret = -1;  // not a square: not on the curve
            goto done;
        }
        CHECK(ret = fe_mul(&x, &x, &curve.sqrtm1));
    }

    // x = 0 with a set sign bit has no valid root.
    if (mbedtls_mpi_cmp_int(&x, 0) == 0 && sign) {
        ret = -1;
        goto done;
    }
    if (mbedtls_mpi_get_bit(&x, 0) != sign) {
        CHECK(ret = mbedtls_mpi_sub_mpi(&x, &curve.p, &x));
        CHECK(ret = fe_mod(&x));
    }

    CHECK(ret = mbedtls_mpi_copy(&q->X, &x));
    CHECK(ret = mbedtls_mpi_copy(&q->Y, &y));
    CHECK(ret = mbedtls_mpi_lset(&q->Z, 1));
    CHECK(ret = fe_mul(&q->T, &q->X, &q->Y));

done:
    mbedtls_mpi_free(&y);
    mbedtls_mpi_free(&u);
    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&v3);
    mbedtls_mpi_free(&v7);
    mbedtls_mpi_free(&t);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&check);
    mbedtls_mpi_free(&exponent);
    return ret;
}

// --- hashing -------------------------------------------------------------

// SHA-512 over up to three parts. PSA, because mbedtls/sha512.h is private in
// mbedtls 4.x.
static int sha512(uint8_t out[64], const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len,
                  const uint8_t* c, size_t c_len) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t               written;

    if (psa_hash_setup(&op, PSA_ALG_SHA_512) != PSA_SUCCESS) return -1;
    if (a_len && psa_hash_update(&op, a, a_len) != PSA_SUCCESS) goto fail;
    if (b_len && psa_hash_update(&op, b, b_len) != PSA_SUCCESS) goto fail;
    if (c_len && psa_hash_update(&op, c, c_len) != PSA_SUCCESS) goto fail;
    if (psa_hash_finish(&op, out, 64, &written) != PSA_SUCCESS || written != 64) return -1;
    return 0;

fail:
    psa_hash_abort(&op);
    return -1;
}

// Interpret a 64-byte hash as a little-endian integer and reduce mod L.
static int scalar_from_hash(mbedtls_mpi* r, const uint8_t hash[64]) {
    int ret = mpi_from_le(r, hash, 64);
    return ret ? ret : mbedtls_mpi_mod_mpi(r, r, &curve.order);
}

// --- public API ----------------------------------------------------------

bool ed25519_keypair(uint8_t public_key[ED25519_PUBLIC_LEN], uint8_t private_key[ED25519_PRIVATE_LEN],
                     const uint8_t seed[ED25519_SEED_LEN]) {
    if (curve_init() != 0) return false;

    mbedtls_mpi a;
    mbedtls_mpi_init(&a);
    point_t A;
    point_init(&A);
    point_t base;
    point_init(&base);
    bool ok = false;

    if (sha512(private_key, seed, ED25519_SEED_LEN, NULL, 0, NULL, 0) != 0) goto done;

    // Clamping: clear the low three bits, clear the top bit, set bit 254.
    private_key[0]  &= 248;
    private_key[31] &= 127;
    private_key[31] |= 64;

    if (mpi_from_le(&a, private_key, 32) != 0) goto done;
    if (point_base(&base) != 0) goto done;
    if (point_scalar_mul(&A, &a, &base) != 0) goto done;
    if (point_encode(public_key, &A) != 0) goto done;
    ok = true;

done:
    mbedtls_mpi_free(&a);
    point_free(&A);
    point_free(&base);
    return ok;
}

bool ed25519_sign(uint8_t signature[ED25519_SIGNATURE_LEN], const uint8_t* message, size_t message_len,
                  const uint8_t public_key[ED25519_PUBLIC_LEN], const uint8_t private_key[ED25519_PRIVATE_LEN]) {
    if (curve_init() != 0) return false;

    mbedtls_mpi r, k, a, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&k);
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&s);
    point_t R, base;
    point_init(&R);
    point_init(&base);
    uint8_t hash[64];
    bool    ok = false;

    // r = H(prefix || M) mod L, where prefix is the second half of the private
    // key. Deterministic: no RNG in the signing path.
    if (sha512(hash, private_key + 32, 32, message, message_len, NULL, 0) != 0) goto done;
    if (scalar_from_hash(&r, hash) != 0) goto done;

    if (point_base(&base) != 0) goto done;
    if (point_scalar_mul(&R, &r, &base) != 0) goto done;
    if (point_encode(signature, &R) != 0) goto done;

    // k = H(R || A || M) mod L
    if (sha512(hash, signature, 32, public_key, 32, message, message_len) != 0) goto done;
    if (scalar_from_hash(&k, hash) != 0) goto done;

    // S = (r + k*a) mod L
    if (mpi_from_le(&a, private_key, 32) != 0) goto done;
    if (mbedtls_mpi_mul_mpi(&s, &k, &a) != 0) goto done;
    if (mbedtls_mpi_add_mpi(&s, &s, &r) != 0) goto done;
    if (mbedtls_mpi_mod_mpi(&s, &s, &curve.order) != 0) goto done;
    if (mpi_to_le(signature + 32, 32, &s) != 0) goto done;
    ok = true;

done:
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&k);
    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&s);
    point_free(&R);
    point_free(&base);
    return ok;
}

bool ed25519_verify(const uint8_t signature[ED25519_SIGNATURE_LEN], const uint8_t* message, size_t message_len,
                    const uint8_t public_key[ED25519_PUBLIC_LEN]) {
    if (curve_init() != 0) return false;

    mbedtls_mpi s, k;
    mbedtls_mpi_init(&s);
    mbedtls_mpi_init(&k);
    point_t A, R, base, sB, kA, expected;
    point_init(&A);
    point_init(&R);
    point_init(&base);
    point_init(&sB);
    point_init(&kA);
    point_init(&expected);
    uint8_t hash[64];
    uint8_t encoded[32];
    bool    ok = false;

    // A non-canonical S is a forgery vector, so reject it before doing any work.
    if (mpi_from_le(&s, signature + 32, 32) != 0) goto done;
    if (mbedtls_mpi_cmp_mpi(&s, &curve.order) >= 0) goto done;

    if (point_decode(&A, public_key) != 0) goto done;
    if (point_decode(&R, signature) != 0) goto done;

    if (sha512(hash, signature, 32, public_key, 32, message, message_len) != 0) goto done;
    if (scalar_from_hash(&k, hash) != 0) goto done;

    // Check S*B == R + k*A by computing both sides and comparing encodings.
    if (point_base(&base) != 0) goto done;
    if (point_scalar_mul(&sB, &s, &base) != 0) goto done;
    if (point_scalar_mul(&kA, &k, &A) != 0) goto done;
    if (point_add(&expected, &R, &kA) != 0) goto done;

    if (point_encode(encoded, &sB) != 0) goto done;
    uint8_t other[32];
    if (point_encode(other, &expected) != 0) goto done;
    ok = memcmp(encoded, other, 32) == 0;

done:
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&k);
    point_free(&A);
    point_free(&R);
    point_free(&base);
    point_free(&sB);
    point_free(&kA);
    point_free(&expected);
    return ok;
}

// --- self test -----------------------------------------------------------

// RFC 8032 section 7.1, TEST 1 and TEST 2.
static const uint8_t TEST1_SEED[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                       0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                       0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
static const uint8_t TEST1_PUB[32]  = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                                       0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                                       0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
static const uint8_t TEST1_SIG[64]  = {
    0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
    0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
    0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
    0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};

static const uint8_t TEST2_SEED[32] = {0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3,
                                       0x46, 0xec, 0x11, 0x4e, 0x0f, 0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab,
                                       0xa6, 0x24, 0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb};
static const uint8_t TEST2_PUB[32]  = {0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a,
                                       0xa7, 0x4d, 0x1b, 0x7e, 0xbc, 0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4,
                                       0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};
static const uint8_t TEST2_MSG[1]   = {0x72};
static const uint8_t TEST2_SIG[64]  = {
    0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82, 0x0b, 0x5f, 0x64, 0x25, 0x40,
    0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f, 0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda,
    0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
    0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00};

static bool run_vector(const uint8_t seed[32], const uint8_t expect_pub[32], const uint8_t* message,
                       size_t message_len, const uint8_t expect_sig[64]) {
    uint8_t pub[32], priv[64], sig[64];

    if (!ed25519_keypair(pub, priv, seed)) return false;
    if (memcmp(pub, expect_pub, 32) != 0) return false;
    if (!ed25519_sign(sig, message, message_len, pub, priv)) return false;
    if (memcmp(sig, expect_sig, 64) != 0) return false;
    if (!ed25519_verify(sig, message, message_len, pub)) return false;

    // A tampered signature must fail, or verification is not doing anything.
    uint8_t bad[64];
    memcpy(bad, sig, 64);
    bad[0] ^= 0x01;
    if (ed25519_verify(bad, message, message_len, pub)) return false;

    return true;
}

bool ed25519_selftest(void) {
    if (!run_vector(TEST1_SEED, TEST1_PUB, NULL, 0, TEST1_SIG)) return false;
    if (!run_vector(TEST2_SEED, TEST2_PUB, TEST2_MSG, sizeof(TEST2_MSG), TEST2_SIG)) return false;
    return true;
}
