/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/libxsmm/libxsmm/                    *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Hans Pabst (Intel Corp.)
******************************************************************************/

/** This sample uses LIBXSMM's header-only implementation. */
#include <libxsmm_source.h>

#if !defined(USE_LIBXSMM)
# define USE_LIBXSMM
#endif

#if defined(USE_LIBXSMM)
# if !defined(EIGEN_VECTORIZE_AVX)
#   define EIGEN_VECTORIZE_AVX
# endif
# if !defined(EIGEN_USE_LIBXSMM)
#   define EIGEN_USE_LIBXSMM
# endif
#endif

#if !defined(__EIGEN) && !defined(__EIGEN_UNSUPPORTED) && 0
# define __EIGEN_UNSUPPORTED
# define __EIGEN
#endif

#if !defined(EIGEN_USE_THREADS) && defined(__EIGEN) && (defined(_OPENMP) \
 || !defined(__BLAS) || (defined(__BLAS) && 1 < (__BLAS)))
# define EIGEN_USE_THREADS
#endif

#if defined(__EIGEN_UNSUPPORTED)
# include <unsupported/Eigen/CXX11/Tensor>
# include <unsupported/Eigen/CXX11/ThreadPool>
#endif
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cstdlib>
#include <cstdio>

#if !defined(REALTYPE)
# define REALTYPE float
#endif

#if !defined(CHECK) && (LIBXSMM_EQUAL(REALTYPE, float) || LIBXSMM_EQUAL(REALTYPE, double))
# if !defined(MKL_DIRECT_CALL_SEQ) && !defined(MKL_DIRECT_CALL)
LIBXSMM_BLAS_SYMBOL_DECL(REALTYPE, gemm)
# endif
# define CHECK
#endif


int main(int argc, char* argv[])
{
  int result = EXIT_SUCCESS;
  try {
#if !defined(__EIGEN_UNSUPPORTED)
    LIBXSMM_UNUSED(argc); LIBXSMM_UNUSED(argv);
    throw std::runtime_error("Eigen or Eigen/unsupported not found!");
#else
    LIBXSMM_BLAS_CONST libxsmm_blasint m = (1 < argc ? std::atoi(argv[1]) : 512);
    LIBXSMM_BLAS_CONST libxsmm_blasint k = (3 < argc ? atoi(argv[3]) : m);
    LIBXSMM_BLAS_CONST libxsmm_blasint n = (2 < argc ? atoi(argv[2]) : k);
    const int nrepeat = LIBXSMM_MAX(4 < argc ? atoi(argv[4]) : 13 / LIBXSMM_MAX(1, libxsmm_icbrt_u64(1ULL * m * n * k) >> 10), 3);
# if defined(CHECK) && (!defined(__BLAS) || (0 != __BLAS))
    const double env_check = (NULL == getenv("CHECK") ? 1.0 : atof(getenv("CHECK")));
    const double check = LIBXSMM_ABS(env_check);
# endif
    const double gflops = 2.0 * m * n * k * 1E-9;
    const int max_nthreads = Eigen::nbThreads();
    const int env_nthreads = (NULL == getenv("NTHREADS") ? max_nthreads : atoi(getenv("NTHREADS")));
    const int nthreads = LIBXSMM_CLMP(env_nthreads, 1, max_nthreads);

    Eigen::ThreadPool threadpool(nthreads);
    Eigen::ThreadPoolDevice device(&threadpool, threadpool.NumThreads());
    typedef Eigen::Tensor<REALTYPE,2/*nindices*/,0/*options*/,libxsmm_blasint> tensor_type;
    tensor_type ta(m, k), tb(k, n), tc(m, n);
    LIBXSMM_BLAS_CONST char transa = 'N', transb = 'N';
    LIBXSMM_BLAS_CONST REALTYPE alpha(1), beta(0);
    libxsmm_timer_tickint start
    double d1;
    {
      std::array<Eigen::IndexPair<libxsmm_blasint>,1> product_dims = {
        Eigen::IndexPair<libxsmm_blasint>(1, 0),
      };
      ta.setRandom(); tb.setRandom();
      start = libxsmm_timer_tick();
      for (int i = 0; i < nrepeat; ++i) {
        const tensor_type& tt = ta.contract(tb, product_dims);
        tc.device(device) = tt;
      }
      d1 = libxsmm_timer_duration(start, libxsmm_timer_tick());
    }
    libxsmm_gemm_print(stdout, libxsmm_datatype_enum<REALTYPE>::value, &transa, &transb,
      &m, &n, &k, &alpha, ta.data(), &m, tb.data(), &k, &beta, tc.data(), &m);
    fprintf(stdout, "\n\n");
# if defined(CHECK) && (!defined(__BLAS) || (0 != __BLAS))
    Eigen::Tensor<REALTYPE, 2/*nindices*/, 0/*options*/, libxsmm_blasint> td(m, n);
    double d2;
    {
      start = libxsmm_timer_tick();
      for (int i = 0; i < nrepeat; ++i) {
        LIBXSMM_GEMM_SYMBOL(REALTYPE)(&transa, &transb, &m, &n, &k,
          &alpha, ta.data(), &m, tb.data(), &k,
            &beta, td.data(), &m);
      }
      d2 = libxsmm_timer_duration(start, libxsmm_timer_tick());
    }
# endif
    if (0 < d1) {
      fprintf(stdout, "\tEigen"
# if !defined(USE_LIBXSMM)
        "+XSMM"
# endif
        ": %.1f GFLOPS/s\n", gflops * nrepeat / d1);
    }
# if defined(CHECK) && (!defined(__BLAS) || (0 != __BLAS))
    if (0 < d2) {
      fprintf(stdout, "\tBLAS: %.1f GFLOPS/s\n", gflops * nrepeat / d2);
    }
    libxsmm_matdiff_info diff;
    result = libxsmm_matdiff(&diff, LIBXSMM_DATATYPE(REALTYPE), m, n, td.data(), tc.data(), &m, &m);
    if (EXIT_SUCCESS == result) {
      fprintf(stdout, "\tdiff: L2abs=%f Linf=%f\n", diff.l2_abs, diff.linf_abs);
      if (check < diff.l2_rel) {
        fprintf(stderr, "FAILED.\n");
        result = EXIT_FAILURE;
      }
    }
# endif
    fprintf(stdout, "Finished\n");
#endif /*defined(__EIGEN_UNSUPPORTED)*/
  }
  catch(const std::exception& e) {
    fprintf(stderr, "Error: %s\n", e.what());
    result = EXIT_FAILURE;
  }
  catch(const char* message) {
    fprintf(stderr, "Error: %s\n", message);
    result = EXIT_FAILURE;
  }
  catch(...) {
    fprintf(stderr, "Error: unknown exception caught!\n");
    result = EXIT_FAILURE;
  }

  return result;
}

