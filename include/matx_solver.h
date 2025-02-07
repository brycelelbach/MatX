////////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (c) 2021, NVIDIA Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
/////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "cublas_v2.h"
#include "cusolverDn.h"
#include "matx_dim.h"
#include "matx_error.h"
#include "matx_tensor.h"
#include <cstdio>
#include <numeric>

namespace matx {

/**
 * Dense solver base class that all dense solver types inherit common methods
 * and structures from. The dense solvers used in the 64-bit cuSolver API all
 * use host and device workspace, as well as an "info" allocation to point to
 * issues during solving.
 */
class matxDnSolver_t {
public:
  matxDnSolver_t()
  {
    MATX_ASSERT(cusolverDnCreate(&handle) == CUSOLVER_STATUS_SUCCESS,
                matxSolverError);
    MATX_ASSERT(cusolverDnCreateParams(&dn_params) == CUSOLVER_STATUS_SUCCESS,
                matxSolverError);
  }

  matxError_t SetAdvancedOptions(cusolverDnFunction_t function,
                                 cusolverAlgMode_t algo)
  {
    MATX_ASSERT(cusolverDnSetAdvOptions(dn_params, function, algo) ==
                    CUSOLVER_STATUS_SUCCESS,
                matxSolverError);
    return matxSuccess;
  }

  virtual ~matxDnSolver_t()
  {
    matxFree(d_workspace);
    matxFree(h_workspace);
    matxFree(d_info);
    cusolverDnDestroy(handle);
  }

  template <typename T, int RANK>
  void SetBatchPointers(tensor_t<T, RANK> &a)
  {
    if constexpr (RANK == 2) {
      batch_a_ptrs.push_back(&a(0, 0));
    }
    else if constexpr (RANK == 3) {
      for (int i = 0; i < a.Size(0); i++) {
        batch_a_ptrs.push_back(&a(i, 0, 0));
      }
    }
    else {
      for (int i = 0; i < a.Size(0); i++) {
        for (int j = 0; j < a.Size(1); j++) {
          batch_a_ptrs.push_back(&a(i, j, 0, 0));
        }
      }
    }
  }

  /**
   * Get a transposed view of a tensor into a user-supplied buffer
   *
   * @param tp
   *   Pointer to pre-allocated memory
   * @param a
   *   Tensor to transpose
   * @param stream
   *   CUDA stream
   */
  template <typename T, int RANK>
  static inline tensor_t<T, RANK>
  TransposeCopy(T *tp, const tensor_t<T, RANK> &a, cudaStream_t stream = 0)
  {
    auto pa = a.PermuteMatrix();

    tensor_t<T, RANK> tv{tp, pa.Shape()};
    copy(tv, pa, stream);
    return tv;
  }

  template <typename T, int RANK>
  static inline uint32_t GetNumBatches(const tensor_t<T, RANK> &a)
  {
    uint32_t cnt = 1;
    for (int i = 3; i <= RANK; i++) {
      cnt *= static_cast<uint32_t>(a.Size(RANK - i));
    }

    return cnt;
  }

  void AllocateWorkspace(size_t batches)
  {
    matxAlloc(&d_workspace, batches * dspace, MATX_DEVICE_MEMORY);
    matxAlloc((void **)&d_info, batches * sizeof(*d_info), MATX_DEVICE_MEMORY);
    matxAlloc(&h_workspace, batches * hspace, MATX_HOST_MEMORY);
  }

  virtual void GetWorkspaceSize(size_t *host, size_t *device) = 0;

protected:
  cusolverDnHandle_t handle;
  cusolverDnParams_t dn_params;
  std::vector<void *> batch_a_ptrs;
  int *d_info;
  void *d_workspace = nullptr;
  void *h_workspace = nullptr;
  size_t hspace;
  size_t dspace;
};

/**
 * Parameters needed to execute a cholesky factorization. We distinguish unique
 * factorizations mostly by the data pointer in A
 */
struct DnCholParams_t {
  int64_t n;
  void *A;
  size_t batch_size;
  cublasFillMode_t uplo;
  MatXDataType_t dtype;
};

template <typename T1, int RANK>
class matxDnCholSolverPlan_t : public matxDnSolver_t {
public:
  /**
   * Plan for solving
   * \f$\textbf{A} = \textbf{L} * \textbf{L^{H}}\f$ or \f$\textbf{A} =
   * \textbf{U} * \textbf{U^{H}}\f$ using the Cholesky method
   *
   * Creates a handle for solving the factorization of A = M * M^H of a dense
   * matrix using the Cholesky method, where M is either the upper or lower
   * triangular portion of A. Input matrix A must be a square Hermitian matrix
   * positive-definite where only the upper or lower triangle is used.
   *
   * @tparam T1
   *  Data type of A matrix
   * @tparam RANK
   *  Rank of A matrix
   *
   * @param a
   *   Input tensor view
   * @param uplo
   *   Use upper or lower triangle for computation
   *
   */
  matxDnCholSolverPlan_t(const tensor_t<T1, RANK> &a,
                         cublasFillMode_t uplo = CUBLAS_FILL_MODE_UPPER)
  {
    static_assert(RANK >= 2);

    params = GetCholParams(a, uplo);
    GetWorkspaceSize(&hspace, &dspace);
    AllocateWorkspace(params.batch_size);
  }

  void GetWorkspaceSize(size_t *host, size_t *device) override
  {
    MATX_ASSERT(cusolverDnXpotrf_bufferSize(handle, dn_params, params.uplo,
                                            params.n, MatXTypeToCudaType<T1>(),
                                            params.A, params.n,
                                            MatXTypeToCudaType<T1>(), device,
                                            host) == CUSOLVER_STATUS_SUCCESS,
                matxCudaError);
  }

  static DnCholParams_t GetCholParams(const tensor_t<T1, RANK> &a,
                                      cublasFillMode_t uplo)
  {
    DnCholParams_t params;
    params.batch_size = matxDnSolver_t::GetNumBatches(a);
    params.n = a.Size(RANK - 1);
    params.A = a.Data();
    params.uplo = uplo;
    params.dtype = TypeToInt<T1>();

    return params;
  }

