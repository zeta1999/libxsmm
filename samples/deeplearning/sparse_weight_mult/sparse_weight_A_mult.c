/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Alexander Heinecke (Intel Corp.)
******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <libxsmm.h>

int main(int argc, char* argv[]) {
  unsigned int N =     ( argc > 1 ) ? atoi(argv[1]) : 64;
  unsigned int C =     ( argc > 2 ) ? atoi(argv[2]) : 512;
  unsigned int K =     ( argc > 3 ) ? atoi(argv[3]) : 32;
  unsigned int nb =    ( argc > 4 ) ? atoi(argv[4]) : 16;
  double sparse_frac = ( argc > 5 ) ? atof(argv[5]) : 0.90;
  unsigned int REPS  = ( argc > 6 ) ? atoi(argv[6]) : 1;

  const libxsmm_gemm_prefetch_type prefetch = LIBXSMM_GEMM_PREFETCH_NONE;
  const int flags = LIBXSMM_GEMM_FLAGS('N', 'N');
  const float alpha = 1, beta = 1;

  unsigned int* l_rowptr = NULL;
  unsigned int* l_colidx = NULL;
  float* l_a_de = (float*)libxsmm_aligned_malloc(sizeof(float) * C * K, 64);
  float* l_a_sp_csr = NULL;
  float* l_b = (float*)libxsmm_aligned_malloc(sizeof(float) * N * C, 64);
  float* l_c_gold = (float*)libxsmm_aligned_malloc(sizeof(float) * N * K, 64);
  float* l_c_asm_csr = (float*)libxsmm_aligned_malloc(sizeof(float) * N * K, 64);
  float l_max_error = 0.0;
  unsigned int l_k, l_n;
  unsigned int l_i, l_j, l_jj;
  unsigned int NB = N / nb;

  LIBXSMM_VLA_DECL(2, float, l_p_a_de, l_a_de, C);
  LIBXSMM_VLA_DECL(3, float, l_p_b, l_b, NB, nb);
  LIBXSMM_VLA_DECL(3, float, l_p_c_asm_csr, l_c_asm_csr, NB, nb);
  LIBXSMM_VLA_DECL(3, float, l_p_c_gold, l_c_gold, NB, nb);

  libxsmm_descriptor_blob l_xgemm_blob;
  const libxsmm_gemm_descriptor* l_xgemm_desc = 0;
  LIBXSMM_MMFUNCTION_TYPE(float) mykernel_csr = NULL;

  unsigned long long l_start, l_end;
  double l_total;
  unsigned int nnz = 0;

  if (argc != 7 && argc != 1) {
    fprintf( stderr, "arguments failure\n" );
    return -1;
  }

  if ( N % nb != 0 ) {
    fprintf( stderr, "N needs to be disable by nb\n" );
    return -1;
  }

  /* touch B */
  for ( l_i = 0; l_i < C; l_i++) {
    for ( l_j = 0; l_j < NB; l_j++) {
      for ( l_k = 0; l_k < nb; l_k++ ) {
        LIBXSMM_VLA_ACCESS(3, l_p_b, l_i, l_j, l_k, NB, nb) = (float)libxsmm_rng_f64();
      }
    }
  }

  /* touch dense A */
  for ( l_i = 0; l_i < K; l_i++ ) {
    for ( l_j = 0; l_j < C; l_j++ ) {
      float tmp = (float)libxsmm_rng_f64();
      if ( tmp < sparse_frac ) {
        tmp = 0;
      } else {
        nnz++;
      }
      LIBXSMM_VLA_ACCESS(2, l_p_a_de, l_i, l_j, C) = tmp;
    }
  }
  printf("we just generated a %i x %i matrix with %i NZ entries\n", K, C, nnz);

  /* touch C */
  for ( l_i = 0; l_i < K; l_i++) {
    for ( l_j = 0; l_j < NB; l_j++) {
      for ( l_k = 0; l_k < nb; l_k++ ) {
        LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_i, l_j, l_k, NB, nb) = 0.f;
        LIBXSMM_VLA_ACCESS(3, l_p_c_asm_csr,  l_i, l_j, l_k, NB, nb) = 0.f;
      }
    }
  }

  /* create B, csr */
  l_rowptr   = (unsigned int*) libxsmm_aligned_malloc( (K+1)*sizeof(unsigned int), 64 );
  l_colidx   = (unsigned int*) libxsmm_aligned_malloc( nnz*sizeof(unsigned int),   64 );
  l_a_sp_csr = (float*       ) libxsmm_aligned_malloc( nnz*sizeof(float),          64 );
  l_k = 0;
  l_rowptr[K] = nnz;
  for ( l_i = 0; l_i < K; l_i++ ) {
    l_rowptr[l_i] = l_k;
    for ( l_j = 0; l_j < C; l_j++ ) {
      if ( LIBXSMM_VLA_ACCESS(2, l_p_a_de, l_i, l_j, C) != 0.0 ) {
        l_colidx[l_k] = l_j;
        l_a_sp_csr[l_k] = LIBXSMM_VLA_ACCESS(2, l_p_a_de, l_i, l_j, C);
        l_k++;
      }
    }
  }

  /* dense routine */
  l_start = libxsmm_timer_tick();
