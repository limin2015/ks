/*
 * --------------------------------------------------------------------------
 * GSKS (General Stride Kernel Summation)
 * --------------------------------------------------------------------------
 * Copyright (C) 2015, The University of Texas at Austin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 *
 *
 * dgsks_ref.c
 *
 * Chenhan D. Yu - Department of Computer Science, 
 *                 The University of Texas at Austin
 *
 *
 * Purpose: 
 * this is the main file of the double precision kernel summation 
 * reference kernel implemented with a GEMM + VEXP + GEMV approach.
 *
 * Todo:
 *
 *
 * Modification:
 * Chenhan
 * Apr 27, 2015: New tanh kernel.
 *
 *
 * Chenhan
 * Mar 16, 2015: Change_ref dgsks interface to support a separate ulist.
 *               Add a new type of kernel (variable bandwidth gaussian).
 *
 *
 * */

#include <omp.h>
#include <mkl.h>
#include <ks.h>


/* 
 * --------------------------------------------------------------------------
 * @brief  This reference function will call GEMM, VEXP, GEMV.
 *
 * @param  *kernel This structure is used to specified the type of the kernel.
 * @param  m       Number of target points
 * @param  n       Number of source points
 * @param  k       Data point dimension
 * @param  *u      Potential vector
 * @param  *umap   Potential vector index map
 * @param  *XA     Target coordinate table [ k * nxa ]
 * @param  *XA2    Target square 2-norm table
 * @param  *alpha  Target points index map
 * @param  *XB     Source coordinate table [ k * nxb ]
 * @param  *XB2    Source square 2-norm table
 * @param  *beta   Source points index map
 * @param  *w      Weight vector
 * @param  *omega  Weight vector index map
 * --------------------------------------------------------------------------
 */