  void Exec(tensor_t<T1, RANK> &out, const tensor_t<T1, RANK> &a,
            cudaStream_t stream, cublasFillMode_t uplo = CUBLAS_FILL_MODE_UPPER)
  {
    // Ensure matrix is square
    MATX_ASSERT(a.Size(RANK - 1) == a.Size(RANK - 2), matxInvalidSize);

    // Ensure output size matches input
    for (int i = 0; i < RANK; i++) {
      MATX_ASSERT(out.Size(i) == a.Size(i), matxInvalidSize);
    }

    cusolverDnSetStream(handle, stream);

    SetBatchPointers(out);
    if (out.Data() != a.Data()) {
      copy(out, a, stream);
    }

    // At this time cuSolver does not have a batched 64-bit cholesky interface.
    // Change this to use the batched version once available.
    for (size_t i = 0; i < batch_a_ptrs.size(); i++) {
      auto ret = cusolverDnXpotrf(
          handle, dn_params, uplo, params.n, MatXTypeToCudaType<T1>(),
          batch_a_ptrs[i], params.n, MatXTypeToCudaType<T1>(),
          reinterpret_cast<uint8_t *>(d_workspace) + i * dspace, dspace,
          reinterpret_cast<uint8_t *>(h_workspace) + i * hspace, hspace,
          d_info + i);

      MATX_ASSERT(ret == CUSOLVER_STATUS_SUCCESS, matxSolverError);
    }
  }

  /**
   * Cholesky solver handle destructor
   *
   * Destroys any helper data used for provider type and any workspace memory
   * created
   *
   */
  ~matxDnCholSolverPlan_t() {}

private:
  DnCholParams_t params;
};

/**
 * Crude hash to get a reasonably good delta for collisions. This doesn't need
 * to be perfect, but fast enough to not slow down lookups, and different enough
 * so the common solver parameters change
 */
struct DnCholParamsKeyHash {
  std::size_t operator()(const DnCholParams_t &k) const noexcept
  {
    return (std::hash<index_t>()(k.n)) + (std::hash<index_t>()(k.batch_size));
  }
};

/**
 * Test cholesky parameters for equality. Unlike the hash, all parameters must
 * match.
 */
struct DnCholParamsKeyEq {
  bool operator()(const DnCholParams_t &l, const DnCholParams_t &t) const
      noexcept
  {
    return l.n == t.n && l.batch_size == t.batch_size && l.dtype == t.dtype;
  }
};

// Static caches of inverse handles
static matxCache_t<DnCholParams_t, DnCholParamsKeyHash, DnCholParamsKeyEq>
    dnchol_cache;

/**
 * Perform a Cholesky decomposition using a cached plan
 *
 * See documentation of matxDnCholSolverPlan_t for a description of how the
 * algorithm works. This function provides a simple interface to the cuSolver
 * library by deducing all parameters needed to perform a Cholesky decomposition
 * from only the matrix A. The input and output parameters may be the same
 * tensor. In that case, the input is destroyed and the output is stored
 * in-place.
 *
 * @tparam T1
 *   Data type of matrix A
 * @tparam RANK
 *   Rank of matrix A
 *
 * @param out
 *   Output tensor
 * @param a
 *   Input tensor
 * @param stream
 *   CUDA stream
 * @param uplo
 *   Part of matrix to fill
 */
template <typename T1, int RANK>
void chol(tensor_t<T1, RANK> &out, const tensor_t<T1, RANK> &a,
          cudaStream_t stream = 0,
          cublasFillMode_t uplo = CUBLAS_FILL_MODE_UPPER)
{
  /* Temporary WAR
     cuSolver doesn't support row-major layouts. Since we want to make the
     library appear as though everything is row-major, we take a performance hit
     to transpose in and out of the function. Eventually this may be fixed in
     cuSolver.
  */
  T1 *tp;
  matxAlloc(reinterpret_cast<void **>(&tp), a.Bytes(), MATX_ASYNC_DEVICE_MEMORY,
            stream);
  auto tv = matxDnSolver_t::TransposeCopy(tp, a, stream);

  // Get parameters required by these tensors
  auto params = matxDnCholSolverPlan_t<T1, RANK>::GetCholParams(tv, uplo);
  params.uplo = uplo;

  // Get cache or new inverse plan if it doesn't exist
  auto ret = dnchol_cache.Lookup(params);
  if (ret == std::nullopt) {
    auto tmp = new matxDnCholSolverPlan_t{tv};
    dnchol_cache.Insert(params, static_cast<void *>(tmp));
    tmp->Exec(tv, tv, stream, uplo);
  }
  else {
    auto chol_type =
        static_cast<matxDnCholSolverPlan_t<T1, RANK> *>(ret.value());
    chol_type->Exec(tv, tv, stream, uplo);
  }

  /* Temporary WAR
   * Copy and free async buffer for transpose */
  copy(out, tv.PermuteMatrix(), stream);
  matxFree(tp);
}

/***************************************** LU FACTORIZATION
 * *********************************************/

/**
 * Parameters needed to execute an LU factorization. We distinguish unique
 * factorizations mostly by the data pointer in A
 */
struct DnLUParams_t {
  int64_t m;
  int64_t n;
  void *A;
  void *piv;
  size_t batch_size;
  MatXDataType_t dtype;
};

template <typename T1, int RANK>
class matxDnLUSolverPlan_t : public matxDnSolver_t {
public:
  /**
   * Plan for factoring A such that \f$\textbf{P} * \textbf{A} = \textbf{L} *
   * \textbf{U}\f$
   *
   * Creates a handle for factoring matrix A into the format above. Matrix must
   * not be singular.
   *
   * @tparam T1
   *  Data type of A matrix
   * @tparam RANK
   *  Rank of A matrix
   *
   * @param piv
   *   Pivot indices
   * @param a
   *   Input tensor view
   *
   */
  matxDnLUSolverPlan_t(tensor_t<int64_t, RANK - 1> &piv,
                       const tensor_t<T1, RANK> &a)
  {
    static_assert(RANK >= 2);

    params = GetLUParams(piv, a);
    GetWorkspaceSize(&hspace, &dspace);
    AllocateWorkspace(params.batch_size);
  }

