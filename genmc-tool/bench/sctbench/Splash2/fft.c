

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#define PAGE_SIZE               4096
#define NUM_CACHE_LINES        65536 
#define LOG2_LINE_SIZE             4
#define PI                         3.1416
#define DEFAULT_M                 10
#define DEFAULT_P                  1


#include <pthread.h>
#include <sched.h>

#include "../../utils/random.h"
//#include "libittnotify.h"
#ifndef BARRIER_T
#define BARRIER_T
typedef struct {
    volatile int gsense;
    volatile long count;    /* the count of entered processors */
} barrier_t;
#endif


int numFinished = 0;

#define SWAP(a,b) {double tmp; tmp=a; a=b; b=tmp;}

struct GlobalMemory {
    int id;
    pthread_mutex_t(idlock);
    barrier_t(start);
    int* transtimes;
    int* totaltimes;
    int starttime;
    int finishtime;
    int initdonetime;
} *Global;


int P = DEFAULT_P;
int M = DEFAULT_M;
int N;                  /* N = 2^M                                */
int rootN;              /* rootN = N^1/2                          */
double* x;              /* x is the original time-domain data     */
double* trans;          /* trans is used as scratch space         */
double* umain;          /* umain is roots of unity for 1D FFTs    */
double* umain2;         /* umain2 is entire roots of unity matrix */
int test_result = 0;
int doprint = 0;
int dostats = 0;
int transtime = 0;
int transtime2 = 0;
int avgtranstime = 0;
int avgcomptime = 0;
unsigned int transstart = 0;
unsigned int transend = 0;
int maxtotal = 0;
int mintotal = 0;
double maxfrac = 0;
double minfrac = 0;
double avgfractime = 0;
int orig_num_lines = NUM_CACHE_LINES;     /* number of cache lines */
int num_cache_lines = NUM_CACHE_LINES;    /* number of cache lines */
int log2_line_size = LOG2_LINE_SIZE;
int line_size;
int rowsperproc;
double ck1;
double ck3;                        /* checksums for testing answer */
int pad_length;

void InitX(int N, double* x);
void InitU(int N, double* u);
void InitU2(int N, double* u, int n1);
void TwiddleOneCol(int direction, int n1, int N, int j, double* u, double* x, int pad_length);
void Scale(int n1, int N, double* x);
void Transpose(int n1, double* src, double* dest, int MyNum, int MyFirst, int MyLast, int pad_length);
void CopyColumn(int n1, double* src, double* dest);
void FFT1DOnce(int direction, int M, int N, double* u, double* x);
// void SlaveStart();
void* SlaveStart(void*);
void PrintArray(int N, double* x);
double TouchArray(double*, double*, double*, double*, int, int, int, int);
void FFT1D(int, int, int, double*, double*, double*, double*, int, int*, int,
    int, int, int, int, int, int, struct GlobalMemory*);
double CheckSum();
double drand48();
int log_2(int);
void printerr(char*);

