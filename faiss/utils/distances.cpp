/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include <faiss/utils/distances.h>

#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cmath>

#include <omp.h>

#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissAssert.h>



#ifndef FINTEGER
#define FINTEGER long
#endif


extern "C" {

/* declare BLAS functions, see http://www.netlib.org/clapack/cblas/ */

int sgemm_ (const char *transa, const char *transb, FINTEGER *m, FINTEGER *
            n, FINTEGER *k, const float *alpha, const float *a,
            FINTEGER *lda, const float *b, FINTEGER *
            ldb, float *beta, float *c, FINTEGER *ldc);


}


namespace faiss {



/***************************************************************************
 * Matrix/vector ops
 ***************************************************************************/



/* Compute the inner product between a vector x and
   a set of ny vectors y.
   These functions are not intended to replace BLAS matrix-matrix, as they
   would be significantly less efficient in this case. */
void fvec_inner_products_ny (float * ip,
                             const float * x,
                             const float * y,
                             size_t d, size_t ny)
{
    // Not sure which one is fastest
#if 0
    {
        FINTEGER di = d;
        FINTEGER nyi = ny;
        float one = 1.0, zero = 0.0;
        FINTEGER onei = 1;
        sgemv_ ("T", &di, &nyi, &one, y, &di, x, &onei, &zero, ip, &onei);
    }
#endif
    for (size_t i = 0; i < ny; i++) {
        ip[i] = fvec_inner_product (x, y, d);
        y += d;
    }
}





/* Compute the L2 norm of a set of nx vectors */
void fvec_norms_L2 (float * __restrict nr,
                    const float * __restrict x,
                    size_t d, size_t nx)
{

#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        nr[i] = sqrtf (fvec_norm_L2sqr (x + i * d, d));
    }
}

void fvec_norms_L2sqr (float * __restrict nr,
                       const float * __restrict x,
                       size_t d, size_t nx)
{
#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++)
        nr[i] = fvec_norm_L2sqr (x + i * d, d);
}



void fvec_renorm_L2 (size_t d, size_t nx, float * __restrict x)
{
#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        float * __restrict xi = x + i * d;

        float nr = fvec_norm_L2sqr (xi, d);

        if (nr > 0) {
            size_t j;
            const float inv_nr = 1.0 / sqrtf (nr);
            for (j = 0; j < d; j++)
                xi[j] *= inv_nr;
        }
    }
}












/***************************************************************************
 * KNN functions
 ***************************************************************************/

namespace {


template<class C>
struct HeapResultHandler {

    using T = typename C::T;
    using TI = typename C::TI;

    int nq;
    T *heap_dis_tab;
    TI *heap_ids_tab;

    int64_t k;  // number of results to keep

    HeapResultHandler(
        size_t nq,
        T * heap_dis_tab, TI * heap_ids_tab,
        size_t k):
        nq(nq),
        heap_dis_tab(heap_dis_tab), heap_ids_tab(heap_ids_tab), k(k)
    {

    }

    /******************************************************
     * API for 1 result at a time (each SingleResultHandler is
     * called from 1 thread)
     ******************************************************/

    struct SingleResultHandler {
        HeapResultHandler & hr;
        size_t k;

        T *heap_dis;
        TI *heap_ids;
        T thresh;

        SingleResultHandler(HeapResultHandler &hr): hr(hr), k(hr.k) {}

        /// begin results for query # i
        void begin(size_t i) {
            heap_dis = hr.heap_dis_tab + i * k;
            heap_ids = hr.heap_ids_tab + i * k;
            heap_heapify<C> (k, heap_dis, heap_ids);
            thresh = heap_dis[0];
        }

        /// add one result for query i
        void add_result(T dis, TI idx) {
            if (C::cmp(heap_dis[0], dis)) {
                heap_pop<C>(k, heap_dis, heap_ids);
                heap_push<C>(k, heap_dis, heap_ids, dis, idx);
                thresh = heap_dis[0];
            }
        }

        /// series of results for query i is done
        void end() {
            heap_reorder<C> (k, heap_dis, heap_ids);
        }
    };


    /******************************************************
     * API for multiple results (called from 1 thread)
     ******************************************************/

    size_t i0, i1;

    /// begin
    void begin_multiple(size_t i0, size_t i1) {
        this->i0 = i0;
        this->i1 = i1;
        for(size_t i = i0; i < i1; i++) {
            heap_heapify<C> (k, heap_dis_tab + i * k, heap_ids_tab + i * k);
        }
    }