  void GetWorkspaceSize(size_t *host, size_t *device) override
  {
    MATX_ASSERT(cusolverDnXgetrf_bufferSize(handle, dn_params, params.m,
                                            params.n, MatXTypeToCudaType<T1>(),
                                            params.A, params.m,
                                            MatXTypeToCudaType<T1>(), device,
                                            host) == CUSOLVER_STATUS_SUCCESS,
                matxCudaError);
  }

  static DnLUParams_t GetLUParams(tensor_t<int64_t, RANK - 1> &piv,
                                  const tensor_t<T1, RANK> &a) noexcept
  {
    DnLUParams_t params;
    params.batch_size = matxDnSolver_t::GetNumBatches(a);
    params.m = a.Size(RANK - 2);
    params.n = a.Size(RANK - 1);
    params.A = a.Data();
    params.piv = piv.Data();
    params.dtype = TypeToInt<T1>();

    return params;
  }

  void Exec(tensor_t<T1, RANK> &out, tensor_t<int64_t, RANK - 1> &piv,
            const tensor_t<T1, RANK> &a, const cudaStream_t stream = 0)
  {
    cusolverDnSetStream(handle, stream);
    int info;

    if constexpr (RANK == 2) {
      batch_piv_ptrs.push_back(&piv(0));
    }
    else if constexpr (RANK == 3) {
      for (int i = 0; i < piv.Size(0); i++) {
        batch_piv_ptrs.push_back(&piv(i, 0));
      }
    }
    else {
      for (int i = 0; i < piv.Size(0); i++) {
        for (int j = 0; j < piv.Size(1); j++) {
          batch_piv_ptrs.push_back(&piv(i, j, 0));
        }
      }
    }

    SetBatchPointers(out);

    if (out.Data() != a.Data()) {
      copy(out, a, stream);
    }

    // At this time cuSolver does not have a batched 64-bit LU interface. Change
    // this to use the batched version once available.
    for (size_t i = 0; i < batch_a_ptrs.size(); i++) {
      auto ret = cusolverDnXgetrf(
          handle, dn_params, params.m, params.n, MatXTypeToCudaType<T1>(),
          batch_a_ptrs[i], params.m, batch_piv_ptrs[i],
          MatXTypeToCudaType<T1>(),
          reinterpret_cast<uint8_t *>(d_workspace) + i * dspace, dspace,
          reinterpret_cast<uint8_t *>(h_workspace) + i * hspace, hspace,
          d_info + i);

      MATX_ASSERT(ret == CUSOLVER_STATUS_SUCCESS, matxSolverError);

      // This will block. Figure this out later
      cudaMemcpy(&info, d_info + i, sizeof(info), cudaMemcpyDeviceToHost);
      MATX_ASSERT(info == 0, matxSolverError);
    }
  }