int main(int argc, char** argv) {
    int i;
    int j;
    int c;
    extern char* optarg;
    int m1;
    int factor;
    int pages;
    unsigned int start;

    { long time(); (start) = time(0); };
    // // -p2 -m2 -n4 -l4
    P = 2;
    M = 2;
    if (P < 1) {
        printerr("P must be >= 1\n");
        exit(-1);
    }
    if (log_2(P) == -1) {
        printerr("P must be a power of 2\n");
        exit(-1);
    }
    m1 = M / 2;
    if (2 * m1 != M) {
        printerr("M must be even\n");
        exit(-1);
    }
    num_cache_lines = 4;    // n
    orig_num_lines = num_cache_lines;
    if (num_cache_lines < 1) {
        printerr("Number of cache lines must be >= 1\n");
        exit(-1);
    }
    log2_line_size = 4; // l
    if (log2_line_size < 0) {
        printerr("Log base 2 of cache line length in bytes must be >= 0\n");
        exit(-1);
    }





    N = 1 << M;
    rootN = 1 << (M / 2);
    rowsperproc = rootN / P;
    if (rowsperproc == 0) {
        printerr("Matrix not large enough. 2**(M/2) must be >= P\n");
        exit(-1);
    }

    line_size = 1 << log2_line_size;
    if (line_size < 2 * sizeof(double)) {
        printf("WARNING: Each element is a complex double (%d bytes)\n", 2 * sizeof(double));
        printf("  => Less than one element per cache line\n");
        printf("     Computing transpose blocking factor\n");
        factor = (2 * sizeof(double)) / line_size;
        num_cache_lines = orig_num_lines / factor;
    }
    if (line_size <= 2 * sizeof(double)) {
        pad_length = 1;
    }
    else {
        pad_length = line_size / (2 * sizeof(double));
    }

    if (rowsperproc * rootN * 2 * sizeof(double) >= PAGE_SIZE) {
        pages = (2 * pad_length * sizeof(double) * rowsperproc) / PAGE_SIZE;
        if (pages * PAGE_SIZE != 2 * pad_length * sizeof(double) * rowsperproc) {
            pages++;
        }
        pad_length = (pages * PAGE_SIZE) / (2 * sizeof(double) * rowsperproc);
    }
    else {
        pad_length = (PAGE_SIZE - (rowsperproc * rootN * 2 * sizeof(double))) /

            (2 * sizeof(double) * rowsperproc);
        if (pad_length * (2 * sizeof(double) * rowsperproc) !=
            (PAGE_SIZE - (rowsperproc * rootN * 2 * sizeof(double)))) {
            printerr("Padding algorithm unsuccessful\n");
            exit(-1);
        }
    }

    Global = (struct GlobalMemory*)malloc(sizeof(struct GlobalMemory));;
    x = (double*)malloc(2 * (N + rootN * pad_length) * sizeof(double) + PAGE_SIZE);;
    trans = (double*)malloc(2 * (N + rootN * pad_length) * sizeof(double) + PAGE_SIZE);;
    umain = (double*)malloc(2 * rootN * sizeof(double));;
    umain2 = (double*)malloc(2 * (N + rootN * pad_length) * sizeof(double) + PAGE_SIZE);;

    Global->transtimes = (int*)malloc(P * sizeof(int));;
    Global->totaltimes = (int*)malloc(P * sizeof(int));;
    if (Global == NULL) {
        printerr("Could not malloc memory for Global\n");
        exit(-1);
    }
    else if (x == NULL) {
        printerr("Could not malloc memory for x\n");
        exit(-1);
    }
    else if (trans == NULL) {
        printerr("Could not malloc memory for trans\n");
        exit(-1);
    }
    else if (umain == NULL) {
        printerr("Could not malloc memory for umain\n");
        exit(-1);
    }
    else if (umain2 == NULL) {
        printerr("Could not malloc memory for umain2\n");
        exit(-1);
    }

    x = (double*)(((unsigned)x) + PAGE_SIZE - ((unsigned)x) % PAGE_SIZE);
    trans = (double*)(((unsigned)trans) + PAGE_SIZE - ((unsigned)trans) % PAGE_SIZE);
    umain2 = (double*)(((unsigned)umain2) + PAGE_SIZE - ((unsigned)umain2) % PAGE_SIZE);

    /* In order to optimize data distribution, the data structures x, trans,
       and umain2 have been aligned so that each begins on a page boundary.
       This ensures that the amount of padding calculated by the program is
       such that each processor's partition ends on a page boundary, thus
       ensuring that all data from these structures that are needed by a
       processor can be allocated to its local memory */

       /* POSSIBLE ENHANCEMENT:  Here is where one might distribute the x,
          trans, and umain2 data structures across physically distributed
          memories as desired.

          One way to place data is as follows:

          double *base;
          int i;

          i = ((N/P)+(rootN/P)*pad_length)*2;
          base = &(x[0]);
          for (j=0;j<P;j++) {
           Place all addresses x such that (base <= x < base+i) on node j
           base += i;
          }

          The trans and umain2 data structures can be placed in a similar manner.

          */

    printf("\n");
    printf("FFT with Blocking Transpose\n");
    printf("   %d Complex Doubles\n", N);
    printf("   %d Processors\n", P);
    if (num_cache_lines != orig_num_lines) {
        printf("   %d Cache lines\n", orig_num_lines);
        printf("   %d Cache lines for blocking transpose\n", num_cache_lines);
    }
    else {
        printf("   %d Cache lines\n", num_cache_lines);
    }
    printf("   %d Byte line size\n", (1 << log2_line_size));
    printf("   %d Bytes per page\n", PAGE_SIZE);
    printf("\n");

    {
        (Global->start).gsense = 1;
        (Global->start).count = 0;
    };
    { pthread_mutex_init(&(Global->idlock), NULL); };
    Global->id = 0;
    InitX(N, x);                  /* place random values in x */

    if (test_result) {
        ck1 = CheckSum(N, x);
    }
    if (doprint) {
        printf("Original data values:\n");
        PrintArray(N, x);
    }

    InitU(N, umain);               /* initialize u arrays*/
    InitU2(N, umain2, rootN);

    /* fire off P processes */
    for (i = 1; i < P; i++) {
        {
            pthread_t t;
            pthread_create(&t, NULL, (SlaveStart), NULL);
        };
    }
    SlaveStart(NULL);

    { ; }
    assert(numFinished == P);

    if (doprint) {
        if (test_result) {
            printf("Data values after inverse FFT:\n");
        }
        else {
            printf("Data values after FFT:\n");
        }
        PrintArray(N, x);
    }

    transtime = Global->transtimes[0];
    printf("\n");
    printf("                 PROCESS STATISTICS\n");
    printf("            Computation      Transpose     Transpose\n");
    printf(" Proc          Time            Time        Fraction\n");
    printf("    0        %10d     %10d      %8.5f\n",
        Global->totaltimes[0], Global->transtimes[0],
        ((double)Global->transtimes[0]) / Global->totaltimes[0]);
    if (dostats) {
        transtime2 = Global->transtimes[0];
        avgtranstime = Global->transtimes[0];
        avgcomptime = Global->totaltimes[0];
        maxtotal = Global->totaltimes[0];
        mintotal = Global->totaltimes[0];
        maxfrac = ((double)Global->transtimes[0]) / Global->totaltimes[0];
        minfrac = ((double)Global->transtimes[0]) / Global->totaltimes[0];
        avgfractime = ((double)Global->transtimes[0]) / Global->totaltimes[0];
        for (i = 1; i < P; i++) {
            if (Global->transtimes[i] > transtime) {
                transtime = Global->transtimes[i];
            }
            if (Global->transtimes[i] < transtime2) {
                transtime2 = Global->transtimes[i];
            }
            if (Global->totaltimes[i] > maxtotal) {
                maxtotal = Global->totaltimes[i];
            }
            if (Global->totaltimes[i] < mintotal) {
                mintotal = Global->totaltimes[i];
            }
            if (((double)Global->transtimes[i]) / Global->totaltimes[i] > maxfrac) {
                maxfrac = ((double)Global->transtimes[i]) / Global->totaltimes[i];
            }
            if (((double)Global->transtimes[i]) / Global->totaltimes[i] < minfrac) {
                minfrac = ((double)Global->transtimes[i]) / Global->totaltimes[i];
            }
            printf("  %3d        %10d     %10d      %8.5f\n",
                i, Global->totaltimes[i], Global->transtimes[i],
                ((double)Global->transtimes[i]) / Global->totaltimes[i]);
            avgtranstime += Global->transtimes[i];
            avgcomptime += Global->totaltimes[i];
            avgfractime += ((double)Global->transtimes[i]) / Global->totaltimes[i];
        }
        printf("  Avg        %10.0f     %10.0f      %8.5f\n",
            ((double)avgcomptime) / P, ((double)avgtranstime) / P, avgfractime / P);
        printf("  Max        %10d     %10d      %8.5f\n",
            maxtotal, transtime, maxfrac);
        printf("  Min        %10d     %10d      %8.5f\n",
            mintotal, transtime2, minfrac);
    }
    Global->starttime = start;
    printf("\n");
    printf("                 TIMING INFORMATION\n");
    printf("Start time                        : %16d\n",
        Global->starttime);
    printf("Initialization finish time        : %16d\n",
        Global->initdonetime);
    printf("Overall finish time               : %16d\n",
        Global->finishtime);
    printf("Total time with initialization    : %16d\n",
        Global->finishtime - Global->starttime);
    printf("Total time without initialization : %16d\n",
        Global->finishtime - Global->initdonetime);
    printf("Overall transpose time            : %16d\n",
        transtime);
    printf("Overall transpose fraction        : %16.5f\n",
        ((double)transtime) / (Global->finishtime - Global->initdonetime));
    printf("\n");

    if (test_result) {
        ck3 = CheckSum(N, x);
        printf("              INVERSE FFT TEST RESULTS\n");
        printf("Checksum difference is %.3f (%.3f, %.3f)\n",
            ck1 - ck3, ck1, ck3);
        double myfabs(double);
        if (myfabs(ck1 - ck3) < 0.001) {
            printf("TEST PASSED\n");
        }
        else {
            printf("TEST FAILED\n");
        }
    }

    { exit(0); };
}