    /// add results for query i0..i1 and j0..j1
    void add_results(size_t j0, size_t j1, const T *dis_tab) {
        // maybe parallel for
        for (size_t i = i0; i < i1; i++) {
            T * heap_dis = heap_dis_tab + i * k;
            TI * heap_ids = heap_ids_tab + i * k;
            T thresh = heap_dis[0];
            for (size_t j = j0; j < j1; j++) {
                T dis = *dis_tab++;
                if (C::cmp(thresh, dis)) {
                    heap_pop<C>(k, heap_dis, heap_ids);
                    heap_push<C>(k, heap_dis, heap_ids, dis, j);
                    thresh = heap_dis[0];
                }
            }
        }
    }

    /// series of results for queries i0..i1 is done
    void end_multiple() {
        // maybe parallel for
        for(size_t i = i0; i < i1; i++) {
            heap_reorder<C> (k, heap_dis_tab + i * k, heap_ids_tab + i * k);
        }
    }

};


template<class C>
struct RangeSearchResultHandler {
    using T = typename C::T;
    using TI = typename C::TI;

    RangeSearchResult *res;
    float radius;

    RangeSearchResultHandler(RangeSearchResult *res, float radius):
        res(res), radius(radius)
    {}

    /******************************************************
     * API for 1 result at a time (each SingleResultHandler is
     * called from 1 thread)
     ******************************************************/

    struct SingleResultHandler {
        // almost the same interface as RangeSearchResultHandler
        RangeSearchPartialResult pres;
        float radius;
        RangeQueryResult *qr = nullptr;

        SingleResultHandler(RangeSearchResultHandler &rh):
            pres(rh.res), radius(rh.radius)
        {}

        /// begin results for query # i
        void begin(size_t i) {
            qr = &pres.new_result(i);
        }

        /// add one result for query i
        void add_result(T dis, TI idx) {

            if (C::cmp(radius, dis)) {
                qr->add(dis, idx);
            }
        }

        /// series of results for query i is done
        void end() {
        }

        ~SingleResultHandler() {
            pres.finalize();
        }
    };

    /******************************************************
     * API for multiple results (called from 1 thread)
     ******************************************************/

    size_t i0, i1;

    std::vector <RangeSearchPartialResult *> partial_results;
    std::vector <size_t> j0s;
    int pr = 0;

    /// begin
    void begin_multiple(size_t i0, size_t i1) {
        this->i0 = i0;
        this->i1 = i1;
    }

    /// add results for query i0..i1 and j0..j1

    void add_results(size_t j0, size_t j1, const T *dis_tab) {
        RangeSearchPartialResult *pres;
        // there is one RangeSearchPartialResult structure per j0
        // (= block of columns of the large distance matrix)
        // it is a bit tricky to find the poper PartialResult structure
        // because the inner loop is on db not on queries.

        if (pr < j0s.size() && j0 == j0s[pr]) {
            pres = partial_results[pr];
            pr++;
        } else if (j0 == 0 && j0s.size() > 0) {
            pr = 0;
            pres = partial_results[pr];
            pr++;
        } else { // did not find this j0
            pres = new RangeSearchPartialResult (res);
            partial_results.push_back(pres);
            j0s.push_back(j0);
            pr = partial_results.size();
        }

        for (size_t i = i0; i < i1; i++) {
            const float *ip_line = dis_tab + (i - i0) * (j1 - j0);
            RangeQueryResult & qres = pres->new_result (i);

            for (size_t j = j0; j < j1; j++) {
                float dis = *ip_line++;
                if (C::cmp(radius, dis)) {
                    qres.add (dis, j);
                }
            }
        }
    }

    void end_multiple() {

    }