  /**
   * LU solver handle destructor
   *
   * Destroys any helper data used for provider type and any workspace memory
   * created
   *
   */
  ~matxDnLUSolverPlan_t() {}

private:
  std::vector<int64_t *> batch_piv_ptrs;
  DnLUParams_t params;
};

/**
 * Crude hash to get a reasonably good delta for collisions. This doesn't need
 * to be perfect, but fast enough to not slow down lookups, and different enough
 * so the common solver parameters change
 */
struct DnLUParamsKeyHash {
  std::size_t operator()(const DnLUParams_t &k) const noexcept
  {
    return (std::hash<index_t>()(k.m)) + (std::hash<index_t>()(k.n)) +
           (std::hash<index_t>()(k.batch_size));
  }
};

/**
 * Test LU parameters for equality. Unlike the hash, all parameters must match.
 */
struct DnLUParamsKeyEq {
  bool operator()(const DnLUParams_t &l, const DnLUParams_t &t) const noexcept
  {
    return l.n == t.n && l.m == t.m && l.batch_size == t.batch_size &&
           l.dtype == t.dtype;
  }
};

// Static caches of LU handles
static matxCache_t<DnLUParams_t, DnLUParamsKeyHash, DnLUParamsKeyEq> dnlu_cache;

/**
 * Perform a LU decomposition using a cached plan
 *
 * See documentation of matxDnLUSolverPlan_t for a description of how the
 * algorithm works. This function provides a simple interface to the cuSolver
 * library by deducing all parameters needed to perform an LU decomposition from
 * only the matrix A. The input and output parameters may be the same tensor. In
 * that case, the input is destroyed and the output is stored in-place.
 *
 * @tparam T1
 *   Data type of matrix A
 * @tparam RANK
 *   Rank of matrix A
 *
 * @param out
 *   Output tensor view
 * @param piv
 *   Output of pivot indices
 * @param a
 *   Input matrix A
 * @param stream
 *   CUDA stream
 */
template <typename T1, int RANK>
void lu(tensor_t<T1, RANK> &out, tensor_t<int64_t, RANK - 1> &piv,
        const tensor_t<T1, RANK> &a, const cudaStream_t stream = 0)
{
  /* Temporary WAR
     cuSolver doesn't support row-major layouts. Since we want to make the
     library appear as though everything is row-major, we take a performance hit
     to transpose in and out of the function. Eventually this may be fixed in
     cuSolver.
  */
  T1 *tp;
  matxAlloc(reinterpret_cast<void **>(&tp), a.Bytes(), MATX_ASYNC_DEVICE_MEMORY,
            stream);
  auto tv = matxDnSolver_t::TransposeCopy(tp, a, stream);
  auto tvt = tv.PermuteMatrix();

  // Get parameters required by these tensors
  auto params = matxDnLUSolverPlan_t<T1, RANK>::GetLUParams(piv, tvt);

  // Get cache or new LU plan if it doesn't exist
  auto ret = dnlu_cache.Lookup(params);
  if (ret == std::nullopt) {
    auto tmp = new matxDnLUSolverPlan_t{piv, tvt};

    dnlu_cache.Insert(params, static_cast<void *>(tmp));
    tmp->Exec(tvt, piv, tvt, stream);
  }
  else {
    auto lu_type = static_cast<matxDnLUSolverPlan_t<T1, RANK> *>(ret.value());
    lu_type->Exec(tvt, piv, tvt, stream);
  }

  /* Temporary WAR
   * Copy and free async buffer for transpose */
  copy(out, tv.PermuteMatrix(), stream);
  matxFree(tp);
}

/**
 * Compute the determinant of a matrix
 *
 * Computes the terminant of a matrix by first computing the LU composition,
 * then reduces the product of the diagonal elements of U. The input and output
 * parameters may be the same tensor. In that case, the input is destroyed and
 * the output is stored in-place.
 *
 * @tparam T1
 *   Data type of matrix A
 * @tparam RANK
 *   Rank of matrix A
 *
 * @param out
 *   Output tensor view
 * @param a
 *   Input matrix A
 * @param stream
 *   CUDA stream
 */
template <typename T1, int RANK>
void det(tensor_t<T1, RANK - 2> &out, const tensor_t<T1, RANK> &a,
         const cudaStream_t stream = 0)
{
  // Get parameters required by these tensors
  tensorShape_t<RANK - 1> s;

  // Set batching dimensions of piv
  for (int i = 0; i < RANK - 2; i++) {
    s.SetSize(i, a.Size(i));
  }

  s.SetSize(RANK - 2, std::min(a.Size(RANK - 1), a.Size(RANK - 2)));

  tensor_t<int64_t, RANK - 1> piv{s};
  tensor_t<T1, RANK> ac{a.Shape()};

  lu(ac, piv, a, stream);
  prod(out, diag(ac), stream);
}

/***************************************** QR FACTORIZATION
 * *********************************************/

/**
 * Parameters needed to execute a QR factorization. We distinguish unique
 * factorizations mostly by the data pointer in A
 */
struct DnQRParams_t {
  int64_t m;
  int64_t n;
  void *A;
  void *tau;
  size_t batch_size;
  MatXDataType_t dtype;
};

template <typename T1, int RANK>
class matxDnQRSolverPlan_t : public matxDnSolver_t {
public:
  /**
   * Plan for factoring A such that \f$\textbf{A} = \textbf{Q} * \textbf{R}\f$
   *
   * Creates a handle for factoring matrix A into the format above. QR
   * decomposition in cuBLAS/cuSolver does not return the Q matrix directly, and
   * it must be computed separately used the Householder reflections in the tau
   * output, along with the overwritten A matrix input. The input and output
   * parameters may be the same tensor. In that case, the input is destroyed and
   * the output is stored in-place.
   *
   * @tparam T1
   *  Data type of A matrix
   * @tparam RANK
   *  Rank of A matrix
   *
   * @param tau
   *   Scaling factors for reflections
   * @param a
   *   Input tensor view
   *
   */
  matxDnQRSolverPlan_t(tensor_t<T1, RANK - 1> &tau,
                       const tensor_t<T1, RANK> &a)
  {
    static_assert(RANK >= 2);

    params = GetQRParams(tau, a);
    GetWorkspaceSize(&hspace, &dspace);
    AllocateWorkspace(params.batch_size);
  }

  void GetWorkspaceSize(size_t *host, size_t *device) override
  {
    MATX_ASSERT(
        cusolverDnXgeqrf_bufferSize(
            handle, dn_params, params.m, params.n, MatXTypeToCudaType<T1>(),
            params.A, params.m, MatXTypeToCudaType<T1>(), params.tau,
            MatXTypeToCudaType<T1>(), device, host) == CUSOLVER_STATUS_SUCCESS,
        matxCudaError);
  }

  static DnQRParams_t GetQRParams(tensor_t<T1, RANK - 1> &tau,
                                  const tensor_t<T1, RANK> &a)
  {
    DnQRParams_t params;

    params.batch_size = matxDnSolver_t::GetNumBatches(a);
    params.m = a.Size(RANK - 2);
    params.n = a.Size(RANK - 1);
    params.A = a.Data();
    params.tau = tau.Data();
    params.dtype = TypeToInt<T1>();

    return params;
  }

  void Exec(tensor_t<T1, RANK> &out, tensor_t<T1, RANK - 1> &tau,
            const tensor_t<T1, RANK> &a, cudaStream_t stream = 0)
  {
    // Ensure output size matches input
    for (int i = 0; i < RANK; i++) {
      MATX_ASSERT(out.Size(i) == a.Size(i), matxInvalidSize);
    }

    SetBatchPointers(out);

    if constexpr (RANK == 2) {
      batch_tau_ptrs.push_back(&tau(0));
    }
    else if constexpr (RANK == 3) {
      for (int i = 0; i < tau.Size(0); i++) {
        batch_tau_ptrs.push_back(&tau(i, 0));
      }
    }
    else {
      for (int i = 0; i < tau.Size(0); i++) {
        for (int j = 0; j < tau.Size(1); j++) {
          batch_tau_ptrs.push_back(&tau(i, j, 0));
        }
      }
    }

    if (out.Data() != a.Data()) {
      copy(out, a, stream);
    }

    cusolverDnSetStream(handle, stream);
    int info;

    // At this time cuSolver does not have a batched 64-bit LU interface. Change
    // this to use the batched version once available.
    for (size_t i = 0; i < batch_a_ptrs.size(); i++) {
      auto ret = cusolverDnXgeqrf(
          handle, dn_params, params.m, params.n, MatXTypeToCudaType<T1>(),
          batch_a_ptrs[i], params.m, MatXTypeToCudaType<T1>(),
          batch_tau_ptrs[i], MatXTypeToCudaType<T1>(),
          reinterpret_cast<uint8_t *>(d_workspace) + i * dspace, dspace,
          reinterpret_cast<uint8_t *>(h_workspace) + i * hspace, hspace,
          d_info + i);

      MATX_ASSERT(ret == CUSOLVER_STATUS_SUCCESS, matxSolverError);

      // This will block. Figure this out later
      cudaMemcpy(&info, d_info + i, sizeof(info), cudaMemcpyDeviceToHost);
      MATX_ASSERT(info == 0, matxSolverError);
    }
  }