double my_fabs(double x) {
    return (x < 0) ? -x : x;
}


void* SlaveStart(void*) {
    int i;
    int j;
    int MyNum;
    double error;
    double* upriv;
    int initdone;
    int finish;
    int l_transtime = 0;
    int MyFirst;
    int MyLast;

    { pthread_mutex_lock(&(Global->idlock)); };
    MyNum = Global->id;
    Global->id++;
    { pthread_mutex_unlock(&(Global->idlock)); };

    /* POSSIBLE ENHANCEMENT:  Here is where one might pin processes to
       processors to avoid migration */

    {
#if 0
        asm("sfence");
#endif
        int lsense = (Global->start).gsense;
        while (1) {
            int oldcount = (Global->start).count;
            int newcount = oldcount + lsense;
            int updatedcount;
            asm("lock cmpxchg %1,%2" :
            "=a" (updatedcount) :
                "r" (newcount), "m" ((Global->start).count), "0a" (oldcount));
            if (updatedcount == oldcount) {
                if ((newcount == (P) && lsense == 1) || (newcount == 0 && lsense == -1))
                    (Global->start).gsense = -lsense;
                break;
            }
            sched_yield();
        }
        while ((Global->start).gsense == lsense)
        {
            sched_yield();
        }
#if 0
        asm("lfence");
#endif
    }
    ;

    upriv = (double*)malloc(2 * (rootN - 1) * sizeof(double));
    if (upriv == NULL) {
        fprintf(stderr, "Proc %d could not malloc memory for upriv\n", MyNum);
        exit(-1);
    }
    for (i = 0; i < 2 * (rootN - 1); i++) {
        upriv[i] = umain[i];
    }

    MyFirst = rootN * MyNum / P;
    MyLast = rootN * (MyNum + 1) / P;

    TouchArray(x, trans, umain2, upriv, N, MyNum, MyFirst, MyLast);

    {
#if 0
        asm("sfence");
#endif
        int lsense = (Global->start).gsense;
        while (1) {
            int oldcount = (Global->start).count;
            int newcount = oldcount + lsense;
            int updatedcount;
            asm("lock cmpxchg %1,%2" :
            "=a" (updatedcount) :
                "r" (newcount), "m" ((Global->start).count), "0a" (oldcount));
            if (updatedcount == oldcount) {
                if ((newcount == (P) && lsense == 1) || (newcount == 0 && lsense == -1))
                    (Global->start).gsense = -lsense;
                break;
            }
            sched_yield();
        }
        while ((Global->start).gsense == lsense)
        {
            sched_yield();
        }
#if 0
        asm("lfence");
#endif
    }
    ;

    /* POSSIBLE ENHANCEMENT:  Here is where one might reset the
       statistics that one is measuring about the parallel execution */

    if ((MyNum == 0) || (dostats)) {
        { long time(); (initdone) = time(0); };
    }

    /* perform forward FFT */
    FFT1D(1, M, N, x, trans, upriv, umain2, MyNum, &l_transtime, MyFirst,
        MyLast, pad_length, P, test_result, doprint, dostats, Global);

    /* perform backward FFT */
    if (test_result) {
        FFT1D(-1, M, N, x, trans, upriv, umain2, MyNum, &l_transtime, MyFirst,
            MyLast, pad_length, P, test_result, doprint, dostats, Global);
    }

    if ((MyNum == 0) || (dostats)) {
        { long time(); (finish) = time(0); };
        Global->transtimes[MyNum] = l_transtime;
        Global->totaltimes[MyNum] = finish - initdone;
    }
    if (MyNum == 0) {
        Global->finishtime = finish;
        Global->initdonetime = initdone;
    }
    numFinished++;
}


