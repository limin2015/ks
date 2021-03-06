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
 * dgsks.c
 *
 * Chenhan D. Yu - Department of Computer Science, 
 *                 The University of Texas at Austin
 *
 *
 * Purpose: 
 * this is the main file of the double precision general stride
 * kernel summation kernel.
 *
 *
 * Todo:
 *
 *
 * Modification:
 *
 * Chenhan
 * May 05, 2015: Fix the double free bug in kernel->packh. Now I discard 
 *               kernel->packh, but allocate packh in dgsks. In this case,
 *               there is an additional parameter needed to be passed to
 *               the macro-kernel.
 *
 * Chenhan
 * Jun 29, 2015: var2 has all been deprecated. Now all micro-kernels will
 *               decide whether or not to accumulate the rank-k update
 *               result based on the value of pc. Also the micro-kernel
 *               interface has been unified; thus, now they are called by
 *               function pointers. See the head file ks_kernel.h in the
 *               micro-kernel directory.
 *
 * */

#include <ks.h>
#include <gsks_internal.h>
#include <gsks_config.h>
#include <gsks_kernel.h>


#define min( i, j ) ( (i)<(j) ? (i): (j) )



/* 
 * --------------------------------------------------------------------------
 * @brief  This is the packing routine that packs the target coordinates
 *         into a Z shape contiguous buffer.
 * --------------------------------------------------------------------------
 */
inline void packA_kcxmc(
    int    m,
    int    k,
    double *XA,
    int    ldXA,
    int    *amap,
    double *packA
    )
{
  int    i, p;
  double *a_pntr[ DKS_PACK_MR ];

  for ( i = 0; i < m; i ++ ) {
    a_pntr[ i ] = XA + ldXA * amap[ i ];
  }

  for ( i = m; i < DKS_PACK_MR; i ++ ) {
    a_pntr[ i ] = XA + ldXA * amap[ 0 ];
  }

  for ( p = 0; p < k; p ++ ) {
    for ( i = 0; i < DKS_PACK_MR; i ++ ) {
      *packA ++ = *a_pntr[ i ] ++;
    }
  }
}


/* 
 * --------------------------------------------------------------------------
 * @brief  This is the packing routine that packs the source coordinates
 *         into a Z shape contiguous buffer.
 * --------------------------------------------------------------------------
 */
inline void packB_kcxnc(
    int    n,
    int    k,
    double *XB,
    int    ldXB, // ldXB is the original k
    int    *bmap,
    double *packB
    )
{
  int    j, p; 
  //double *b_pntr[ DKS_NR ];
  double *b_pntr[ DKS_PACK_NR ];

  for ( j = 0; j < n; j ++ ) {
    b_pntr[ j ] = XB + ldXB * bmap[ j ];
  }

  //for ( j = n; j < DKS_NR; j ++ ) {
  for ( j = n; j < DKS_PACK_NR; j ++ ) {
    b_pntr[ j ] = XB + ldXB * bmap[ 0 ];
  }

  for ( p = 0; p < k; p ++ ) {
    //for ( j = 0; j < DKS_NR; j ++ ) {
    for ( j = 0; j < DKS_PACK_NR; j ++ ) {
      *packB ++ = *b_pntr[ j ] ++;
    }
  }
}


inline void packw_rhsxnc(
    int    n,
    int    rhs,
    double *w,
    int    ldw, // ldw should be rhs
    int    *wmap,
    double *packw
    )
{
  int    j, p;
  double *w_pntr[ DKS_PACK_NR ];

  for ( j = 0; j < n; j ++ ) {
    w_pntr[ j ] = w + ldw * wmap[ j ];
  }

  for ( p = 0; p < rhs; p ++ ) {
    for ( j = 0; j < n; j ++ ) {
      *packw ++ = *w_pntr[ j ] ++;
    }
    for ( j = n; j < DKS_PACK_NR; j ++ ) {
      *packw ++ = 0.0;
    }
  }
}