  /**
   * QR solver handle destructor
   *
   * Destroys any helper data used for provider type and any workspace memory
   * created
   *
   */
  ~matxDnQRSolverPlan_t() {}

private:
  std::vector<T1 *> batch_tau_ptrs;
  DnQRParams_t params;
};

/**
 * Crude hash to get a reasonably good delta for collisions. This doesn't need
 * to be perfect, but fast enough to not slow down lookups, and different enough
 * so the common solver parameters change
 */
struct DnQRParamsKeyHash {
  std::size_t operator()(const DnQRParams_t &k) const noexcept
  {
    return (std::hash<index_t>()(k.m)) + (std::hash<index_t>()(k.n)) +
           (std::hash<index_t>()(k.batch_size));
  }
};

/**
 * Test QR parameters for equality. Unlike the hash, all parameters must match.
 */
struct DnQRParamsKeyEq {
  bool operator()(const DnQRParams_t &l, const DnQRParams_t &t) const noexcept
  {
    return l.n == t.n && l.m == t.m && l.batch_size == t.batch_size &&
           l.dtype == t.dtype;
  }
};

// Static caches of QR handles
static matxCache_t<DnQRParams_t, DnQRParamsKeyHash, DnQRParamsKeyEq> dnqr_cache;

/**
 * Perform a QR decomposition using a cached plan
 *
 * See documentation of matxDnQRSolverPlan_t for a description of how the
 * algorithm works. This function provides a simple interface to the cuSolver
 * library by deducing all parameters needed to perform a QR decomposition from
 * only the matrix A. The input and output parameters may be the same tensor. In
 * that case, the input is destroyed and the output is stored in-place.
 *
 * @tparam T1
 *   Data type of matrix A
 * @tparam RANK
 *   Rank of matrix A
 *
 * @param out
 *   Output tensor view
 * @param tau
 *   Output of reflection scalar values
 * @param a
 *   Input tensor A
 * @param stream
 *   CUDA stream
 */
template <typename T1, int RANK>
void qr(tensor_t<T1, RANK> &out, tensor_t<T1, RANK - 1> &tau,
        const tensor_t<T1, RANK> &a, cudaStream_t stream = 0)
{
  /* Temporary WAR
     cuSolver doesn't support row-major layouts. Since we want to make the
     library appear as though everything is row-major, we take a performance hit
     to transpose in and out of the function. Eventually this may be fixed in
     cuSolver.
  */
  T1 *tp;
  matxAlloc(reinterpret_cast<void **>(&tp), a.Bytes(), MATX_ASYNC_DEVICE_MEMORY,
            stream);
  auto tv = matxDnSolver_t::TransposeCopy(tp, a, stream);
  auto tvt = tv.PermuteMatrix();

  // Get parameters required by these tensors
  auto params = matxDnQRSolverPlan_t<T1, RANK>::GetQRParams(tau, tvt);

  // Get cache or new QR plan if it doesn't exist
  auto ret = dnqr_cache.Lookup(params);
  if (ret == std::nullopt) {
    auto tmp = new matxDnQRSolverPlan_t{tau, tvt};

    dnqr_cache.Insert(params, static_cast<void *>(tmp));
    tmp->Exec(tvt, tau, tvt, stream);
  }
  else {
    auto qr_type = static_cast<matxDnQRSolverPlan_t<T1, RANK> *>(ret.value());
    qr_type->Exec(tvt, tau, tvt, stream);
  }

  /* Temporary WAR
   * Copy and free async buffer for transpose */
  copy(out, tv.PermuteMatrix(), stream);
  matxFree(tp);
}

/********************************************** SVD
 * *********************************************/

/**
 * Parameters needed to execute singular value decomposition. We distinguish
 * unique factorizations mostly by the data pointer in A.
 */
struct DnSVDParams_t {
  int64_t m;
  int64_t n;
  char jobu;
  char jobvt;
  void *A;
  void *U;
  void *V;
  void *S;
  size_t batch_size;
  MatXDataType_t dtype;
};

template <typename T1, typename T2, typename T3, typename T4, int RANK>
class matxDnSVDSolverPlan_t : public matxDnSolver_t {
public:
  /**
   * Plan for factoring A such that \f$\textbf{A} = \textbf{U} * \textbf{\Sigma}
   * * \textbf{V^{H}}\f$
   *
   * Creates a handle for decomposing matrix A into the format above.
   *
   * @tparam T1
   *  Data type of A matrix
   * @tparam T2
   *  Data type of U matrix
   * @tparam T3
   *  Data type of S vector
   * @tparam T4
   *  Data type of V matrix
   * @tparam RANK
   *  Rank of A, U, and V matrices, and RANK-1 of S
   *
   * @param u
   *   Output tensor view for U matrix
   * @param s
   *   Output tensor view for S matrix
   * @param v
   *   Output tensor view for V matrix
   * @param a
   *   Input tensor view for A matrix
   * @param jobu
   *   Specifies options for computing all or part of the matrix U: = 'A'. See
   * cuSolver documentation for more info
   * @param jobvt
   *   specifies options for computing all or part of the matrix V**T. See
   * cuSolver documentation for more info
   *
   */
  matxDnSVDSolverPlan_t(tensor_t<T2, RANK> &u,
                        tensor_t<T3, RANK - 1> &s,
                        tensor_t<T4, RANK> &v,
                        const tensor_t<T1, RANK> &a, const char jobu = 'A',
                        const char jobvt = 'A')
  {
    static_assert(RANK >= 2);

    T1 *tmp;
    matxAlloc(reinterpret_cast<void **>(&tmp), a.Bytes(), MATX_DEVICE_MEMORY);
    MATX_ASSERT(tmp != nullptr, matxOutOfMemory);

    scratch = new tensor_t<T1, RANK>(tmp, a.Shape());
    params = GetSVDParams(u, s, v, *scratch, jobu, jobvt);

    GetWorkspaceSize(&hspace, &dspace);

    SetBatchPointers(*scratch);
    AllocateWorkspace(params.batch_size);
  }