double TouchArray(x, scratch, u, upriv, N, MyNum, MyFirst, MyLast)

double* x;
double* scratch;
double* u;
double* upriv;
int N;
int MyNum;
int MyFirst;
int MyLast;

{
    int i, j, k;
    double tot = 0.0;

    /* touch my data */
    for (j = 0; j < 2 * (rootN - 1); j++) {
        tot += upriv[j];
    }
    for (j = MyFirst; j < MyLast; j++) {
        k = j * (rootN + pad_length);
        for (i = 0; i < rootN; i++) {
            tot += x[2 * (k + i)] + x[2 * (k + i) + 1] +
                scratch[2 * (k + i)] + scratch[2 * (k + i) + 1] +
                u[2 * (k + i)] + u[2 * (k + i) + 1];
        }
    }
    return tot;
}


double CheckSum(N, x)

int N;
double* x;

{
    int i, j, k;
    double cks;

    cks = 0.0;
    for (j = 0; j < rootN; j++) {
        k = j * (rootN + pad_length);
        for (i = 0; i < rootN; i++) {
            cks += x[2 * (k + i)] + x[2 * (k + i) + 1];
        }
    }

    return(cks);
}


void InitX(int N, double* x) {
    int i, j, k;

    srand48(0);
    for (j = 0; j < rootN; j++) {
        k = j * (rootN + pad_length);
        for (i = 0; i < rootN; i++) {
            x[2 * (k + i)] = drand48();
            x[2 * (k + i) + 1] = drand48();
        }
    }
}