inline void packu_rhsxmc(
    int    m,
    int    rhs,
    double *u,
    int    ldu, // ldu should be rhs
    int    *umap,
    double *packu
    )
{
  int    i, p;
  double *u_pntr[ DKS_PACK_MR ];

  for ( i = 0; i < m; i ++ ) {
    u_pntr[ i ] = u + ldu * umap[ i ];
  }

  for ( p = 0; p < rhs; p ++ ) {
    for ( i = 0; i < m; i ++ ) {
      *packu ++ = *u_pntr[ i ] ++;
    }
    for ( i = m; i < DKS_PACK_MR; i ++ ) {
      packu ++;
    }
  }
}


inline void unpacku_rhsxmc(
    int    m,
    int    rhs,
    double *u,
    int    ldu, // ldu should be rhs
    int    *umap,
    double *packu
    )
{
  int    i, p;
  double *u_pntr[ DKS_PACK_MR ];

  for ( i = 0; i < m; i ++ ) {
    u_pntr[ i ] = u + ldu * umap[ i ];
  }

  for ( p = 0; p < rhs; p ++ ) {
    for ( i = 0; i < m; i ++ ) {
      *u_pntr[ i ] ++ = *packu ++;
    }
    for ( i = m; i < DKS_PACK_MR; i ++ ) {
      packu ++;
    }
  }
}




/* 
 * --------------------------------------------------------------------------
 * @brief  This is macro-kernel of the rank-k update subroutine, containing
 *         the 3 rd and the 2 nd loop. In the very middle of the routine,
 *         a rank-k update micro-kernel will be called to compute the 
 *         1.st loop ( k loop ).
 *
 * @param  m       Number of target points
 * @param  n       Number of source points
 * @param  k       Data point dimension
 * @param  *packA  Packed target coordinates
 * @param  *packB  Packed source coordinates
 * @param  *packC  Packed accumulated rank-k update
 * @param  ldc     Leading dimension of packC
 * @param  pc      This is the 5.th loop counter which indicates whether
 *                 this macro-kernel is first call. The micro-kernel won't
 *                 load the packC if this is the first call.
 * --------------------------------------------------------------------------
 */
void rank_k_macro_kernel(
    int    m,
    int    n,
    int    k,
    double *packA,
    double *packB,
    double *packC,
    int    ldc,
    int    pc
    )
{
  int    i, j, ip, jp;
  aux_t  aux;

  aux.pc     = pc;
  aux.b_next = packB;

  for ( j = 0, jp = 0; j < n; j += DKS_NR, jp += DKS_PACK_NR ) {
    for ( i = 0, ip = 0; i < m; i += DKS_MR, ip += DKS_PACK_MR ) {
      if ( i + DKS_MR >= m ) {
        aux.b_next += DKS_PACK_NR * k;
      }
      ( *rankk ) (
          k,
          &packA[ ip * k ],
          &packB[ jp * k ],
          &packC[ j * ldc + i * DKS_NR ],             // packed
          ldc,
          &aux
          );
    }
  }
}


/* 
 * --------------------------------------------------------------------------
 * @brief  This is macro-kernel will be called if k > KC. The macro-kernel
 *         includes the 3.rd and the 2.nd loop, calling a mr x nr 
 *         micro-kernel which will load the accumulated rank-k update in 
 *         packC.
 *
 * @param  *kernel This structure is used to specified the type of the kernel.
 * @param  m       Number of target points
 * @param  n       Number of source points
 * @param  k       Data point dimension
 * @param  *packu  Packed potential vector, packu = u[ umap[] ];
 * @param  *packA  Packed target coordinates
 * @param  *packA2 Packed target square 2-norm
 * @param  *packB  Packed source coordinates
 * @param  *packB2 Packed source square 2-norm
 * @param  *packw  Packed weight vector, packw = w[ wmap[] ];
 * @param  *packC  Packed accumulated rank-k update
 * @param  ldc     Leading dimension of packC
 * @param  pc      This is the 5.th loop counter which indicates whether
 *                 this macro-kernel is first call. The micro-kernel won't
 *                 load the packC if this is the first call.
 * --------------------------------------------------------------------------
 */
