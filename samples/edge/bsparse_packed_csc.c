/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/libxsmm/libxsmm/                    *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Alexander Heinecke (Intel Corp.)
******************************************************************************/
#include <libxsmm_utils.h>
#include <libxsmm.h>
#include "common_edge_proxy.h"

int main(int argc, char* argv[]) {
  libxsmm_blasint M = ( argc == 7 ) ? atoi(argv[1]) : 9;
  libxsmm_blasint N = ( argc == 7 ) ? atoi(argv[2]) : 10;
  libxsmm_blasint K = ( argc == 7 ) ? atoi(argv[3]) : 20;
  libxsmm_blasint N_CRUNS = ( argc == 7 ) ? atoi(argv[4]) : 8;
  libxsmm_blasint REPS =    ( argc == 7 ) ? atoi(argv[5]) : 1;
  const char* l_csc_file =  ( argc == 7 ) ?      argv[6]  : "file.csc";

  libxsmm_xmmfunction mykernel = { NULL };
  const libxsmm_gemm_shape gemm_shape = libxsmm_create_gemm_shape(
    M, N, K, K, 0, N, LIBXSMM_DATATYPE(REALTYPE),
    LIBXSMM_DATATYPE(REALTYPE), LIBXSMM_DATATYPE(REALTYPE), LIBXSMM_DATATYPE(REALTYPE) );
  const libxsmm_bitfield l_flags = LIBXSMM_GEMM_FLAGS('N', 'N');
  const libxsmm_bitfield l_prefetch_flags = LIBXSMM_GEMM_PREFETCH_NONE;
  libxsmm_gemm_param gemm_param;

  edge_mat_desc mat_desc = libxsmm_sparse_csc_reader_desc( l_csc_file );
  unsigned int l_rowcount = mat_desc.row_count;
  unsigned int l_colcount = mat_desc.col_count;
  unsigned int l_elements = mat_desc.num_elements;

  REALTYPE* l_a = (REALTYPE*)libxsmm_aligned_malloc(K * M * N_CRUNS * sizeof(REALTYPE), 64);
  REALTYPE* l_b_de = (REALTYPE*)libxsmm_aligned_malloc(K * N * sizeof(REALTYPE), 64);
  REALTYPE* l_b_sp = NULL;
  unsigned int* l_colptr = NULL;
  unsigned int* l_rowidx = NULL;
  REALTYPE* l_b_sp_padded = NULL;
  unsigned int* l_colptr_padded = NULL;
  unsigned int* l_rowidx_padded = NULL;
  REALTYPE* l_c_gold = (REALTYPE*)libxsmm_aligned_malloc(M * N * N_CRUNS * sizeof(REALTYPE), 64);
  REALTYPE* l_c_asm = (REALTYPE*)libxsmm_aligned_malloc(M * N * N_CRUNS * sizeof(REALTYPE), 64);
  REALTYPE l_max_error = 0.0;
  libxsmm_blasint l_k, l_n, l_i, l_j, l_jj;

  LIBXSMM_VLA_DECL(3, REALTYPE, l_p_a, l_a, K, N_CRUNS);
  LIBXSMM_VLA_DECL(3, REALTYPE, l_p_c_asm, l_c_asm, N, N_CRUNS);
  LIBXSMM_VLA_DECL(3, REALTYPE, l_p_c_gold, l_c_gold, N, N_CRUNS);

  libxsmm_kernel_info l_kinfo;
  unsigned long long l_libxsmmflops;
  libxsmm_timer_tickint l_start, l_end;
  double l_total;
  int result = EXIT_SUCCESS;

  if (argc != 7) {
    fprintf( stderr, "arguments: M CRUNS #iters csc-file!\n" );
    result = EXIT_FAILURE;
  }

  if ((unsigned int)K != l_rowcount) {
    fprintf( stderr, "arguments K needs to match number of rows of the sparse matrix!\n" );
    result = EXIT_FAILURE;
  }

  if ((unsigned int)N != l_colcount) {
    fprintf( stderr, "arguments N needs to match number of columns of the sparse matrix!\n" );
    result = EXIT_FAILURE;
  }

  if (M != 9) {
    fprintf( stderr, "arguments M needs to match 9!\n" );
    result = EXIT_FAILURE;
  }

  if (NULL == l_a || NULL == l_b_de || NULL == l_c_gold || NULL == l_c_asm) {
    fprintf( stderr, "memory allocation failed!\n" );
    result = EXIT_FAILURE;
  }

  do if (EXIT_SUCCESS == result) {
    /* touch A */
    for ( l_i = 0; l_i < M; l_i++) {
      for ( l_j = 0; l_j < K; l_j++) {
        for ( l_k = 0; l_k < N_CRUNS; l_k++ ) {
          LIBXSMM_VLA_ACCESS(3, l_p_a, l_i, l_j, l_k, K, N_CRUNS) = (REALTYPE)libxsmm_rng_f64();
        }
      }
    }

    /* touch C */
    for ( l_i = 0; l_i < M; l_i++) {
      for ( l_j = 0; l_j < N; l_j++) {
        for ( l_k = 0; l_k < N_CRUNS; l_k++ ) {
          LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_i, l_j, l_k, N, N_CRUNS) = (REALTYPE)0.0;
          LIBXSMM_VLA_ACCESS(3, l_p_c_asm,  l_i, l_j, l_k, N, N_CRUNS) = (REALTYPE)0.0;
        }
      }
    }

    /* read B, csc */
    libxsmm_sparse_csc_reader(  l_csc_file,
                               &l_colptr,
                               &l_rowidx,
                               &l_b_sp,
                               &l_rowcount, &l_colcount, &l_elements );
    if (NULL == l_b_sp || NULL == l_colptr || NULL == l_rowidx) {
      result = EXIT_FAILURE;
      break;
    }

    /* copy b to dense */
    printf("csc matrix data structure we just read:\n");
    printf("rows: %u, columns: %u, elements: %u\n", l_rowcount, l_colcount, l_elements);

    for ( l_n = 0; l_n < (K * N); l_n++) {
      l_b_de[l_n] = 0.0;
    }

    for ( l_n = 0; l_n < N; l_n++) {
      const libxsmm_blasint l_colelems = l_colptr[l_n+1] - l_colptr[l_n];
      assert(l_colptr[l_n+1] >= l_colptr[l_n]);

      for ( l_k = 0; l_k < l_colelems; l_k++) {
        l_b_de[(l_rowidx[l_colptr[l_n] + l_k] * N) + l_n] = l_b_sp[l_colptr[l_n] + l_k];
      }
    }

    /* dense routine */
    l_start = libxsmm_timer_tick();
#if 1
    for ( l_n = 0; l_n < REPS; l_n++) {
      for ( l_i = 0; l_i < M; l_i++) {
        for ( l_j = 0; l_j < N; l_j++) {
          for ( l_jj = 0; l_jj < K; l_jj++) {
            LIBXSMM_PRAGMA_SIMD
            for (l_k = 0; l_k < N_CRUNS; l_k++) {
              LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_i, l_j, l_k, N, N_CRUNS)
                +=   LIBXSMM_VLA_ACCESS(3, l_p_a, l_i, l_jj, l_k, K, N_CRUNS)
                   * l_b_de[(l_jj*N)+l_j];
            }
          }
        }
      }
    }