    ~RangeSearchResultHandler() {
        if (partial_results.size() > 0) {
            RangeSearchPartialResult::merge (partial_results);
        }
    }

};





/* Find the nearest neighbors for nx queries in a set of ny vectors */
template<class ResultHandler>
void exhaustive_inner_product_seq (
        const float * x,
        const float * y,
        size_t d, size_t nx, size_t ny,
        ResultHandler &res)
{
    size_t check_period = InterruptCallback::get_period_hint (ny * d);

    check_period *= omp_get_max_threads();

    using SingleResultHandler = typename ResultHandler::SingleResultHandler;

    for (size_t i0 = 0; i0 < nx; i0 += check_period) {
        size_t i1 = std::min(i0 + check_period, nx);

#pragma omp parallel
        {
            SingleResultHandler resi(res);
#pragma omp for
            for (int64_t i = i0; i < i1; i++) {
                const float * x_i = x + i * d;
                const float * y_j = y;

                resi.begin(i);

                for (size_t j = 0; j < ny; j++) {
                    float ip = fvec_inner_product (x_i, y_j, d);
                    resi.add_result(ip, j);
                    y_j += d;
                }
                resi.end();
            }
        }
        InterruptCallback::check ();
    }

}

template<class ResultHandler>
void exhaustive_L2sqr_seq (
                const float * x,
                const float * y,
                size_t d, size_t nx, size_t ny,
                ResultHandler & res)
{

    size_t check_period = InterruptCallback::get_period_hint (ny * d);
    check_period *= omp_get_max_threads();
    using SingleResultHandler = typename ResultHandler::SingleResultHandler;

    for (size_t i0 = 0; i0 < nx; i0 += check_period) {
        size_t i1 = std::min(i0 + check_period, nx);

#pragma omp parallel
        {
            SingleResultHandler resi(res);
#pragma omp for
            for (int64_t i = i0; i < i1; i++) {
                const float * x_i = x + i * d;
                const float * y_j = y;
                resi.begin(i);
                for (size_t j = 0; j < ny; j++) {
                    float disij = fvec_L2sqr (x_i, y_j, d);
                    resi.add_result(disij, j);
                    y_j += d;
                }
                resi.end();
            }
        }
        InterruptCallback::check ();
    }

};





/** Find the nearest neighbors for nx queries in a set of ny vectors */
template<class ResultHandler>
void exhaustive_inner_product_blas (
        const float * x,
        const float * y,
        size_t d, size_t nx, size_t ny,
        ResultHandler & res)
{
    // BLAS does not like empty matrices
    if (nx == 0 || ny == 0) return;

    /* block sizes */
    const size_t bs_x = distance_compute_blas_query_bs;
    const size_t bs_y = distance_compute_blas_database_bs;
    std::unique_ptr<float[]> ip_block(new float[bs_x * bs_y]);

    for (size_t i0 = 0; i0 < nx; i0 += bs_x) {
        size_t i1 = i0 + bs_x;
        if(i1 > nx) i1 = nx;

        res.begin_multiple(i0, i1);

        for (size_t j0 = 0; j0 < ny; j0 += bs_y) {
            size_t j1 = j0 + bs_y;
            if (j1 > ny) j1 = ny;
            /* compute the actual dot products */
            {
                float one = 1, zero = 0;
                FINTEGER nyi = j1 - j0, nxi = i1 - i0, di = d;
                sgemm_ ("Transpose", "Not transpose", &nyi, &nxi, &di, &one,
                        y + j0 * d, &di,
                        x + i0 * d, &di, &zero,
                        ip_block.get(), &nyi);
            }

            res.add_results(j0, j1, ip_block.get());

        }
        res.end_multiple();
        InterruptCallback::check ();

    }
}




// distance correction is an operator that can be applied to transform
// the distances
template<class ResultHandler>
void exhaustive_L2sqr_blas (
        const float * x,
        const float * y,
        size_t d, size_t nx, size_t ny,
        ResultHandler & res,
        const float *y_norms = nullptr)
{
    // BLAS does not like empty matrices
    if (nx == 0 || ny == 0) return;

    /* block sizes */
    const size_t bs_x = distance_compute_blas_query_bs;
    const size_t bs_y = distance_compute_blas_database_bs;
    // const size_t bs_x = 16, bs_y = 16;
    std::unique_ptr<float []> ip_block(new float[bs_x * bs_y]);
    std::unique_ptr<float []> x_norms(new float[nx]);
    std::unique_ptr<float []> del2;

    fvec_norms_L2sqr (x_norms.get(), x, d, nx);

    if (!y_norms) {
        float *y_norms2 = new float[ny];
        del2.reset(y_norms2);
        fvec_norms_L2sqr (y_norms2, y, d, ny);
        y_norms = y_norms2;
    }

    for (size_t i0 = 0; i0 < nx; i0 += bs_x) {
        size_t i1 = i0 + bs_x;
        if(i1 > nx) i1 = nx;

        res.begin_multiple(i0, i1);

        for (size_t j0 = 0; j0 < ny; j0 += bs_y) {
            size_t j1 = j0 + bs_y;
            if (j1 > ny) j1 = ny;
            /* compute the actual dot products */
            {
                float one = 1, zero = 0;
                FINTEGER nyi = j1 - j0, nxi = i1 - i0, di = d;
                sgemm_ ("Transpose", "Not transpose", &nyi, &nxi, &di, &one,
                        y + j0 * d, &di,
                        x + i0 * d, &di, &zero,
                        ip_block.get(), &nyi);
            }

            for (int64_t i = i0; i < i1; i++) {
                float *ip_line = ip_block.get() + (i - i0) * (j1 - j0);

                for (size_t j = j0; j < j1; j++) {
                    float ip = *ip_line;
                    float dis = x_norms[i] + y_norms[j] - 2 * ip;

                    // negative values can occur for identical vectors
                    // due to roundoff errors
                    if (dis < 0) dis = 0;

                    *ip_line = dis;
                    ip_line++;
                }
            }
            res.add_results(j0, j1, ip_block.get());
        }
        res.end_multiple();
        InterruptCallback::check ();
    }
}



} // anonymous namespace