void dgsks_macro_kernel(
    ks_t   *kernel,
    int    m,
    int    n,
    int    k,
    double *packu,
    double *packA,
    double *packA2,
	double *packAh,
    double *packB,
    double *packB2,
	double *packBh,
    double *packw,
    double *packC,
    int    ldc,
    int    pc
    )
{
  int    i, j, ip, jp;
  aux_t  aux;

  aux.pc     = pc;
  aux.b_next = packB;

  for ( j = 0, jp = 0; j < n; j += DKS_NR, jp += DKS_PACK_NR ) {
    for ( i = 0, ip = 0; i < m; i += DKS_MR, ip += DKS_PACK_MR ) {
      if ( i + DKS_MR >= m ) {
        aux.b_next += DKS_PACK_NR * k;
      }
	  aux.hi = packAh + ip;
	  aux.hj = packBh + jp;
      ( *micro[ kernel->type ] )(
          k,
          KS_RHS,
          packu  + ip * KS_RHS,
          packA2 + ip,
          packA  + ip * k,
          packB2 + jp,
          packB  + jp * k,
          packw  + jp * KS_RHS,
          packC  + j  * ldc + i * DKS_NR, // packed
          kernel,
          &aux
          );
    }
  }
}



/* 
 * --------------------------------------------------------------------------
 * @brief  This is the main routine of the double precision general stride
 *         kernel summation.
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
void dgsks(
    ks_t   *kernel,
    int    m,
    int    n,
    int    k,
    double *u,
    int    *umap,         // New feature, a separate ulist
    double *XA,
    double *XA2,
    int    *amap,
    double *XB,
    double *XB2,
    int    *bmap,
    double *w,
    int    *wmap
    )
{
  int    i, j, p, ip, jp;
  int    ic, ib, jc, jb, pc, pb;
  int    ir, jr;
  int    pack_norm, pack_bandwidth, ks_ic_nt;
  int    ldc, padn;
  double *packA, *packB, *packC, *packw, *packu;
  double *packA2 = NULL, *packB2 = NULL, *packAh = NULL, *packBh = NULL;
  char   *str;

  // Early return if possible
  if ( m == 0 || n == 0 || k == 0 ) {
    printf( "dgsks(): early return\n" );
    return;
  }

  // Sequential is the default situation.
  ks_ic_nt = 1;

  // Check the environment variable.
  str = getenv( "KS_IC_NT" );
  if ( str != NULL ) {
    ks_ic_nt = (int)strtol( str, NULL, 10 );
  }

  packA  = ks_malloc_aligned( DKS_KC, ( DKS_PACK_MC + 1 ) * ks_ic_nt, sizeof(double) ); 
  packA2 = ks_malloc_aligned(      1, ( DKS_PACK_MC + 1 ) * ks_ic_nt, sizeof(double) ); 
  packu  = ks_malloc_aligned( KS_RHS, ( DKS_PACK_MC + 1 ) * ks_ic_nt, sizeof(double) ); 
  packB  = ks_malloc_aligned( DKS_KC, ( DKS_PACK_NC + 1 )           , sizeof(double) ); 
  packB2 = ks_malloc_aligned(      1, ( DKS_PACK_NC + 1 )           , sizeof(double) ); 
  packw  = ks_malloc_aligned( KS_RHS, ( DKS_PACK_NC + 1 )           , sizeof(double) ); 

  // Initilize packA2 and packB2 from getting nan.
  for ( j = 0; j < DKS_PACK_NC + 1; j ++ ) packB2[ j ] = 0.0;
  for ( i = 0; i < DKS_PACK_MC + 1; i ++ ) packA2[ i ] = 0.0;


  switch ( kernel->type ) {
    case KS_GAUSSIAN:
      //printf( "dgsks(): Gaussian kernel\n" );
      pack_bandwidth = 0;
      pack_norm      = 1;
      break;
    case KS_GAUSSIAN_VAR_BANDWIDTH:
      if ( !kernel->hi || !kernel->hj ) {
        printf( "Error dgsks(): bandwidth vector has been initialized yet.\n" );
      }
      pack_bandwidth = 1;
      pack_norm      = 1;
      packAh         = ks_malloc_aligned( 1, ( DKS_PACK_MC + 1 ) * ks_ic_nt, sizeof(double) ); 
      packBh         = ks_malloc_aligned( 1, ( DKS_PACK_NC + 1 ), sizeof(double) ); 
      break;
    case KS_POLYNOMIAL:
      pack_bandwidth = 0;
      pack_norm      = 0;
      break;
    case KS_LAPLACE:
      pack_bandwidth = 0;
      pack_norm      = 1;
      if ( k < 3 ) {
        printf( "Error dgsks(): laplace kernel only supports k > 2.\n" );
      }
      kernel->powe = 0.5 * ( 2.0 - (double)k );
      kernel->scal = tgamma( 0.5 * k + 1.0 ) / 
        ( (double)k * (double)( k - 2 ) * pow( M_PI, 0.5 * k ) );
      break;
    case KS_TANH:
      pack_bandwidth = 0;
      pack_norm      = 0;
      break;
    case KS_QUARTIC:
      pack_bandwidth = 0;
      pack_norm      = 1;
      break;
    case KS_MULTIQUADRATIC:
      pack_bandwidth = 0;
      pack_norm      = 1;
      break;
    case KS_EPANECHNIKOV:
      pack_bandwidth = 0;
      pack_norm      = 1;
      break;
    default:
      printf( "Error dgsks(): illegal kernel type\n" );
      exit( 1 );
  }


  if ( k > DKS_KC ) {
    ldc  = ( ( m - 1 ) / DKS_PACK_MR + 1 ) * DKS_PACK_MR;
    padn = DKS_NC;
    if ( n < DKS_NC ) {
      padn = ( ( n - 1 ) / DKS_PACK_NR + 1 ) * DKS_PACK_NR;
    }

    packC = ks_malloc_aligned( ldc, padn, sizeof(double) ); 

    for ( jc = 0; jc < n; jc += DKS_NC ) {            // 6-th loop
      jb = min( n - jc, DKS_NC );
      for ( pc = 0; pc < k; pc += DKS_KC ) {          // 5-th loop
        pb = min( k - pc, DKS_KC );

        #pragma omp parallel for num_threads( ks_ic_nt ) private( j, jr, jp )
        for ( j = 0, jp = 0; j < jb; j += DKS_NR, jp += DKS_PACK_NR ) {
          
          if ( pc + DKS_KC >= k ) {
            packw_rhsxnc(                            // packw
                min( jb - j, DKS_NR ),
                KS_RHS,
                w,
                KS_RHS,
                &wmap[ jc + j ],
                &packw[ jp * KS_RHS ]
                );

            // packB2, packh (alternatively)
            for ( jr = 0; jr < min( jb - j, DKS_NR ); jr ++ ) {
              if ( pack_norm ) {
                packB2[ jp + jr ] = XB2[ bmap[ jc + j + jr ] ];
              }
              if ( pack_bandwidth ) {
                packBh[ jp + jr ] = kernel->hj[ bmap[ jc + j + jr ] ];
              }
            }
          }

          packB_kcxnc(
              min( jb - j, DKS_NR ),
              pb,
              &XB[ pc ],
              k, // should be ldXB instead
              &bmap[ jc + j ],
              &packB[ jp * pb ]
              );
        }

        #pragma omp parallel for num_threads( ks_ic_nt ) private( ic, ib, i, ir, ip )
        for ( ic = 0; ic < m; ic += DKS_MC ) {        // 4-th loop

          // Get the thread id ( 0 ~ 9 )
          int     tid = omp_get_thread_num();

          ib = min( m - ic, DKS_MC );
          for ( i = 0, ip = 0; i < ib; i += DKS_MR, ip += DKS_PACK_MR ) {
            if ( pc + DKS_KC >= k ) {

              packu_rhsxmc(
                  min( ib - i, DKS_MR ),
                  KS_RHS,
                  u,
                  KS_RHS,
                  &umap[ ic + i ],
                  &packu[ tid * DKS_PACK_MC * KS_RHS + ip * KS_RHS ]
                  );


              for ( ir = 0; ir < min( ib - i, DKS_MR ); ir ++ ) {
                if ( pack_norm ) {
                  packA2[ tid * DKS_PACK_MC + ip + ir ] = XA2[ amap[ ic + i + ir ] ];
                }
                if ( pack_bandwidth ) {
                  packAh[ tid * DKS_PACK_MC + ip + ir ] = kernel->hi[ amap[ ic + i + ir ] ];
				}
              }
            }
            packA_kcxmc(
                min( ib - i, DKS_MR ),
                pb,
                &XA[ pc ],
                k,
                &amap[ ic + i ],
                &packA[ tid * DKS_PACK_MC * pb + ip * pb ]
                );
          }

          // Check if this is the last kc interation
          if ( pc + DKS_KC < k ) {
            rank_k_macro_kernel(
                ib,
                jb,
                pb,
                packA   + tid * DKS_PACK_MC * pb,
                packB,
                packC   + ic * padn,                  // packed
                ( ( ib - 1 ) / DKS_MR + 1 ) * DKS_MR, // packed ldc
                pc
                );
          }
          else {
            dgsks_macro_kernel(                       // 1~3 loops
                kernel,
                ib,
                jb,
                pb,
                packu  + tid * DKS_PACK_MC * KS_RHS,
                packA  + tid * DKS_PACK_MC * pb,
                packA2 + tid * DKS_PACK_MC,
                packAh + tid * DKS_PACK_MC,
                packB,
                packB2,
                packBh,
                packw,
                packC  + ic * padn,                   // packed
                ( ( ib - 1 ) / DKS_MR + 1 ) * DKS_MR, // packed ldc
                pc
                );

            /* Unpack u */
            for ( i = 0, ip = 0; i < ib; i += DKS_MR, ip +=DKS_PACK_MR ) {
              unpacku_rhsxmc(
                  min( ib - i, DKS_MR ),
                  KS_RHS,
                  u,
                  KS_RHS,
                  &umap[ ic + i ],
                  &packu[ tid * DKS_PACK_MC * KS_RHS + ip * KS_RHS ]
                  );
            }
          }
        }
      }
    }