void InitU(int N, double* u) {
    int q;
    int j;
    int base;
    int n1;

    for (q = 0; 1 << q < N; q++) {
        n1 = 1 << q;
        base = n1 - 1;
        for (j = 0; j < n1; j++) {
            if (base + j > rootN - 1) {
                return;
            }
            u[2 * (base + j)] = cos(2.0 * PI * j / (2 * n1));
            u[2 * (base + j) + 1] = -sin(2.0 * PI * j / (2 * n1));
        }
    }
}

void InitU2(int N, double* u, int n1) {
    int i, j, k;
    int base;

    for (j = 0; j < n1; j++) {
        k = j * (rootN + pad_length);
        for (i = 0; i < n1; i++) {
            u[2 * (k + i)] = cos(2.0 * PI * i * j / (N));
            u[2 * (k + i) + 1] = -sin(2.0 * PI * i * j / (N));
        }
    }
}

int BitReverse(int M, int k) {
    int i;
    int j;
    int tmp;

    j = 0;
    tmp = k;
    for (i = 0; i < M; i++) {
        j = 2 * j + (tmp & 0x1);
        tmp = tmp >> 1;
    }
    return(j);
}


void FFT1D(int direction, int M, int N, double* x, double* scratch, double* upriv, double* umain2, int MyNum, int* l_transtime,
    int MyFirst, int MyLast, int pad_length, int P, int test_result, int doprint, int dostats,
    struct GlobalMemory* Global)
{
    int i;
    int j;
    int m1;
    int n1;
    int flag = 0;
    unsigned int clocktime1;
    unsigned int clocktime2;

    m1 = M / 2;
    n1 = 1 << m1;