/*******************************************************
 * KNN driver functions
 *******************************************************/

int distance_compute_blas_threshold = 20;
int distance_compute_blas_query_bs = 4096;
int distance_compute_blas_database_bs = 1024;


void knn_inner_product (const float * x,
        const float * y,
        size_t d, size_t nx, size_t ny,
        float_minheap_array_t * ha)
{
    HeapResultHandler<CMin<float, int64_t>> res(
        ha->nh, ha->val, ha->ids, ha->k);
    if (nx < distance_compute_blas_threshold) {
        exhaustive_inner_product_seq (x, y, d, nx, ny, res);
    } else {
        exhaustive_inner_product_blas (x, y, d, nx, ny, res);
    }
}




void knn_L2sqr (
        const float * x,
        const float * y,
        size_t d, size_t nx, size_t ny,
        float_maxheap_array_t * ha,
        const float *y_norm2
) {
    HeapResultHandler<CMax<float, int64_t>> res(
        ha->nh, ha->val, ha->ids, ha->k);

    if (nx < distance_compute_blas_threshold) {
        exhaustive_L2sqr_seq (x, y, d, nx, ny, res);
    } else {
        exhaustive_L2sqr_blas (x, y, d, nx, ny, res, y_norm2);
    }
}


/***************************************************************************
 * Range search
 ***************************************************************************/




void range_search_L2sqr (
        const float * x,
        const float * y,
        size_t d, size_t nx, size_t ny,
        float radius,
        RangeSearchResult *res)
{
    RangeSearchResultHandler<CMax<float, int64_t>> resh(res, radius);
    if (nx < distance_compute_blas_threshold) {
        exhaustive_L2sqr_seq (x, y, d, nx, ny, resh);
    } else {
        exhaustive_L2sqr_blas (x, y, d, nx, ny, resh);
    }
}

void range_search_inner_product (
        const float * x,
        const float * y,
        size_t d, size_t nx, size_t ny,
        float radius,
        RangeSearchResult *res)
{

    RangeSearchResultHandler<CMin<float, int64_t>> resh(res, radius);
    if (nx < distance_compute_blas_threshold) {
        exhaustive_inner_product_seq (x, y, d, nx, ny, resh);
    } else {
        exhaustive_inner_product_blas (x, y, d, nx, ny, resh);
    }
}


/***************************************************************************
 * compute a subset of  distances
 ***************************************************************************/

/* compute the inner product between x and a subset y of ny vectors,
   whose indices are given by idy.  */
void fvec_inner_products_by_idx (float * __restrict ip,
                                 const float * x,
                                 const float * y,
                                 const int64_t * __restrict ids, /* for y vecs */
                                 size_t d, size_t nx, size_t ny)
{
#pragma omp parallel for
    for (int64_t j = 0; j < nx; j++) {
        const int64_t * __restrict idsj = ids + j * ny;
        const float * xj = x + j * d;
        float * __restrict ipj = ip + j * ny;
        for (size_t i = 0; i < ny; i++) {
            if (idsj[i] < 0)
                continue;
            ipj[i] = fvec_inner_product (xj, y + d * idsj[i], d);
        }
    }
}



/* compute the inner product between x and a subset y of ny vectors,
   whose indices are given by idy.  */
void fvec_L2sqr_by_idx (float * __restrict dis,
                        const float * x,
                        const float * y,
                        const int64_t * __restrict ids, /* ids of y vecs */
                        size_t d, size_t nx, size_t ny)
{
#pragma omp parallel for
    for (int64_t j = 0; j < nx; j++) {
        const int64_t * __restrict idsj = ids + j * ny;
        const float * xj = x + j * d;
        float * __restrict disj = dis + j * ny;
        for (size_t i = 0; i < ny; i++) {
            if (idsj[i] < 0)
                continue;
            disj[i] = fvec_L2sqr (xj, y + d * idsj[i], d);
        }
    }
}