#ifdef GSKS_MIC_AVX512
    hbw_free( packC );
#else
    free( packC );
#endif
  }
  else {

    for ( jc = 0; jc < n; jc += DKS_NC ) {            // 6-th loop
      jb = min( n - jc, DKS_NC );
      for ( pc = 0; pc < k; pc += DKS_KC ) {          // 5-th loop
        pb = min( k - pc, DKS_KC );

        #pragma omp parallel for num_threads( ks_ic_nt ) private( j, jr, jp )
        for ( j = 0, jp = 0; j < jb; j += DKS_NR, jp += DKS_PACK_NR ) {

          packw_rhsxnc(
            min( jb - j, DKS_NR ),
            KS_RHS,
            w,
            KS_RHS,
            &wmap[ jc + j ],
            &packw[ jp * KS_RHS ]
            );

          // packB2 and packh
          for ( jr = 0; jr < min( jb - j, DKS_NR ); jr ++ ) {
            if ( pack_norm ) {
              packB2[ jp + jr ] = XB2[ bmap[ jc + j + jr ] ];
            }
            if ( pack_bandwidth ) {
              packBh[ jp + jr ] = kernel->hj[ bmap[ jc + j + jr ] ];
            }
          }

          // packB
          packB_kcxnc(
              min( jb - j, DKS_NR ),
              pb,
              XB,
              k, // should be ldXB instead
              &bmap[ jc + j ],
              &packB[ jp * k ]
              );
        }

        #pragma omp parallel for num_threads( ks_ic_nt ) private( ic, ib, i, ir, ip )
        for ( ic = 0; ic < m; ic += DKS_MC ) {       // 4-th loop

          // Get the thread id ( 0 ~ 9 )
          int     tid = omp_get_thread_num();

          ib = min( m - ic, DKS_MC );
          for ( i = 0, ip = 0; i < ib; i += DKS_MR, ip += DKS_PACK_MR ) {

            packu_rhsxmc(
              min( ib - i, DKS_MR ),
              KS_RHS,
              u,
              KS_RHS,
              &umap[ ic + i ],
              &packu[ tid * DKS_PACK_MC * KS_RHS + ip * KS_RHS ]
              );

            for ( ir = 0; ir < min( ib - i, DKS_MR ); ir ++ ) {
			  if ( pack_norm ) {
				packA2[ tid * DKS_PACK_MC + ip + ir ] = XA2[ amap[ ic + i + ir ] ];
			  }
			  if ( pack_bandwidth ) {
				packAh[ tid * DKS_PACK_MC + ip + ir ] = kernel->hi[ amap[ ic + i + ir ] ];
			  }
			}
			packA_kcxmc(
				min( ib - i, DKS_MR ),
				pb,
                XA,
                k,
                &amap[ ic + i ],
                &packA[ tid * DKS_PACK_MC * pb + ip * pb ]
                );
          }

          dgsks_macro_kernel(                      // 1~3 loops
              kernel,
              ib,
              jb,
              pb,
              packu  + tid * DKS_PACK_MC * KS_RHS,
              packA  + tid * DKS_PACK_MC * pb,
              packA2 + tid * DKS_PACK_MC,
              packAh + tid * DKS_PACK_MC,
              packB,
              packB2,
              packBh,
              packw,
              NULL,
              0,
              pc
              );

		  for ( i = 0, ip = 0; i < ib; i += DKS_MR, ip += DKS_PACK_MR ) {
			// unpacku with multiple rhs.
			unpacku_rhsxmc(
				min( ib - i, DKS_MR ),
				KS_RHS,
				u,
				KS_RHS,
				&umap[ ic + i ],
				&packu[ tid * DKS_PACK_MC * KS_RHS + ip * KS_RHS ]
				);
          }
        }
      }
    }

  }


  // -----------------------------------------------------------------
  // Free all packing buffers.
  // -----------------------------------------------------------------