  void GetWorkspaceSize(size_t *host, size_t *device) override
  {
    MATX_ASSERT(
        cusolverDnXgesvd_bufferSize(
            handle, dn_params, params.jobu, params.jobvt, params.m, params.n,
            MatXTypeToCudaType<T1>(), params.A, params.m,
            MatXTypeToCudaType<T3>(), params.S, MatXTypeToCudaType<T2>(),
            params.U, params.m, MatXTypeToCudaType<T4>(), params.V, params.n,
            MatXTypeToCudaType<T1>(), device, host) == CUSOLVER_STATUS_SUCCESS,
        matxCudaError);
  }

  static DnSVDParams_t
  GetSVDParams(tensor_t<T2, RANK> &u, tensor_t<T3, RANK - 1> &s,
               tensor_t<T4, RANK> &v, const tensor_t<T1, RANK> &a,
               const char jobu = 'A', const char jobvt = 'A')
  {
    DnSVDParams_t params;
    params.batch_size = matxDnSolver_t::GetNumBatches(a);
    params.m = a.Size(RANK - 2);
    params.n = a.Size(RANK - 1);
    params.A = a.Data();
    params.U = u.Data();
    params.V = v.Data();
    params.S = s.Data();
    params.jobu = jobu;
    params.jobvt = jobvt;
    params.dtype = TypeToInt<T1>();

    return params;
  }

  void Exec(tensor_t<T2, RANK> u, tensor_t<T3, RANK - 1> s,
            tensor_t<T4, RANK> v, const tensor_t<T1, RANK> a,
            const char jobu = 'A', const char jobvt = 'A',
            cudaStream_t stream = 0)
  {
    if constexpr (RANK == 2) {
      batch_s_ptrs.push_back(&s(0));
      batch_u_ptrs.push_back(&u(0, 0));
      batch_v_ptrs.push_back(&v(0, 0));
    }
    else if constexpr (RANK == 3) {
      for (int i = 0; i < a.Size(0); i++) {
        batch_s_ptrs.push_back(&s(i, 0));
        batch_u_ptrs.push_back(&u(i, 0, 0));
        batch_v_ptrs.push_back(&v(i, 0, 0));
      }
    }
    else {
      for (int i = 0; i < a.Size(0); i++) {
        for (int j = 0; j < a.Size(1); j++) {
          batch_s_ptrs.push_back(&s(i, j, 0));
          batch_u_ptrs.push_back(&u(i, j, 0, 0));
          batch_v_ptrs.push_back(&v(i, j, 0, 0));
        }
      }
    }

    cusolverDnSetStream(handle, stream);
    copy(*scratch, a, stream);
    int info;

    // At this time cuSolver does not have a batched 64-bit LU interface. Change
    // this to use the batched version once available.
    for (size_t i = 0; i < batch_a_ptrs.size(); i++) {
      auto ret = cusolverDnXgesvd(
          handle, dn_params, jobu, jobvt, params.m, params.n,
          MatXTypeToCudaType<T1>(), batch_a_ptrs[i], params.m,
          MatXTypeToCudaType<T3>(), batch_s_ptrs[i], MatXTypeToCudaType<T2>(),
          batch_u_ptrs[i], params.m, MatXTypeToCudaType<T4>(), batch_v_ptrs[i],
          params.n, MatXTypeToCudaType<T1>(),
          reinterpret_cast<uint8_t *>(d_workspace) + i * dspace, dspace,
          reinterpret_cast<uint8_t *>(h_workspace) + i * hspace, hspace,
          d_info + i);

      MATX_ASSERT(ret == CUSOLVER_STATUS_SUCCESS, matxSolverError);

      // This will block. Figure this out later
      cudaMemcpy(&info, d_info + i, sizeof(info), cudaMemcpyDeviceToHost);
      MATX_ASSERT(info == 0, matxSolverError);
    }
  }