void pairwise_indexed_L2sqr (
        size_t d, size_t n,
        const float * x, const int64_t *ix,
        const float * y, const int64_t *iy,
        float *dis)
{
#pragma omp parallel for
    for (int64_t j = 0; j < n; j++) {
        if (ix[j] >= 0 && iy[j] >= 0) {
            dis[j] = fvec_L2sqr (x + d * ix[j], y + d * iy[j], d);
        }
    }
}

void pairwise_indexed_inner_product (
        size_t d, size_t n,
        const float * x, const int64_t *ix,
        const float * y, const int64_t *iy,
        float *dis)
{
#pragma omp parallel for
    for (int64_t j = 0; j < n; j++) {
        if (ix[j] >= 0 && iy[j] >= 0) {
            dis[j] = fvec_inner_product (x + d * ix[j], y + d * iy[j], d);
        }
    }
}


/* Find the nearest neighbors for nx queries in a set of ny vectors
   indexed by ids. May be useful for re-ranking a pre-selected vector list */
void knn_inner_products_by_idx (const float * x,
                                const float * y,
                                const int64_t * ids,
                                size_t d, size_t nx, size_t ny,
                                float_minheap_array_t * res)
{
    size_t k = res->k;

#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        const float * x_ = x + i * d;
        const int64_t * idsi = ids + i * ny;
        size_t j;
        float * __restrict simi = res->get_val(i);
        int64_t * __restrict idxi = res->get_ids (i);
        minheap_heapify (k, simi, idxi);

        for (j = 0; j < ny; j++) {
            if (idsi[j] < 0) break;
            float ip = fvec_inner_product (x_, y + d * idsi[j], d);

            if (ip > simi[0]) {
                minheap_pop (k, simi, idxi);
                minheap_push (k, simi, idxi, ip, idsi[j]);
            }
        }
        minheap_reorder (k, simi, idxi);
    }

}

void knn_L2sqr_by_idx (const float * x,
                       const float * y,
                       const int64_t * __restrict ids,
                       size_t d, size_t nx, size_t ny,
                       float_maxheap_array_t * res)
{
    size_t k = res->k;

#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        const float * x_ = x + i * d;
        const int64_t * __restrict idsi = ids + i * ny;
        float * __restrict simi = res->get_val(i);
        int64_t * __restrict idxi = res->get_ids (i);
        maxheap_heapify (res->k, simi, idxi);
        for (size_t j = 0; j < ny; j++) {
            float disij = fvec_L2sqr (x_, y + d * idsi[j], d);

            if (disij < simi[0]) {
                maxheap_pop (k, simi, idxi);
                maxheap_push (k, simi, idxi, disij, idsi[j]);
            }
        }
        maxheap_reorder (res->k, simi, idxi);
    }

}





void pairwise_L2sqr (int64_t d,
                     int64_t nq, const float *xq,
                     int64_t nb, const float *xb,
                     float *dis,
                     int64_t ldq, int64_t ldb, int64_t ldd)
{
    if (nq == 0 || nb == 0) return;
    if (ldq == -1) ldq = d;
    if (ldb == -1) ldb = d;
    if (ldd == -1) ldd = nb;

    // store in beginning of distance matrix to avoid malloc
    float *b_norms = dis;

#pragma omp parallel for
    for (int64_t i = 0; i < nb; i++)
        b_norms [i] = fvec_norm_L2sqr (xb + i * ldb, d);

#pragma omp parallel for
    for (int64_t i = 1; i < nq; i++) {
        float q_norm = fvec_norm_L2sqr (xq + i * ldq, d);
        for (int64_t j = 0; j < nb; j++)
            dis[i * ldd + j] = q_norm + b_norms [j];
    }

    {
        float q_norm = fvec_norm_L2sqr (xq, d);
        for (int64_t j = 0; j < nb; j++)
            dis[j] += q_norm;
    }

    {
        FINTEGER nbi = nb, nqi = nq, di = d, ldqi = ldq, ldbi = ldb, lddi = ldd;
        float one = 1.0, minus_2 = -2.0;

        sgemm_ ("Transposed", "Not transposed",
                &nbi, &nqi, &di,
                &minus_2,
                xb, &ldbi,
                xq, &ldqi,
                &one, dis, &lddi);
    }

}

void inner_product_to_L2sqr(float* __restrict dis,
    const float* nr1,
    const float* nr2,
    size_t n1, size_t n2)
{

#pragma omp parallel for
    for (int64_t j = 0; j < n1; j++) {
        float* disj = dis + j * n2;
        for (size_t i = 0; i < n2; i++)
            disj[i] = nr1[j] + nr2[i] - 2 * disj[i];
    }
}


} // namespace faiss