#if 1
  for ( l_n = 0; l_n < REPS; l_n++) {
    for ( l_i = 0; l_i < NB; l_i++) {
      for ( l_j = 0; l_j < K; l_j++) {
        for ( l_jj = 0; l_jj < C; l_jj++) {
          LIBXSMM_PRAGMA_SIMD
          for (l_k = 0; l_k < nb; l_k++) {
            LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_j, l_i, l_k, NB, nb)
              +=   LIBXSMM_VLA_ACCESS(3, l_p_b, l_jj, l_i, l_k, NB, nb)
                 * l_a_de[(l_j*C)+l_jj];
          }
        }
      }
    }
  }
#endif
  l_end = libxsmm_timer_tick();
  l_total = libxsmm_timer_duration(l_start, l_end);
  printf("%fs for dense\n", l_total);
  printf("%f GFLOPS for dense\n", ((double)((double)REPS * (double)N * (double)C * (double)K) * 2.0) / (l_total * 1.0e9));

  l_xgemm_desc = libxsmm_gemm_descriptor_dinit(&l_xgemm_blob, LIBXSMM_GEMM_PRECISION(float),
    K, NB, C, 0, NB, NB, alpha, beta, flags, prefetch);

  /* sparse routine */
  mykernel_csr = libxsmm_create_xcsr_soa(l_xgemm_desc, l_rowptr, l_colidx, (const void*)l_a_sp_csr, nb).smm;

  l_start = libxsmm_timer_tick();
  for ( l_n = 0; l_n < REPS; l_n++) {
    mykernel_csr( l_a_sp_csr, l_b, l_c_asm_csr );
  }
  l_end = libxsmm_timer_tick();
  l_total = libxsmm_timer_duration(l_start, l_end);
  printf("%fs for sparse (asm, csr)\n", l_total);
  printf("%f GFLOPS for sparse (asm, csr)\n", ((double)((double)REPS * (double)N * (double)C * (double)K) * 2.0) / (l_total * 1.0e9));

  /* check for errors */
  l_max_error = 0.f;
  for ( l_i = 0; l_i < NB; l_i++) {
    for ( l_j = 0; l_j < K; l_j++) {
      for ( l_k = 0; l_k < nb; l_k++ ) {
        if (fabs( LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_j, l_i, l_k, NB, nb)
                    - LIBXSMM_VLA_ACCESS(3, l_p_c_asm_csr, l_j, l_i, l_k, NB, nb) ) > l_max_error ) {
          l_max_error = (float)fabs( LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_j, l_i, l_k, NB, nb)
                                       -LIBXSMM_VLA_ACCESS(3, l_p_c_asm_csr, l_j, l_i, l_k, NB, nb) );
        }
      }
    }
  }
  printf("max error (csr): %f\n", l_max_error);

  /* free */
  libxsmm_free( l_a_de );
  libxsmm_free( l_b );
  libxsmm_free( l_c_gold );
  libxsmm_free( l_c_asm_csr );

  libxsmm_free( l_a_sp_csr );
  libxsmm_free( l_rowptr );
  libxsmm_free( l_colidx );

  return 0;
}