  /**
   * SVD solver handle destructor
   *
   * Destroys any helper data used for provider type and any workspace memory
   * created
   *
   */
  ~matxDnSVDSolverPlan_t() {}

private:
  std::vector<T1 *> batch_s_ptrs;
  std::vector<T1 *> batch_v_ptrs;
  std::vector<T1 *> batch_u_ptrs;
  tensor_t<T1, RANK> *scratch = nullptr;
  DnSVDParams_t params;
};

/**
 * Crude hash to get a reasonably good delta for collisions. This doesn't need
 * to be perfect, but fast enough to not slow down lookups, and different enough
 * so the common solver parameters change
 */
struct DnSVDParamsKeyHash {
  std::size_t operator()(const DnSVDParams_t &k) const noexcept
  {
    return (std::hash<index_t>()(k.m)) + (std::hash<index_t>()(k.n)) +
           (std::hash<index_t>()(k.batch_size));
  }
};

/**
 * Test SVD parameters for equality. Unlike the hash, all parameters must match.
 */
struct DnSVDParamsKeyEq {
  bool operator()(const DnSVDParams_t &l, const DnSVDParams_t &t) const noexcept
  {
    return l.n == t.n && l.m == t.m && l.jobu == t.jobu && l.jobvt == t.jobvt &&
           l.batch_size == t.batch_size && l.dtype == t.dtype;
  }
};

// Static caches of SVD handles
static matxCache_t<DnSVDParams_t, DnSVDParamsKeyHash, DnSVDParamsKeyEq>
    dnsvd_cache;

/**
 * Perform a SVD decomposition using a cached plan
 *
 * See documentation of matxDnSVDSolverPlan_t for a description of how the
 * algorithm works. This function provides a simple interface to the cuSolver
 * library by deducing all parameters needed to perform a SVD decomposition from
 * only the matrix A.
 *
 * @tparam T1
 *   Data type of matrix A
 * @tparam RANK
 *   Rank of matrix A
 *
 * @param u
 *   U matrix output
 * @param s
 *   Sigma matrix output
 * @param v
 *   V matrix output
 * @param a
 *   Input matrix A
 * @param stream
 *   CUDA stream
 * @param jobu
 *   Specifies options for computing all or part of the matrix U: = 'A'. See
 * cuSolver documentation for more info
 * @param jobvt
 *   specifies options for computing all or part of the matrix V**T. See
 * cuSolver documentation for more info
 *
 */
template <typename T1, typename T2, typename T3, typename T4, int RANK>
void svd(tensor_t<T2, RANK> &u, tensor_t<T3, RANK - 1> &s,
         tensor_t<T4, RANK> &v, const tensor_t<T1, RANK> &a,
         cudaStream_t stream = 0, const char jobu = 'A', const char jobvt = 'A')
{
  /* Temporary WAR
     cuSolver doesn't support row-major layouts. Since we want to make the
     library appear as though everything is row-major, we take a performance hit
     to transpose in and out of the function. Eventually this may be fixed in
     cuSolver.
  */
  T1 *tp;
  matxAlloc(reinterpret_cast<void **>(&tp), a.Bytes(), MATX_ASYNC_DEVICE_MEMORY,
            stream);
  auto tv = matxDnSolver_t::TransposeCopy(tp, a, stream);
  auto tvt = tv.PermuteMatrix();

  // Get parameters required by these tensors
  auto params = matxDnSVDSolverPlan_t<T1, T2, T3, T4, RANK>::GetSVDParams(
      u, s, v, tvt, jobu, jobvt);

  // Get cache or new QR plan if it doesn't exist
  auto ret = dnsvd_cache.Lookup(params);
  if (ret == std::nullopt) {
    auto tmp = new matxDnSVDSolverPlan_t{u, s, v, tvt, jobu, jobvt};

    dnsvd_cache.Insert(params, static_cast<void *>(tmp));
    tmp->Exec(u, s, v, tvt, jobu, jobvt, stream);
  }
  else {
    auto svd_type =
        static_cast<matxDnSVDSolverPlan_t<T1, T2, T3, T4, RANK> *>(ret.value());
    svd_type->Exec(u, s, v, tvt, jobu, jobvt, stream);
  }

  /* Temporary WAR
   * Copy and free async buffer for transpose */
  matxFree(tp);
}

/*************************************** Eigenvalues and eigenvectors
 * *************************************/

/**
 * Parameters needed to execute singular value decomposition. We distinguish
 * unique factorizations mostly by the data pointer in A.
 */
struct DnEigParams_t {
  int64_t m;
  cusolverEigMode_t jobz;
  cublasFillMode_t uplo;
  void *A;
  void *out;
  void *W;
  size_t batch_size;
  MatXDataType_t dtype;
};

template <typename T1, typename T2, int RANK>
class matxDnEigSolverPlan_t : public matxDnSolver_t {
public:
  /**
   * Plan computing eigenvalues/vectors on A such that:
   *
   * \f$\textbf{A} * textbf{V} = \textbf{V} * \textbf{\Lambda}\f$
   *
   *
   * @tparam T1
   *  Data type of A matrix
   * @tparam T2
   *  Data type of W matrix
   * @tparam RANK
   *  Rank of A matrix
   *
   * @param w
   *   Eigenvalues of A
   * @param a
   *   Input tensor view
   * @param jobz
   *   CUSOLVER_EIG_MODE_VECTOR to compute eigenvectors or
   * CUSOLVER_EIG_MODE_NOVECTOR to not compute
   * @param uplo
   *   Where to store data in A
   *
   */
  matxDnEigSolverPlan_t(tensor_t<T2, RANK - 1> &w,
                        const tensor_t<T1, RANK> &a,
                        cusolverEigMode_t jobz = CUSOLVER_EIG_MODE_VECTOR,
                        cublasFillMode_t uplo = CUBLAS_FILL_MODE_UPPER)
  {
    static_assert(RANK >= 2);

    params = GetEigParams(w, a, jobz, uplo);
    GetWorkspaceSize(&hspace, &dspace);
    AllocateWorkspace(params.batch_size);
  }

  void GetWorkspaceSize(size_t *host, size_t *device) override
  {
    MATX_ASSERT(cusolverDnXsyevd_bufferSize(
                    handle, dn_params, params.jobz, params.uplo, params.m,
                    MatXTypeToCudaType<T1>(), params.A, params.m,
                    MatXTypeToCudaType<T2>(), params.W,
                    MatXTypeToCudaType<T1>(), device,
                    host) == CUSOLVER_STATUS_SUCCESS,
                matxCudaError);
  }

  static DnEigParams_t GetEigParams(tensor_t<T2, RANK - 1> &w,
                                    const tensor_t<T1, RANK> &a,
                                    cusolverEigMode_t jobz,
                                    cublasFillMode_t uplo)
  {
    DnEigParams_t params;
    params.batch_size = matxDnSolver_t::GetNumBatches(a);
    params.m = a.Size(RANK - 1);
    params.A = a.Data();
    params.W = w.Data();
    params.jobz = jobz;
    params.uplo = uplo;
    params.dtype = TypeToInt<T1>();

    return params;
  }

