
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

/*! \file histo_shmem.cpp
 * \brief An implementation of histogram that uses OpenSHMEM global atomics.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <shmem.h>
#include "myshmem.h"

/*!
 * \brief This routine implements straight forward,
 *         single word atomic updates to implement histogram.
 * \param *index array of indices into the shared array of counts.
 * \param l_num_ups the local length of the index array
 * \param *counts SHARED pointer to the count array.
 * \return average run time
 *
 */
double histo_shmem(int64_t *pckindx, int64_t T, int64_t *counts) {
  int ret;
  int64_t i;
  double tm;
  int64_t pe, col;
  double stat;

  shmem_barrier_all();
  tm = wall_seconds();
  i = 0UL;
  for(i = 0; i < T; i++) {
    col = pckindx[i] >> 16;
    pe  = pckindx[i] & 0xffff;
    shmem_int64_atomic_add(counts+col, 1, pe);
    //shmem_int64_atomic_inc(counts+col, pe);
  }
  shmem_barrier_all();
  tm = wall_seconds() - tm;
  stat = myshmem_reduce_add(tm)/THREADS;
  return stat;
}

int main(int argc, char * argv[]) {
  shmem_init();

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
  int64_t num_counts = lnum_counts*THREADS;
  int64_t * counts = (int64_t *)shmem_malloc(lnum_counts* sizeof(int64_t)); assert(counts != NULL);
  int64_t *lcounts = counts;
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

  shmem_barrier_all();

  int64_t use_model;
  double laptime = 0.0;
  int64_t num_models = 0L;               // number of models that are executed

      laptime = histo_shmem(pckindx, l_num_ups, counts);
      num_models++;

      T0_fprintf(stderr,"  %8.3lf seconds\n", laptime);

  shmem_barrier_all();

  // Check the results
  // Assume that the atomic add version will correctly zero out the counts array
  for(i = 0; i < l_num_ups; i++) {
    lindx = index[i] / THREADS;
    pe  = index[i] % THREADS;
    shmem_int64_atomic_add(counts+lindx, -num_models, pe); 
  }
  shmem_barrier_all();

  int64_t num_errors = 0, totalerrors = 0;
  for(i = 0; i < lnum_counts; i++) {
    if(lcounts[i] != 0L) {
      num_errors++;
      if(num_errors < 5)  // print first five errors, report number of errors below
        fprintf(stderr,"ERROR: Thread %d error at %ld (= %ld)\n", MYTHREAD, i, lcounts[i]);
    }
  }
  totalerrors = (double)myshmem_reduce_add((double)num_errors);
  if(totalerrors) {
     T0_fprintf(stderr,"FAILED!!!! total errors = %ld\n", totalerrors);
  }

  shmem_free(counts);
  free(index);
  free(pckindx);
  shmem_finalize();

  return 0;
}