#ifdef GSKS_MIC_AVX512
  hbw_free( packA );
  hbw_free( packB );
  hbw_free( packu );
  hbw_free( packw );
  hbw_free( packA2 );
  hbw_free( packB2 );
#else
  free( packA );
  free( packB );
  free( packu );
  free( packw );
  free( packA2 );
  free( packB2 );
#endif
  // -----------------------------------------------------------------


  // ------------------------------------------------------------------------
  // Free kernel dependent buffers
  // ------------------------------------------------------------------------
  switch ( kernel->type ) {
    case KS_GAUSSIAN:
      break;
    case KS_GAUSSIAN_VAR_BANDWIDTH:
#ifdef GSKS_MIC_AVX512
      hbw_free( packAh );
      hbw_free( packBh );
#else
      free( packAh );
      free( packBh );
#endif
      break;
    case KS_POLYNOMIAL:
      break;
    case KS_LAPLACE:
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
      printf( "Error dgsks(): illegal kernel type\n" );
      exit( 1 );
  }
  // ------------------------------------------------------------------------
}



/*
 *
 */ 
void dgsks_wrapper(
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
    int    *omega,
    int    type,
    double scal,
    double cons, 
    double powe,
    double *h
    )
{
  ks_t   kernel;
  kernel.type = type;
  kernel.scal = scal;
  kernel.powe = powe;
  kernel.hi   = h;
  kernel.hj   = h;
  dgsks(
    &kernel,
    m,
    n,
    k,
    u,
    umap,
    XA,
    XA2,
    alpha,
    XB,
    XB2,
    beta,
    w,
    omega
    );
}