  void Exec(tensor_t<T1, RANK> &out, tensor_t<T2, RANK - 1> &w,
            const tensor_t<T1, RANK> &a,
            cusolverEigMode_t jobz = CUSOLVER_EIG_MODE_VECTOR,
            cublasFillMode_t uplo = CUBLAS_FILL_MODE_UPPER,
            cudaStream_t stream = 0)
  {
    // Ensure matrix is square
    MATX_ASSERT(a.Size(RANK - 1) == a.Size(RANK - 2), matxInvalidSize);

    // Ensure output size matches input
    for (int i = 0; i < RANK; i++) {
      MATX_ASSERT(out.Size(i) == a.Size(i), matxInvalidSize);
    }

    if constexpr (RANK == 2) {
      batch_w_ptrs.push_back(&w(0));
    }
    else if constexpr (RANK == 3) {
      for (int i = 0; i < a.Size(0); i++) {
        batch_w_ptrs.push_back(&w(i, 0));
      }
    }
    else {
      for (int i = 0; i < a.Size(0); i++) {
        for (int j = 0; j < a.Size(1); j++) {
          batch_w_ptrs.push_back(&w(i, j, 0));
        }
      }
    }

    SetBatchPointers(out);

    if (out.Data() != a.Data()) {
      copy(out, a, stream);
    }

    cusolverDnSetStream(handle, stream);
    int info;

    // At this time cuSolver does not have a batched 64-bit LU interface. Change
    // this to use the batched version once available.
    for (size_t i = 0; i < batch_a_ptrs.size(); i++) {
      auto ret = cusolverDnXsyevd(
          handle, dn_params, jobz, uplo, params.m, MatXTypeToCudaType<T1>(),
          batch_a_ptrs[i], params.m, MatXTypeToCudaType<T2>(), batch_w_ptrs[i],
          MatXTypeToCudaType<T1>(),
          reinterpret_cast<uint8_t *>(d_workspace) + i * dspace, dspace,
          reinterpret_cast<uint8_t *>(h_workspace) + i * hspace, hspace,
          d_info + i);

      MATX_ASSERT(ret == CUSOLVER_STATUS_SUCCESS, matxSolverError);

      // This will block. Figure this out later
      cudaMemcpy(&info, d_info + i, sizeof(info), cudaMemcpyDeviceToHost);
      MATX_ASSERT(info == 0, matxSolverError);
    }
  }

  /**
   * Eigen solver handle destructor
   *
   * Destroys any helper data used for provider type and any workspace memory
   * created
   *
   */
  ~matxDnEigSolverPlan_t() {}

private:
  std::vector<T1 *> batch_w_ptrs;
  DnEigParams_t params;
};

/**
 * Crude hash to get a reasonably good delta for collisions. This doesn't need
 * to be perfect, but fast enough to not slow down lookups, and different enough
 * so the common solver parameters change
 */
struct DnEigParamsKeyHash {
  std::size_t operator()(const DnEigParams_t &k) const noexcept
  {
    return (std::hash<index_t>()(k.m)) + (std::hash<index_t>()(k.batch_size));
  }
};

/**
 * Test Eigen parameters for equality. Unlike the hash, all parameters must
 * match.
 */
struct DnEigParamsKeyEq {
  bool operator()(const DnEigParams_t &l, const DnEigParams_t &t) const noexcept
  {
    return l.m == t.m && l.batch_size == t.batch_size && l.dtype == t.dtype;
  }
};

// Static caches of Eig handles
static matxCache_t<DnEigParams_t, DnEigParamsKeyHash, DnEigParamsKeyEq>
    dneig_cache;

/**
 * Perform a Eig decomposition using a cached plan
 *
 * See documentation of matxDnEigSolverPlan_t for a description of how the
 * algorithm works. This function provides a simple interface to the cuSolver
 * library by deducing all parameters needed to perform a eigen decomposition
 * from only the matrix A. The input and output parameters may be the same
 * tensor. In that case, the input is destroyed and the output is stored
 * in-place.
 *
 * @tparam T1
 *   Data type of matrix A
 * @tparam RANK
 *   Rank of matrix A
 *
 * @param out
 *   Output tensor view
 * @param w
 *   Eigenvalues output
 * @param a
 *   Input matrix A
 * @param stream
 *   CUDA stream
 * @param jobz
 *   CUSOLVER_EIG_MODE_VECTOR to compute eigenvectors or
 * CUSOLVER_EIG_MODE_NOVECTOR to not compute
 * @param uplo
 *   Where to store data in A
 */
template <typename T1, typename T2, int RANK>
void eig(tensor_t<T1, RANK> &out, tensor_t<T2, RANK - 1> &w,
         const tensor_t<T1, RANK> &a, cudaStream_t stream = 0,
         cusolverEigMode_t jobz = CUSOLVER_EIG_MODE_VECTOR,
         cublasFillMode_t uplo = CUBLAS_FILL_MODE_UPPER)
{
  /* Temporary WAR
     cuSolver doesn't support row-major layouts. Since we want to make the
     library appear as though everything is row-major, we take a performance hit
     to transpose in and out of the function. Eventually this may be fixed in
     cuSolver.
  */
  T1 *tp;
  matxAlloc(reinterpret_cast<void **>(&tp), a.Bytes(), MATX_ASYNC_DEVICE_MEMORY,
            stream);
  auto tv = matxDnSolver_t::TransposeCopy(tp, a, stream);

  // Get parameters required by these tensors
  auto params =
      matxDnEigSolverPlan_t<T1, T2, RANK>::GetEigParams(w, tv, jobz, uplo);

  // Get cache or new eigen plan if it doesn't exist
  auto ret = dneig_cache.Lookup(params);
  if (ret == std::nullopt) {
    auto tmp = new matxDnEigSolverPlan_t{w, tv, jobz, uplo};

    dneig_cache.Insert(params, static_cast<void *>(tmp));
    tmp->Exec(tv, w, tv, jobz, uplo, stream);
  }
  else {
    auto eig_type =
        static_cast<matxDnEigSolverPlan_t<T1, T2, RANK> *>(ret.value());
    eig_type->Exec(tv, w, tv, jobz, uplo, stream);
  }

  /* Temporary WAR
   * Copy and free async buffer for transpose */
  copy(out, tv.PermuteMatrix(), stream);
  matxFree(tp);
}

} // end namespace matx
