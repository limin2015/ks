#include <stdio.h>
#include <stdlib.h>
#include <omp.h>
#include <ks.h>

double *ks_malloc_aligned(
    int    m,
    int    n,
    int    size
    )
{
  double *ptr;
  int    err;

  err = posix_memalign( (void**)&ptr, (size_t)DKS_SIMD_ALIGN_SIZE, size * m * n );

  if ( err ) {
    printf( "ks_malloc_aligned(): posix_memalign() failures" );
    exit( 1 );
  }

  return ptr;
}