    {
#if 0
        asm("sfence");
#endif
        int lsense = (Global->start).gsense;
        while (1) {
            int oldcount = (Global->start).count;
            int newcount = oldcount + lsense;
            int updatedcount;
            asm("lock cmpxchg %1,%2" :
            "=a" (updatedcount) :
                "r" (newcount), "m" ((Global->start).count), "0a" (oldcount));
            if (updatedcount == oldcount) {
                if ((newcount == (P) && lsense == 1) || (newcount == 0 && lsense == -1))
                    (Global->start).gsense = -lsense;
                break;
            }
            sched_yield();
        }
        while ((Global->start).gsense == lsense)
        {
            sched_yield();
        }
#if 0
        asm("lfence");
#endif
    }
    ;

    if ((MyNum == 0) || (dostats)) {
        { long time(); (clocktime1) = time(0); };
    }

    /* transpose from x into scratch */
    Transpose(n1, x, scratch, MyNum, MyFirst, MyLast, pad_length);

    if ((MyNum == 0) || (dostats)) {
        { long time(); (clocktime2) = time(0); };
        *l_transtime += (clocktime2 - clocktime1);
    }

    /* do n1 1D FFTs on columns */
    for (j = MyFirst; j < MyLast; j++) {
        FFT1DOnce(direction, m1, n1, upriv, &scratch[2 * j * (n1 + pad_length)]);
        TwiddleOneCol(direction, n1, N, j, umain2, &scratch[2 * j * (n1 + pad_length)],
            pad_length);
    }

    {
#if 0
        asm("sfence");
#endif
        int lsense = (Global->start).gsense;
        while (1) {
            int oldcount = (Global->start).count;
            int newcount = oldcount + lsense;
            int updatedcount;
            asm("lock cmpxchg %1,%2" :
            "=a" (updatedcount) :
                "r" (newcount), "m" ((Global->start).count), "0a" (oldcount));
            if (updatedcount == oldcount) {
                if ((newcount == (P) && lsense == 1) || (newcount == 0 && lsense == -1))
                    (Global->start).gsense = -lsense;
                break;
            }
            sched_yield();
        }
        while ((Global->start).gsense == lsense)
        {
            sched_yield();
        }
#if 0
        asm("lfence");
#endif
    }
    ;

    if ((MyNum == 0) || (dostats)) {
        { long time(); (clocktime1) = time(0); };
    }
    /* transpose */
    Transpose(n1, scratch, x, MyNum, MyFirst, MyLast, pad_length);

    if ((MyNum == 0) || (dostats)) {
        { long time(); (clocktime2) = time(0); };
        *l_transtime += (clocktime2 - clocktime1);
    }

    /* do n1 1D FFTs on columns again */
    for (j = MyFirst; j < MyLast; j++) {
        FFT1DOnce(direction, m1, n1, upriv, &x[2 * j * (n1 + pad_length)]);
        if (direction == -1)
            Scale(n1, N, &x[2 * j * (n1 + pad_length)]);
    }

    {
#if 0
        asm("sfence");
#endif
        int lsense = (Global->start).gsense;
        while (1) {
            int oldcount = (Global->start).count;
            int newcount = oldcount + lsense;
            int updatedcount;
            asm("lock cmpxchg %1,%2" :
            "=a" (updatedcount) :
                "r" (newcount), "m" ((Global->start).count), "0a" (oldcount));
            if (updatedcount == oldcount) {
                if ((newcount == (P) && lsense == 1) || (newcount == 0 && lsense == -1))
                    (Global->start).gsense = -lsense;
                break;
            }
            sched_yield();
        }
        while ((Global->start).gsense == lsense)
        {
            sched_yield();
        }
#if 0
        asm("lfence");
#endif
    }
    ;

    if ((MyNum == 0) || (dostats)) {
        { long time(); (clocktime1) = time(0); };
    }

    /* transpose back */
    Transpose(n1, x, scratch, MyNum, MyFirst, MyLast, pad_length);

    if ((MyNum == 0) || (dostats)) {
        { long time(); (clocktime2) = time(0); };
        *l_transtime += (clocktime2 - clocktime1);
    }