void dgsks_ref(
    ks_t   *kernel,
    int    m,
    int    n,
    int    k,
    double *u,
    int    *umap,
    double *XA,
    double *XA2,
    int    *alpha,
    double *XB,
    double *XB2,
    int    *beta,
    double *w,
    int    *omega
    )
{
  int    i, j, p;
  int    pack_bandwidth;
  double *As, *Bs, *Cs, *us, *ws, *hs, *powe;
  double rank_k_scale;

  double beg, tcollect, tgemm, tgemv, tkernel;


  beg = omp_get_wtime();

  // ------------------------------------------------------------------------
  // Setup kernel dependent parameters
  // ------------------------------------------------------------------------
  switch ( kernel->type ) {
    case KS_GAUSSIAN:
      pack_bandwidth = 0;
      rank_k_scale = -2.0;
      break;
    case KS_GAUSSIAN_VAR_BANDWIDTH:
      pack_bandwidth = 1;
      rank_k_scale = -2.0;
      if ( !kernel->h ) {
        printf( "Error dgsks(): bandwidth vector has been initialized yet.\n" );
      }
      //kernel->packh = (double*)malloc( sizeof(double) * n );
      break;
    case KS_POLYNOMIAL:
      pack_bandwidth = 0;
      rank_k_scale = 1.0;
      powe = (double*)malloc( sizeof(double) * m * n );
      for ( i = 0; i < m * n; i++ ) powe[ i ] = kernel->powe;
      break;
    case KS_LAPLACE:
      pack_bandwidth = 0;
      rank_k_scale = -2.0;
      if ( k < 3 ) {
        printf( "Error dgsks(): laplace kernel only supports k > 2.\n" );
      }
      kernel->powe = 0.5 * ( 2.0 - (double)k );
      kernel->scal = tgamma( 0.5 * k + 1.0 ) / 
        ( (double)k * (double)( k - 2 ) * pow( M_PI, 0.5 * k ) );
      powe = (double*)malloc( sizeof(double) * m * n );
      for ( i = 0; i < m * n; i++ ) powe[ i ] = kernel->powe;
      //printf( "powe = %lf, scal = %lf\n", kernel->powe, kernel->scal );
      break;
    case KS_TANH:
      pack_bandwidth = 0;
      rank_k_scale = 1.0;
      break;
    case KS_QUARTIC:
      pack_bandwidth = 0;
      rank_k_scale = -2.0;
      break;
    case KS_MULTIQUADRATIC:
      pack_bandwidth = 0;
      rank_k_scale = -2.0;
      break;
    case KS_EPANECHNIKOV:
      pack_bandwidth = 0;
      rank_k_scale = -2.0;
      break;
    default:
      printf( "Error dgsks_ref(): illegal kernel type\n" );
      exit( 1 );
  }
  // ------------------------------------------------------------------------


  // ------------------------------------------------------------------------
  // Allocate temporary buffers for BLAS calls.
  // ------------------------------------------------------------------------
  As = (double*)malloc( sizeof(double) * m * k );
  Bs = (double*)malloc( sizeof(double) * n * k );
  Cs = (double*)malloc( sizeof(double) * m * n );
  us = (double*)malloc( sizeof(double) * m * KS_RHS );
  ws = (double*)malloc( sizeof(double) * n * KS_RHS );
  // ------------------------------------------------------------------------



  // ------------------------------------------------------------------------
  // Collect As from XA, us from u
  // ------------------------------------------------------------------------
  #pragma omp parallel for private( p )
  for ( i = 0; i < m; i ++ ) {
    for ( p = 0; p < k; p ++ ) {
      As[ i * k + p ] = XA[ alpha[ i ] * k + p ];
    }
    for ( p = 0; p < KS_RHS; p ++ ) {
      us[ p * m + i ] = u[ umap[ i ] * KS_RHS + p ];
    }
  }
  // ------------------------------------------------------------------------


  // ------------------------------------------------------------------------
  // Collect Bs from XB, ws from w
  // ------------------------------------------------------------------------
  #pragma omp parallel for private( p )
  for ( j = 0; j < n; j ++ ) {
    for ( p = 0; p < k; p ++ ) {
      Bs[ j * k + p ] = XB[ beta[ j ] * k + p ];
    }
    for ( p = 0; p < KS_RHS; p ++ ) {
      ws[ p * n + j ] = w[ omega[ j ] * KS_RHS + p ];
    }
  }
  // ------------------------------------------------------------------------
  tcollect = omp_get_wtime() - beg;


  
  beg = omp_get_wtime();
  // ------------------------------------------------------------------------
  // C = -2.0 * A^t * B (GEMM)
  // ------------------------------------------------------------------------
  cblas_dgemm(
      CblasColMajor,
      CblasTrans,
      CblasNoTrans,
      m,
      n,
      k,
      rank_k_scale,
      As,
      k,
      Bs,
      k,
      0.0,
      Cs,
      m
      );
  // ------------------------------------------------------------------------
  tgemm = omp_get_wtime() - beg;


  beg = omp_get_wtime();
  // ------------------------------------------------------------------------
  // Apply different kernel functions
  // ------------------------------------------------------------------------
  switch ( kernel->type ) {
    case KS_GAUSSIAN:
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] += XA2[ alpha[ i ] ];
          Cs[ j * m + i ] += XB2[ beta[ j ] ];
          Cs[ j * m + i ] *= kernel->scal;
        }
        vdExp( m, Cs + j * m, Cs + j * m );
      }
      //vdExp( m * n, Cs, Cs );
      break;
    case KS_GAUSSIAN_VAR_BANDWIDTH:
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] += XA2[ alpha[ i ] ];
          Cs[ j * m + i ] += XB2[ beta[ j ] ];
          Cs[ j * m + i ] *= kernel->h[ beta[ j ] ];
        }
      }
      vdExp( m * n, Cs, Cs );
      break;
    case KS_POLYNOMIAL:
      if ( kernel->powe == 2.0 ) {
        #pragma omp parallel for private( i )
        for ( j = 0; j < n; j ++ ) {
          for ( i = 0; i < m; i ++ ) {
            Cs[ j * m + i ] *= kernel->scal;
            Cs[ j * m + i ] += kernel->cons;
            Cs[ j * m + i ] = Cs[ j * m + i ] * Cs[ j * m + i ];
          }
        }
      }
      else if ( kernel->powe == 4.0 ) {
        #pragma omp parallel for private( i )
        for ( j = 0; j < n; j ++ ) {
          for ( i = 0; i < m; i ++ ) {
            Cs[ j * m + i ] *= kernel->scal;
            Cs[ j * m + i ] += kernel->cons;
            Cs[ j * m + i ] = Cs[ j * m + i ] * Cs[ j * m + i ];
            Cs[ j * m + i ] = Cs[ j * m + i ] * Cs[ j * m + i ];
          }
        }
      }
      else {
        #pragma omp parallel for
        for ( j = 0; j < n; j ++ ) {
          vdPow( m, Cs + j * m, powe, Cs + j * m );
        }
      }
      break;
    case KS_LAPLACE:
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] += XA2[ alpha[ i ] ];
          Cs[ j * m + i ] += XB2[ beta[ j ] ];
          if ( Cs[ j * m + i ] < 1E-15 ) {
            Cs[ j * m + i ] = 1.79E+308;
          }
        }
      }
      vdPow( m * n, Cs, powe, Cs );
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] *= kernel->scal;
        }
      }
      break;
    case KS_TANH:
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] *= kernel->scal;
          Cs[ j * m + i ] += kernel->cons;
        }
        vdTanh( m, Cs + j * m, Cs + j * m );
      }
      //vdTanh( m * n, Cs, Cs );
      break;
    case KS_QUARTIC:
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] += XA2[ alpha[ i ] ];
          Cs[ j * m + i ] += XB2[ beta[ j ] ];
          if ( Cs[ j * m + i ] < 1.0 ) {
            Cs[ j * m + i ] = ( 1.0 - Cs[ j * m + i ] );
            Cs[ j * m + i ] = ( 15.0 / 16.0 ) * Cs[ j * m + i ] * Cs[ j * m + i ];
          }
          else {
            Cs[ j * m + i ] = 0.0;
          }
        }
      }
      break;
    case KS_MULTIQUADRATIC:
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] += XA2[ alpha[ i ] ];
          Cs[ j * m + i ] += XB2[ beta[ j ] ];
          Cs[ j * m + i ] += kernel->cons;
        }
      }
      break;
    case KS_EPANECHNIKOV:
      #pragma omp parallel for private( i )
      for ( j = 0; j < n; j ++ ) {
        for ( i = 0; i < m; i ++ ) {
          Cs[ j * m + i ] += XA2[ alpha[ i ] ];
          Cs[ j * m + i ] += XB2[ beta[ j ] ];
          if ( Cs[ j * m + i ] < 1.0 ) {
            Cs[ j * m + i ] = ( 1.0 - Cs[ j * m + i ] );
            Cs[ j * m + i ] = ( 3.0 / 4.0 ) * Cs[ j * m + i ];
          }
          else {
            Cs[ j * m + i ] = 0.0;
          }
        }
      }
      break;
    default:
      printf( "Error dgsks_ref(): illegal kernel type\n" );
      exit( 1 );
  }
  // ------------------------------------------------------------------------
  tkernel = omp_get_wtime() - beg;


  beg = omp_get_wtime();
  // ------------------------------------------------------------------------
  // Kernel Summation (GEMV)
  // ------------------------------------------------------------------------
  //cblas_dgemv(
  //    CblasColMajor,
  //    CblasNoTrans,
  //    m,
  //    n,
  //    1.0,
  //    Cs,
  //    m,
  //    ws,
  //    1,
  //    1.0,
  //    us,
  //    1
  //    );
  cblas_dgemm(
      CblasColMajor,
      CblasNoTrans,
      CblasNoTrans,
      m,
      KS_RHS,
      n,
      1.0,
      Cs,
      m,
      ws,
      n,
      1.0,
      us,
      m
      );
  // ------------------------------------------------------------------------
  tgemv = omp_get_wtime() - beg;


  beg = omp_get_wtime();
  // ------------------------------------------------------------------------
  // Assemble us back to u
  // ------------------------------------------------------------------------
  #pragma omp parallel for
  for ( i = 0; i < m; i ++ ) {
    for ( p = 0; p < KS_RHS; p ++ ) {
      u[ umap[ i ] * KS_RHS + p ] = us[ p * m + i ];
    }
  }
  // ------------------------------------------------------------------------
  
  // DEBUG
  /*
  printf( "u = \n" );
  for ( p = 0; p < KS_RHS; p ++ ) {
    for ( i = 0; i < m; i ++ ) {
      printf( "%lf, ", us[ p * m + i ] );
    }
    printf( "\n" );
  }

  printf( "w = \n" );
  for ( p = 0; p < KS_RHS; p ++ ) {
    for ( j = 0; j < n; j ++ ) {
      printf( "%lf, ", ws[ p * n + j ] );
    }
    printf( "\n" );
  }
  */

  



  // ------------------------------------------------------------------------
  // Free the temporary buffers
  // ------------------------------------------------------------------------
  free( As );
  free( Bs );
  free( Cs );
  free( us );
  free( ws );
  // ------------------------------------------------------------------------
  

  // ------------------------------------------------------------------------
  // Free kernel dependent buffers
  // ------------------------------------------------------------------------
  switch ( kernel->type ) {
    case KS_GAUSSIAN:
      break;
    case KS_GAUSSIAN_VAR_BANDWIDTH:
      //free( kernel->packh );
      break;
    case KS_POLYNOMIAL:
      free( powe );
      break;
    case KS_LAPLACE:
      free( powe );
      break;
    case KS_TANH:
      break;
    case KS_QUARTIC:
      break;
    case KS_MULTIQUADRATIC:
      break;
    case KS_EPANECHNIKOV:
      break;
    default:
      printf( "Error dgsks_ref(): illegal kernel type\n" );
      exit( 1 );
  }
  // ------------------------------------------------------------------------
  tcollect += ( omp_get_wtime() - beg );

  //printf( "%5.3lf, %5.3lf, %5.3lf, %5.3lf, %5.3lf\n", 
  //    tcollect, tgemm, tkernel, tgemv, tcollect + tgemm + tkernel + tgemv );
}
