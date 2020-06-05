
/******************************************************************
//
//
//  Copyright(C) 2019, Institute for Defense Analyses
//  4850 Mark Center Drive, Alexandria, VA; 703-845-2500
//  This material may be reproduced by or for the US Government
//  pursuant to the copyright license under the clauses at DFARS
//  252.227-7013 and 252.227-7014.
//
//
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//    * Neither the name of the copyright holder nor the
//      names of its contributors may be used to endorse or promote products
//      derived from this software without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
//  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
//  COPYRIGHT HOLDER NOR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
//  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
//  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
//  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
//  OF THE POSSIBILITY OF SUCH DAMAGE.
//
 *****************************************************************/

/*! \file histo_upc.cpp
 * \brief An implementation of histogram that uses UPC global atomics.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
//#include <upc.h>
#include <upc_relaxed.h>
#include <upc_atomic.h>
#include <upc_collective.h>

#include <time.h>
#include <sys/time.h>

#define T0_fprintf if(MYTHREAD==0) fprintf

upc_atomicdomain_t * lgp_atomic_domain;

double wall_seconds() {
  struct timeval tp;
  int retVal = gettimeofday(&tp,NULL);
  if (retVal == -1) { perror("gettimeofday:"); fflush(stderr); }
  return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

double reduce_add(double myval){
  static shared double *dst=NULL, * src;
  if (dst==NULL) {
      dst = upc_all_alloc(THREADS, sizeof(double));
      src = upc_all_alloc(THREADS, sizeof(double));
  }
  src[MYTHREAD] = myval;
  upc_barrier;
  upc_all_reduceD(dst, src, UPC_ADD, THREADS, 1, NULL, UPC_IN_NOSYNC | UPC_OUT_NOSYNC);
  upc_barrier;
  return dst[0];
}

/*!
 * \brief This routine implements straight forward,
 *         single word atomic updates to implement histogram.
 * \param *index array of indices into the shared array of counts.
 * \param l_num_ups the local length of the index array
 * \param *counts SHARED pointer to the count array.
 * \return average run time
 *
 */
double histo_upc(int64_t *index, int64_t T, shared int64_t *counts) {
  int ret;
  int64_t i, one=1;
  double tm;
  int64_t pe, col;
  double stat;

  upc_barrier;
  tm = wall_seconds();
  i = 0UL;
  for(i = 0; i < T; i++) {
    //col = pckindx[i] >> 16;
    //pe  = pckindx[i] & 0xffff;
    #pragma pgas defer_sync
    upc_atomic_relaxed(lgp_atomic_domain, NULL, UPC_ADD, &counts[index[i]], &one, NULL);
    //upc_atomic_relaxed(lgp_atomic_domain, NULL, UPC_INC, &counts[index[i]], NULL, NULL);
  }
  upc_barrier;
  tm = wall_seconds() - tm;
  stat = reduce_add(tm)/THREADS;
  return stat;
}

int main(int argc, char * argv[]) {

  char hostname[1024];
  hostname[1023] = '\0';
  gethostname(hostname, 1023);
  fprintf(stderr,"Hostname: %s rank: %d\n", hostname, MYTHREAD);

  int64_t l_num_ups  = 1000000;     // per thread number of requests (updates)
  int64_t lnum_counts = 1000;       // per thread size of the table

  int64_t i;

  int printhelp = 0;
  int opt;
  while( (opt = getopt(argc, argv, "hn:T:")) != -1 ) {
    switch(opt) {
    case 'h': printhelp = 1; break;
    case 'n': sscanf(optarg,"%ld" ,&l_num_ups);  break;
    case 'T': sscanf(optarg,"%ld" ,&lnum_counts);  break;
    default:  break;
    }
  }

  T0_fprintf(stderr,"Running histo on %d PEs\n", THREADS);
  T0_fprintf(stderr,"Number updates / PE              (-n)= %ld\n", l_num_ups);
  T0_fprintf(stderr,"Table size / PE                  (-T)= %ld\n", lnum_counts);
  fflush(stderr);


  // Allocate and zero out the counts array
  lgp_atomic_domain = upc_all_atomicdomain_alloc(UPC_INT64, UPC_ADD, 0);
  int64_t num_counts = lnum_counts*THREADS;
  shared int64_t * counts = upc_all_alloc(num_counts, sizeof(int64_t)); assert(counts != NULL);
  int64_t *lcounts = (int64_t*)(counts+MYTHREAD);
  for(i = 0; i < lnum_counts; i++)
    lcounts[i] = 0L;

  // index is a local array of indices into the shared counts array.
  // This is used by the * version.
  // To avoid paying the UPC tax of computing index[i]/THREADS and index[i]%THREADS
  // when using the exstack and conveyor models
  // we also store a packed version that holds the pe (= index%THREADS) and lindx (=index/THREADS)
  int64_t *index   = (int64_t *) calloc(l_num_ups, sizeof(int64_t)); assert(index != NULL);
  int64_t *pckindx = (int64_t *) calloc(l_num_ups, sizeof(int64_t)); assert(pckindx != NULL);
  int64_t indx, lindx, pe;

  srand(MYTHREAD + 120348);
  for(i = 0; i < l_num_ups; i++) {
    //indx = i % num_counts;          //might want to do this for debugging
    indx = rand() % num_counts;
    index[i] = indx;
    lindx = indx / THREADS;
    pe  = indx % THREADS;
    pckindx[i]  =  (lindx << 16L) | (pe & 0xffff);
  }

  upc_barrier;

  int64_t use_model;
  double laptime = 0.0;
  int64_t num_models = 0L;               // number of models that are executed

      laptime = histo_upc(index, l_num_ups, counts);
      num_models++;

      T0_fprintf(stderr,"  %8.3lf seconds\n", laptime);

  upc_barrier;

  // Check the results
  // Assume that the atomic add version will correctly zero out the counts array
  num_models = -1*num_models;
  for(i = 0; i < l_num_ups; i++) {
    #pragma pgas defer_sync
    upc_atomic_relaxed(lgp_atomic_domain, NULL, UPC_ADD, &counts[index[i]], &num_models, NULL);
  }
  upc_barrier;

  int64_t num_errors = 0, totalerrors = 0;
  for(i = 0; i < lnum_counts; i++) {
    if(lcounts[i] != 0L) {
      num_errors++;
      if(num_errors < 5)  // print first five errors, report number of errors below
        fprintf(stderr,"ERROR: Thread %d error at %ld (= %ld)\n", MYTHREAD, i, lcounts[i]);
    }
  }
  totalerrors = (double)reduce_add((double)num_errors);
  if(totalerrors) {
     T0_fprintf(stderr,"FAILED!!!! total errors = %ld\n", totalerrors);
  }

  upc_all_free(counts);
  free(index);
  free(pckindx);

  return 0;
}