    {
#if 0
        asm("sfence");
#endif
        int lsense = (Global->start).gsense;
        while (1) {
            int oldcount = (Global->start).count;
            int newcount = oldcount + lsense;
            int updatedcount;
            asm("lock cmpxchg %1,%2" :
            "=a" (updatedcount) :
                "r" (newcount), "m" ((Global->start).count), "0a" (oldcount));
            if (updatedcount == oldcount) {
                if ((newcount == (P) && lsense == 1) || (newcount == 0 && lsense == -1))
                    (Global->start).gsense = -lsense;
                break;
            }
            sched_yield();
        }
        while ((Global->start).gsense == lsense)
        {
            sched_yield();
        }
#if 0
        asm("lfence");
#endif
    }
    ;

    /* copy columns from scratch to x */
    if ((test_result) || (doprint)) {
        for (j = MyFirst; j < MyLast; j++) {
            CopyColumn(n1, &scratch[2 * j * (n1 + pad_length)], &x[2 * j * (n1 + pad_length)]);
        }
    }

    {
#if 0
        asm("sfence");
#endif
        int lsense = (Global->start).gsense;
        while (1) {
            int oldcount = (Global->start).count;
            int newcount = oldcount + lsense;
            int updatedcount;
            asm("lock cmpxchg %1,%2" :
            "=a" (updatedcount) :
                "r" (newcount), "m" ((Global->start).count), "0a" (oldcount));
            if (updatedcount == oldcount) {
                if ((newcount == (P) && lsense == 1) || (newcount == 0 && lsense == -1))
                    (Global->start).gsense = -lsense;
                break;
            }
            sched_yield();
        }
        while ((Global->start).gsense == lsense)
        {
            sched_yield();
        }
#if 0
        asm("lfence");
#endif
    }
    ;
}

void TwiddleOneCol(int direction, int n1, int N, int j, double* u, double* x, int pad_length) {
    int i;
    double omega_r;
    double omega_c;
    double x_r;
    double x_c;
    double r1;
    double c1;
    double r2;
    double c2;

    for (i = 0; i < n1; i++) {
        omega_r = u[2 * (j * (n1 + pad_length) + i)];
        omega_c = direction * u[2 * (j * (n1 + pad_length) + i) + 1];
        x_r = x[2 * i];
        x_c = x[2 * i + 1];
        x[2 * i] = omega_r * x_r - omega_c * x_c;
        x[2 * i + 1] = omega_r * x_c + omega_c * x_r;
    }
}

void Scale(int n1, int N, double* x)

{
    int i;

    for (i = 0; i < n1; i++) {
        x[2 * i] /= N;
        x[2 * i + 1] /= N;
    }
}

void Transpose(int n1, double* src, double* dest, int MyNum, int MyFirst, int MyLast, int pad_length) {
    int i;
    int j;
    int k;
    int l;
    int m;
    int blksize;
    int numblks;
    int firstfirst;
    int h_off;
    int v_off;
    int v;
    int h;
    int n1p;
    int row_count;

    blksize = MyLast - MyFirst;
    numblks = (2 * blksize) / num_cache_lines;
    if (numblks * num_cache_lines != 2 * blksize) {
        numblks++;
    }
    blksize = blksize / numblks;
    firstfirst = MyFirst;
    row_count = n1 / P;
    n1p = n1 + pad_length;
    for (l = MyNum + 1; l < P; l++) {
        v_off = l * row_count;
        for (k = 0; k < numblks; k++) {
            h_off = firstfirst;
            for (m = 0; m < numblks; m++) {
                for (i = 0; i < blksize; i++) {
                    v = v_off + i;
                    for (j = 0; j < blksize; j++) {
                        h = h_off + j;
                        dest[2 * (h * n1p + v)] = src[2 * (v * n1p + h)];
                        dest[2 * (h * n1p + v) + 1] = src[2 * (v * n1p + h) + 1];
                    }
                }
                h_off += blksize;
            }
            v_off += blksize;
        }
    }

    for (l = 0; l < MyNum; l++) {
        v_off = l * row_count;
        for (k = 0; k < numblks; k++) {
            h_off = firstfirst;
            for (m = 0; m < numblks; m++) {
                for (i = 0; i < blksize; i++) {
                    v = v_off + i;
                    for (j = 0; j < blksize; j++) {
                        h = h_off + j;
                        dest[2 * (h * n1p + v)] = src[2 * (v * n1p + h)];
                        dest[2 * (h * n1p + v) + 1] = src[2 * (v * n1p + h) + 1];
                    }
                }
                h_off += blksize;
            }
            v_off += blksize;
        }
    }

    v_off = MyNum * row_count;
    for (k = 0; k < numblks; k++) {
        h_off = firstfirst;
        for (m = 0; m < numblks; m++) {
            for (i = 0; i < blksize; i++) {
                v = v_off + i;
                for (j = 0; j < blksize; j++) {
                    h = h_off + j;
                    dest[2 * (h * n1p + v)] = src[2 * (v * n1p + h)];
                    dest[2 * (h * n1p + v) + 1] = src[2 * (v * n1p + h) + 1];
                }
            }
            h_off += blksize;
        }
        v_off += blksize;
    }
}