#endif
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);
    printf("%fs for dense\n", l_total);
    printf("%f GFLOPS for dense\n", ((double)((double)REPS * (double)M * (double)N * (double)K * (double)N_CRUNS) * 2.0) / (l_total * 1.0e9));

    mykernel.gemm = libxsmm_create_packed_spgemm_csc( gemm_shape, l_flags, l_prefetch_flags, N_CRUNS, l_colptr, l_rowidx, (const void*)l_b_sp );

    memset( &gemm_param, 0, sizeof(libxsmm_gemm_param) );
    gemm_param.a.primary = (void*)l_a;
    gemm_param.b.primary = (void*)l_b_sp;
    gemm_param.c.primary = (void*)l_c_asm;
    l_start = libxsmm_timer_tick();
    for ( l_n = 0; l_n < REPS; l_n++) {
      mykernel.gemm( &gemm_param );
    }
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);
    libxsmm_get_kernel_info( mykernel.ptr_const, &l_kinfo);
    l_libxsmmflops = l_kinfo.nflops;
    printf("%fs for sparse (asm)\n", l_total);
    printf("%f GFLOPS for sparse (asm), calculated\n", ((double)((double)REPS * (double)M * (double)l_elements * (double)N_CRUNS) * 2.0) / (l_total * 1.0e9));
    printf("%f GFLOPS for sparse (asm), libxsmm   \n", ((double)((double)REPS * (double)l_libxsmmflops)) / (l_total * 1.0e9));

    /* check for errors */
    l_max_error = (REALTYPE)0.0;
    for ( l_i = 0; l_i < M; l_i++) {
      for ( l_j = 0; l_j < N; l_j++) {
        for ( l_k = 0; l_k < N_CRUNS; l_k++ ) {
          if (LIBXSMM_FABS( LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_i, l_j, l_k, N, N_CRUNS)
                      - LIBXSMM_VLA_ACCESS(3, l_p_c_asm, l_i, l_j, l_k, N, N_CRUNS) ) > l_max_error ) {
            l_max_error = (REALTYPE)LIBXSMM_FABS( LIBXSMM_VLA_ACCESS(3, l_p_c_gold, l_i, l_j, l_k, N, N_CRUNS)
                                         -LIBXSMM_VLA_ACCESS(3, l_p_c_asm, l_i, l_j, l_k, N, N_CRUNS) );
          }
        }
      }
    }
    printf("max error: %f\n", l_max_error);
    printf("PERFDUMP,%s,%u,%i,%i,%i,%u,%u,%f,%f,%f\n", l_csc_file,
      (unsigned int)REPS, M, N, K, l_elements, (unsigned int)M * l_elements * (unsigned int)N_CRUNS * 2u, l_max_error, l_total,
      ((double)((double)REPS * (double)M * (double)l_elements * (double)N_CRUNS) * 2.0) / (l_total * 1.0e9) );
  }
  while (0);

  /* free */
  libxsmm_free( l_b_de );
  libxsmm_free( l_a );
  libxsmm_free( l_c_gold );
  libxsmm_free( l_c_asm );

  free( l_b_sp );
  free( l_colptr );
  free( l_rowidx );
  if ( l_b_sp_padded != NULL )   free( l_b_sp_padded );
  if ( l_colptr_padded != NULL ) free( l_colptr_padded );
  if ( l_rowidx_padded != NULL ) free( l_rowidx_padded );

  return result;
}