void CopyColumn(int n1, double* src, double* dest) {
    int i;

    for (i = 0; i < n1; i++) {
        dest[2 * i] = src[2 * i];
        dest[2 * i + 1] = src[2 * i + 1];
    }
}

void Reverse(int N, int M, double* x) {
    int j, k;

    for (k = 0; k < N; k++) {
        j = BitReverse(M, k);
        if (j > k) {
            SWAP(x[2 * j], x[2 * k]);
            SWAP(x[2 * j + 1], x[2 * k + 1]);
        }
    }
}

void FFT1DOnce(int direction, int M, int N, double* u, double* x) {
    int j;
    int k;
    int q;
    int L;
    int r;
    int Lstar;
    double* u1;
    double* x1;
    double* x2;
    double omega_r;
    double omega_c;
    double tau_r;
    double tau_c;
    double x_r;
    double x_c;

    Reverse(N, M, x);

    for (q = 1; q <= M; q++) {
        L = 1 << q; r = N / L; Lstar = L / 2;
        u1 = &u[2 * (Lstar - 1)];
        for (k = 0; k < r; k++) {
            x1 = &x[2 * (k * L)];
            x2 = &x[2 * (k * L + Lstar)];
            for (j = 0; j < Lstar; j++) {
                omega_r = u1[2 * j];
                omega_c = direction * u1[2 * j + 1];
                x_r = x2[2 * j];
                x_c = x2[2 * j + 1];
                tau_r = omega_r * x_r - omega_c * x_c;
                tau_c = omega_r * x_c + omega_c * x_r;
                x_r = x1[2 * j];
                x_c = x1[2 * j + 1];
                x2[2 * j] = x_r - tau_r;
                x2[2 * j + 1] = x_c - tau_c;
                x1[2 * j] = x_r + tau_r;
                x1[2 * j + 1] = x_c + tau_c;
            }
        }
    }
}

void PrintArray(int N, double* x) {
    int i, j, k;

    for (i = 0; i < rootN; i++) {
        k = i * (rootN + pad_length);
        for (j = 0; j < rootN; j++) {
            printf(" %4.2f %4.2f", x[2 * (k + j)], x[2 * (k + j) + 1]);
            if (i * rootN + j != N - 1) {
                printf(",");
            }
            if ((i * rootN + j + 1) % 8 == 0) {
                printf("\n");
            }
        }
    }
    printf("\n");
    printf("\n");
}


void printerr(s)

char* s;

{
    fprintf(stderr, "ERROR: %s\n", s);
}


int log_2(number)

int number;

{
    int cumulative = 1;
    int out = 0;
    int done = 0;

    while ((cumulative < number) && (!done) && (out < 50)) {
        if (cumulative == number) {
            done = 1;
        }
        else {
            cumulative = cumulative * 2;
            out++;
        }
    }

    if (cumulative == number) {
        return(out);
    }
    else {
        return(-1);
    }
}

