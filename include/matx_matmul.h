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
#include "matx_dim.h"
#include "matx_error.h"
#include "matx_tensor.h"
#include <cublasLt.h>

#if ENABLE_CUTLASS == 1
#include "cutlass/gemm/device/gemm.h"
#include "cutlass/gemm/device/gemm_batched.h"
#endif

#include <cstdio>
#include <numeric>

namespace matx {

/**
 * Defines a provider type for a GEMM. The provider is directly tied to the
 * underlying library used for the gemm, and certain providers provide
 * capabilities that others may not have.
 */
typedef enum {
  PROVIDER_TYPE_CUTLASS = 0,  ///< CUTLASS library
  PROVIDER_TYPE_CUBLASLT = 2, ///< cuBLASLt library
  PROVIDER_TYPE_AUTO,         ///< Automatically select

  PROVIDER_TYPE_SENTINEL ///< Sentinel value. Do not use
} MatXMatMulProvider_t;

typedef enum {
  MEM_ORDER_ROW_MAJOR = 0,
  MEM_ORDER_COL_MAJOR = 1,
} MemOrder_t;

union MatMulScaleType_t {
  float f32;
  double f64;
  float cf32[2];
  double cf64[2];
};

/**
 * Parameters needed to execute a GEMM. For the most part, these are very
 * similar to that of a standard GEMM call
 */
struct MatMulParams_t {
  index_t a_rows = 0;
  index_t a_cols = 0;
  index_t b_rows = 0;
  index_t b_cols = 0;
  index_t c_rows = 0;
  index_t c_cols = 0;
  index_t m = 0;
  index_t n = 0;
  index_t k = 0;
  index_t lda;
  index_t ldb;
  index_t ldc;
  int32_t batch; // Must be int32_t for cuBLASLt
  MatXMatMulProvider_t prov;
  cudaStream_t stream;
  MatXDataType_t dtype;
  cublasOperation_t opA;
  cublasOperation_t opB;
};

template <typename T1, typename T2, typename T3, int RANK,
          MatXMatMulProvider_t PROV = PROVIDER_TYPE_CUBLASLT>
class matxMatMulHandle_t {
public:
  /**
   * Construct a GEMM handle
   *
   * Creates a GEMM handle for the view shapes and provider type given. The view
   * shapres are used to create the underlying metadata used for the GEMM, so a
   * handle should only be used for views of identical sizes. The provider
   * chooses the underlying library used to perform the GEMM. Certain providers
   * have more features than others and may perform differently than others. At
   * the moment, it is recommended to try different providers for a given matrix
   * size until the optimal provider is found. Different providers may also be
   * used by creating multiple handles.
   *
   * @tparam T1
   *    Data type of C matrix
   * @tparam T2
   *    Data type of A matrix
   * @tparam T3
   *    Data type of B matrix
   * @tparam RANK
   *    Rank of A/B/C matrices
   * @tparam PROV
   *    Provider type chosen from MatXMatMulProvider_t type
   *
   * @param c
   *   C matrix view
   * @param a
   *   A matrix view
   * @param b
   *   B matrix view
   *
   */
#ifdef DOXYGEN_ONLY
  matxMatMulHandle_t(tensor_t c, tensor_t a, tensor_t b)
  {
#else
  matxMatMulHandle_t(tensor_t<T1, RANK> c, tensor_t<T2, RANK> a,
                     tensor_t<T3, RANK> b)
  {
#endif

    static_assert((PROV != PROVIDER_TYPE_CUTLASS) || ENABLE_CUTLASS,
                  "Must use -DCUTLASS_DIR in CMake to enable CUTLASS support");

    static_assert(RANK >= 2);
    MATX_ASSERT(a.Size(RANK - 1) == b.Size(RANK - 2), matxInvalidSize);
    MATX_ASSERT(c.Size(RANK - 1) == b.Size(RANK - 1), matxInvalidSize);
    MATX_ASSERT(c.Size(RANK - 2) == a.Size(RANK - 2), matxInvalidSize);

    // Ensure batch dimensions are equal
    for (int i = 0; i < RANK - 2; i++) {
      MATX_ASSERT(a.Size(i) == b.Size(i), matxInvalidSize);
      MATX_ASSERT(a.Size(i) == c.Size(i), matxInvalidSize);
    }

    // This must come before the things below to properly set class parameters
    params_ = GetGemmParams(c, a, b);

    // // Workspace buffer
    matxAlloc((void **)&workspace, workspaceSize, MATX_DEVICE_MEMORY);

    if constexpr (PROV == PROVIDER_TYPE_CUBLASLT) {
      ConfigureCublasLt();
    }
  }

  template <typename InputType>
  static void SetAlphaBeta([[maybe_unused]] char *const palpha,
                           [[maybe_unused]] char *const pbeta,
                           [[maybe_unused]] float const alpha,
                           [[maybe_unused]] float const beta)
  {
    // For now we don't give much flexibility on compute type/alpha
    if constexpr (std::is_same_v<InputType, cuda::std::complex<float>> ||
                  is_complex_half_v<InputType>) {
      cuComplex *calpha = reinterpret_cast<cuComplex *>(palpha);
      cuComplex *cbeta = reinterpret_cast<cuComplex *>(pbeta);
      *calpha = {alpha, 0};
      *cbeta = {beta, 0};
    }
    else if constexpr (std::is_same_v<InputType, cuda::std::complex<double>>) {
      cuDoubleComplex *dalpha = reinterpret_cast<cuDoubleComplex *>(palpha);
      cuDoubleComplex *dbeta = reinterpret_cast<cuDoubleComplex *>(pbeta);
      *dalpha = {alpha, 0};
      *dbeta = {beta, 0};
    }
    else if constexpr (std::is_same_v<InputType, double>) {
      double *dalpha = reinterpret_cast<double *>(palpha);
      double *dbeta = reinterpret_cast<double *>(pbeta);
      *dalpha = alpha;
      *dbeta = beta;
    }
    else if constexpr (is_matx_half_v<InputType> ||
                       std::is_same_v<InputType, float>) {
      float *talpha = reinterpret_cast<float *>(palpha);
      float *tbeta = reinterpret_cast<float *>(pbeta);
      *talpha = alpha;
      *tbeta = beta;
    }
    else {
      MATX_THROW(matxInvalidType, "Invalid type when deducing alpha/beta");
    }
  }

  static MatMulParams_t GetGemmParams(tensor_t<T1, RANK> &c,
                                      const tensor_t<T2, RANK> &a,
                                      const tensor_t<T3, RANK> &b)
  {
    MatMulParams_t params;
    params.dtype = TypeToInt<T1>();
    params.prov = PROV;

    // Batches
    params.batch = 1;

    // If we have a 3D or above tensor, the upper dims are batch dimensions. We
    // only batch on the third dimension and loop anything else above;
    if constexpr (RANK >= 3) {
      params.batch = static_cast<int32_t>(a.Size(RANK - 3));
    }

    tensor_t<T2, RANK> a_comp{a};
    tensor_t<T2, RANK> b_comp{b};
    tensor_t<T2, RANK> c_comp{c};

    // If the user wants C transposed (as a permuted view), we need the output
    // matrix to still be MxN in memory. The reason is the permuted view will
    // handle viewing it as an NxM. To accomplish this we use the identity C' =
    // B'A', so we swap A and B and permute them.
    if (c.Stride(RANK - 2) == 1 && c.Size(RANK - 1) != 1) {
      auto at = a.Permute({1, 0});
      auto bt = b.Permute({1, 0});
      a_comp.Shallow(bt);
      b_comp.Shallow(at);
      c_comp.Shallow(c.Permute({1, 0}));
    }

    if constexpr (PROV == PROVIDER_TYPE_CUBLASLT) {
      if (a_comp.Stride(RANK - 1) == 1) {
        params.opA = CUBLAS_OP_N;
        params.a_rows = a_comp.Size(RANK - 2);
        params.a_cols = a_comp.Size(RANK - 1);
        params.lda = a_comp.Stride(RANK - 2);
      }
      else if (a_comp.Stride(RANK - 2) == 1) {
        params.opA = CUBLAS_OP_T;
        params.a_rows = a_comp.Size(RANK - 1);
        params.a_cols = a_comp.Size(RANK - 2);
        params.lda = a_comp.Stride(RANK - 1);
      }

      if (b_comp.Stride(RANK - 1) == 1) {
        params.opB = CUBLAS_OP_N;
        params.b_rows = b_comp.Size(RANK - 2);
        params.b_cols = b_comp.Size(RANK - 1);
        params.ldb = b_comp.Stride(RANK - 2);
      }
      else if (b_comp.Stride(RANK - 2) == 1) {
        params.opB = CUBLAS_OP_T;
        params.b_rows = b_comp.Size(RANK - 1);
        params.b_cols = b_comp.Size(RANK - 2);
        params.ldb = b_comp.Stride(RANK - 1);
      }

      params.c_rows = params.a_rows;
      params.c_cols = params.b_cols;
      params.ldc = c_comp.Stride(RANK - 2);
    }
    else if constexpr (PROV == PROVIDER_TYPE_CUTLASS) {
      params.opA = CUBLAS_OP_N;
      params.opB = CUBLAS_OP_N;
      params.m = static_cast<int>(a_comp.Size(RANK - 2));
      params.n = static_cast<int>(b_comp.Size(RANK - 1));
      params.k =
          static_cast<int>(a_comp.Size(RANK - 1)); // Gemm Problem dimensions
      params.lda = static_cast<int>(a_comp.Stride(RANK - 2));
      params.ldb = static_cast<int>(b_comp.Stride(RANK - 2));
      params.ldc = static_cast<int>(c_comp.Stride(RANK - 2));
    }

    return params;
  }

  /**
   * GEMM handle destructor
   *
   * Destroys any helper data used for provider type and any workspace memory
   * created
   *
   */
  ~matxMatMulHandle_t()
  {
    matxFree(workspace);

    if constexpr (PROV == PROVIDER_TYPE_CUBLASLT) {
      cublasLtMatmulPreferenceDestroy(preference);
      cublasLtMatrixLayoutDestroy(Cdesc);
      cublasLtMatrixLayoutDestroy(Bdesc);
      cublasLtMatrixLayoutDestroy(Adesc);
      cublasLtMatmulDescDestroy(operationDesc);
    }
  }

/**
 * Execute a Matrix multiply (GEMM)
 *
 * Execute a matrix multiply operation on two rank=2 input tensors into an
 * output tensor. Using BLAS notation, tensor A has dimensions MxK, B is KxN,
 * and C is MxN. Concretely:
 *
 * \f$\textbf{C} = \alpha\textbf{A}\textbf{B} + \beta\textbf{C}\f$
 *
 * MatX will perform runtime checks ensuring that the dimension constraints are
 * met on all views. Unlike BLAS GEMMS, most parameters of the GEMM call are
 * deduced from the view itself; there is no need to specify dimensions or
 * transpose operations. MatX will attempt to perform the GEMM in the most
 * efficient way possible given the knowledge of the view.
 *
 * While GEMMs are strictly rank=2 functions, rank 3 and higher tensors may be
 * passed to this function, which has the effect of batching across the higher
 * dimensions.
 *
 * @note views being passed to matxGemm must not be permuted and must have a
 * contigous stride currently.
 *
 * @tparam T1
 *   Type of beta
 * @tparam T2
 *   Type of alpha
 * @param c
 *   Output tensor C
 * @param a
 *   Input tensor A
 * @param b
 *   Input tensor B
 * @param stream
 *   CUDA stream
 * @param alpha
 *   Alpha value
 * @param beta
 *   Beta value
 *
 */
#ifdef DOXYGEN_ONLY
  inline void Exec(tensor_t &c, const tensor_t &a,
                   const tensor_t &b, cudaStream_t stream, float alpha,
                   float beta)
  {
#else
  inline void Exec(tensor_t<T1, RANK> &c, const tensor_t<T2, RANK> &a,
                   const tensor_t<T3, RANK> &b, cudaStream_t stream,
                   float alpha = 1.0f, float beta = 0.0f)
  {
#endif

    // Reorder C/A to match cutlass API
    MatMulDispatchA(a, b, c, stream, alpha, beta);
  }

private:
  // Member variables
  cublasLtHandle_t ltHandle;
  cublasStatus_t ret = CUBLAS_STATUS_SUCCESS;

  // cuBLASLt variables;
  cublasHandle_t handle;
  cublasLtMatmulDesc_t operationDesc = nullptr;
  cublasLtMatrixLayout_t Adesc = nullptr;
  cublasLtMatrixLayout_t Bdesc = nullptr;
  cublasLtMatrixLayout_t Cdesc = nullptr;
  cublasLtMatmulPreference_t preference = nullptr;
  cublasLtMatrixTransformDesc_t transformDescI = nullptr;
  cublasLtMatrixTransformDesc_t transformDescO = nullptr;
  cublasLtMatrixLayout_t AtransformDesc = nullptr;
  cublasLtMatrixLayout_t BtransformDesc = nullptr;
  cublasLtMatrixLayout_t CtransformDesc = nullptr;
  cublasLtMatmulHeuristicResult_t heuristicResult = {};
  size_t workspaceSize = 1 << 25UL; // 16MB buffer suggested by cuBLAS team
  void *workspace = nullptr;
  MatMulParams_t params_;

  void ConfigureCublasLt()
  {
    MATX_ASSERT(cublasLtCreate(&ltHandle) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(cublasLtMatmulPreferenceCreate(&preference) ==
                    CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(cublasLtMatmulDescCreate(
                    &operationDesc, MatXTypeToCudaComputeType<T1>(),
                    MatXTypeToCudaType<T1>()) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    MATX_ASSERT(cublasLtMatmulPreferenceSetAttribute(
                    preference, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES,
                    &workspaceSize,
                    sizeof(workspaceSize)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    cublasLtOrder_t rowOrder = CUBLASLT_ORDER_ROW;
    int res;

    // A operation
    MATX_ASSERT(cublasLtMatmulDescSetAttribute(
                    operationDesc, CUBLASLT_MATMUL_DESC_TRANSA, &params_.opA,
                    sizeof(params_.opA)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    // B operation
    MATX_ASSERT(cublasLtMatmulDescSetAttribute(
                    operationDesc, CUBLASLT_MATMUL_DESC_TRANSB, &params_.opB,
                    sizeof(params_.opB)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    // Update this later when we're more flexible on compute type
    int32_t scaleType;
    if constexpr (std::is_same_v<T1, float> || is_matx_half_v<T1>) {
      scaleType = CUDA_R_32F;
    }
    else if constexpr (is_complex_half_v<T1> ||
                       std::is_same_v<T1, cuda::std::complex<float>>) {
      scaleType = CUDA_C_32F;
    }
    else if constexpr (std::is_same_v<T1, cuda::std::complex<double>>) {
      scaleType = CUDA_C_64F;
    }
    else {
      scaleType = CUDA_R_64F;
    }
    MATX_ASSERT(cublasLtMatmulDescSetAttribute(
                    operationDesc, CUBLASLT_MATMUL_DESC_SCALE_TYPE, &scaleType,
                    sizeof(scaleType)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    // Matrix layouts
    MATX_ASSERT(cublasLtMatrixLayoutCreate(
                    &Adesc, MatXTypeToCudaType<T2>(), params_.a_rows,
                    params_.a_cols, params_.lda) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    MATX_ASSERT(cublasLtMatrixLayoutCreate(
                    &Bdesc, MatXTypeToCudaType<T3>(), params_.b_rows,
                    params_.b_cols, params_.ldb) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(cublasLtMatrixLayoutCreate(
                    &Cdesc, MatXTypeToCudaType<T1>(), params_.c_rows,
                    params_.c_cols, params_.ldc) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    // Matrix data order
    MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                    Adesc, CUBLASLT_MATRIX_LAYOUT_ORDER, &rowOrder,
                    sizeof(rowOrder)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                    Bdesc, CUBLASLT_MATRIX_LAYOUT_ORDER, &rowOrder,
                    sizeof(rowOrder)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                    Cdesc, CUBLASLT_MATRIX_LAYOUT_ORDER, &rowOrder,
                    sizeof(rowOrder)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                    Adesc, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &params_.batch,
                    sizeof(params_.batch)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                    Bdesc, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &params_.batch,
                    sizeof(params_.batch)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                    Cdesc, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &params_.batch,
                    sizeof(params_.batch)) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);

    if constexpr (is_complex_half_v<T1> && is_complex_half_v<T2>) {
      size_t planarA = (params_.a_rows * params_.a_cols * sizeof(T1)) / 2;
      size_t planarB = (params_.b_rows * params_.b_cols * sizeof(T1)) / 2;
      size_t planarC = (params_.c_rows * params_.c_cols * sizeof(T1)) / 2;

      MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                      Adesc, CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET, &planarA,
                      sizeof(planarA)) == CUBLAS_STATUS_SUCCESS,
                  matxMatMulError);
      MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                      Bdesc, CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET, &planarB,
                      sizeof(planarB)) == CUBLAS_STATUS_SUCCESS,
                  matxMatMulError);
      MATX_ASSERT(cublasLtMatrixLayoutSetAttribute(
                      Cdesc, CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET, &planarC,
                      sizeof(planarC)) == CUBLAS_STATUS_SUCCESS,
                  matxMatMulError);
    }

    MATX_ASSERT(cublasLtMatmulAlgoGetHeuristic(ltHandle, operationDesc, Adesc,
                                               Bdesc, Cdesc, Cdesc, preference,
                                               1, &heuristicResult,
                                               &res) == CUBLAS_STATUS_SUCCESS,
                matxMatMulError);
    MATX_ASSERT(res > 0, matxMatMulError);
  }

  // TODO: Fix the unused parameters once we support mixes of col/row on cublas
  template <MemOrder_t OrderA, MemOrder_t OrderB, MemOrder_t OrderC>
  inline void
  MatMulLaunch(const tensor_t<T1, RANK> &a, const tensor_t<T2, RANK> &b,
               tensor_t<T3, RANK> &c, cudaStream_t stream,
               [[maybe_unused]] float alpha, [[maybe_unused]] float beta)
  {

    MATX_ASSERT(PROV < PROVIDER_TYPE_SENTINEL, matxInvalidParameter);

    if constexpr ((PROV == PROVIDER_TYPE_CUTLASS) &&
                  (is_complex_half_v<T1> || is_complex_half_v<T2>)) {
      MATX_THROW(matxInvalidType,
                 "CUTLASS does not support complex fp16/bf16 yet");
    }

    if constexpr ((is_complex_half_v<T1> && !is_complex_half_v<T2>) ||
                  (is_complex_half_v<T2> && !is_complex_half_v<T3>) ||
                  (is_complex_half_v<T1> && !is_complex_half_v<T3>)) {
      MATX_THROW(matxInvalidType,
                 "A/B/C types must all be half complex if any of them are");
    }

    // Make copies of each tensor in case we have to do a transformation before
    // the GEMM
    [[maybe_unused]] tensor_t<T1, RANK> a_adj { a };
    [[maybe_unused]] tensor_t<T2, RANK> b_adj { b };
    [[maybe_unused]] tensor_t<T3, RANK> c_adj { c };

    // If the tensors are complex half precision, we need to do a planar
    // transform since all libraries expect this format at the moment.
    if constexpr (is_complex_half_v<T1>) {
      typename T1::value_type *A;
      typename T2::value_type *B;
      typename T3::value_type *C;

      matxAlloc(reinterpret_cast<void **>(&A), a.Bytes(),
                MATX_ASYNC_DEVICE_MEMORY, stream);
      matxAlloc(reinterpret_cast<void **>(&B), b.Bytes(),
                MATX_ASYNC_DEVICE_MEMORY, stream);
      matxAlloc(reinterpret_cast<void **>(&C), c.Bytes(),
                MATX_ASYNC_DEVICE_MEMORY, stream);

      auto a_shape = a.Shape();
      a_shape.SetSize(a.Rank() - 2, a.Size(a.Rank() - 2) * 2);
      tensor_t<typename T1::value_type, RANK> a_planar(A, a_shape);

      auto b_shape = b.Shape();
      b_shape.SetSize(b.Rank() - 2, b.Size(b.Rank() - 2) * 2);
      tensor_t<typename T2::value_type, RANK> b_planar(B, b_shape);

      // Convert A/B to planar layout
      (a_planar = planar(a)).run(stream);
      (b_planar = planar(b)).run(stream);

      a_adj.SetData(reinterpret_cast<T1 *>(A));
      b_adj.SetData(reinterpret_cast<T2 *>(B));
      c_adj.SetData(reinterpret_cast<T3 *>(C));
    }

    // For cuBLASLt most of the parameters have already been set in the
    // configure stage
    if constexpr (PROV == PROVIDER_TYPE_CUBLASLT) {
      MatMulScaleType_t salpha, sbeta;
      memset(&salpha, 0, sizeof(salpha));
      memset(&sbeta, 0, sizeof(sbeta));

      if constexpr (std::is_same_v<T1, cuda::std::complex<float>> ||
                    is_complex_half_v<T1>) {
        salpha.cf32[0] = alpha;
        sbeta.cf32[0] = beta;
      }
      else if constexpr (std::is_same_v<T1, cuda::std::complex<double>>) {
        salpha.cf64[0] = alpha;
        sbeta.cf64[0] = beta;
      }
      else if constexpr (std::is_same_v<T1, float> || is_matx_half_v<T1>) {
        salpha.f32 = alpha;
        sbeta.f32 = beta;
      }
      else if constexpr (std::is_same_v<T1, double>) {
        salpha.f64 = alpha;
        sbeta.f64 = beta;
      }

      if constexpr (RANK <= 3) {
        auto res = cublasLtMatmul(
            ltHandle, operationDesc, &salpha, (void *)a_adj.Data(), Adesc,
            (void *)b_adj.Data(), Bdesc, &sbeta, (void *)c_adj.Data(), Cdesc,
            (void *)c_adj.Data(), Cdesc, &heuristicResult.algo, workspace,
            workspaceSize, stream);
        MATX_ASSERT(res == CUBLAS_STATUS_SUCCESS, matxMatMulError);
      }
      else {
        for (index_t i = 0; i < a.Size(0); i++) {
          MATX_ASSERT(
              cublasLtMatmul(
                  ltHandle, operationDesc, &salpha, (void *)&a_adj(i, 0, 0, 0),
                  Adesc, (void *)&b_adj(i, 0, 0, 0), Bdesc, &sbeta,
                  (void *)&c_adj(i, 0, 0, 0), Cdesc, (void *)&c_adj(i, 0, 0, 0),
                  Cdesc, &heuristicResult.algo, workspace, workspaceSize,
                  stream) == CUBLAS_STATUS_SUCCESS,
              matxMatMulError);
        }
      }
    }

    if constexpr (RANK == 2) {
      if constexpr (PROV == PROVIDER_TYPE_CUTLASS) {
#if ENABLE_CUTLASS
        using CutlassAOrder = std::conditional_t<OrderA == MEM_ORDER_ROW_MAJOR,
                                                 cutlass::layout::RowMajor,
                                                 cutlass::layout::ColumnMajor>;
        using CutlassBOrder = std::conditional_t<OrderB == MEM_ORDER_ROW_MAJOR,
                                                 cutlass::layout::RowMajor,
                                                 cutlass::layout::ColumnMajor>;
        using CutlassCOrder = std::conditional_t<OrderC == MEM_ORDER_ROW_MAJOR,
                                                 cutlass::layout::RowMajor,
                                                 cutlass::layout::ColumnMajor>;
        using CutlassGemm =
            cutlass::gemm::device::Gemm<T1,             // Data-type of A matrix
                                        CutlassAOrder,  // Layout of A matrix
                                        T2,             // Data-type of B matrix
                                        CutlassBOrder,  // Layout of B matrix
                                        T3,             // Data-type of C matrix
                                        CutlassCOrder>; // Layout of C matrix

        typename CutlassGemm::Arguments args(
            {static_cast<int>(params_.m), static_cast<int>(params_.n),
             static_cast<int>(params_.k)}, // Gemm Problem dimensions
            {a.Data(),
             static_cast<int>(params_.lda)}, // Tensor-ref for source matrix A
            {b.Data(),
             static_cast<int>(params_.ldb)}, // Tensor-ref for source matrix B
            {c.Data(),
             static_cast<int>(params_.ldc)}, // Tensor-ref for source matrix C
            {c.Data(),
             static_cast<int>(
                 params_.ldc)}, // Tensor-ref for destination matrix D (may be
                                // different memory than source C matrix)
            {alpha, beta});     // Scalars used in the Epilogue

        CutlassGemm gemm_operator;
        cutlass::Status status = gemm_operator(args, nullptr, stream);

        MATX_ASSERT(status == cutlass::Status::kSuccess, matxMatMulError);
#else
        MATX_THROW(matxNotSupported, "CUTLASS not enabled!");
#endif
      }
    }
    else {
      static_assert(RANK > 2);
#if ENABLE_CUTLASS
      using CutlassAOrder = std::conditional_t<OrderA == MEM_ORDER_ROW_MAJOR,
                                               cutlass::layout::RowMajor,
                                               cutlass::layout::ColumnMajor>;
      using CutlassBOrder = std::conditional_t<OrderB == MEM_ORDER_ROW_MAJOR,
                                               cutlass::layout::RowMajor,
                                               cutlass::layout::ColumnMajor>;
      using CutlassCOrder = std::conditional_t<OrderC == MEM_ORDER_ROW_MAJOR,
                                               cutlass::layout::RowMajor,
                                               cutlass::layout::ColumnMajor>;
      using CutlassGemm = cutlass::gemm::device::GemmBatched<
          T1,             // Data-type of A matrix
          CutlassAOrder,  // Layout of A matrix
          T2,             // Data-type of B matrix
          CutlassBOrder,  // Layout of B matrix
          T3,             // Data-type of C matrix
          CutlassCOrder>; // Layout of C matrix
#endif

      if constexpr (RANK == 3) {
        if constexpr (PROV == PROVIDER_TYPE_CUTLASS) {
#if ENABLE_CUTLASS
          typename CutlassGemm::Arguments args(
              {static_cast<int>(params_.m), static_cast<int>(params_.n),
               static_cast<int>(params_.k)}, // Gemm Problem dimensions
              {a_adj.Data(),
               static_cast<int>(params_.lda)}, // Tensor-ref for source matrix A
              a_adj.Stride(RANK - 3),          // Batch Stride A
              {b_adj.Data(),
               static_cast<int>(params_.ldb)}, // Tensor-ref for source matrix B
              b_adj.Stride(RANK - 3),          // Batch Stride B
              {c.Data(),
               static_cast<int>(params_.ldc)}, // Tensor-ref for source matrix C
              c_adj.Stride(RANK - 3),          // Batch Stride C
              {c_adj.Data(),
               static_cast<int>(
                   params_.ldc)}, // Tensor-ref for destination matrix D (may be
                                  // different memory than source C matrix)
              c_adj.Stride(RANK - 3), // Batch Stride C
              {alpha, beta},
              params_.batch // Batch Dimension
          );                // Scalars used in the Epilogue

          CutlassGemm gemm_operator;
          cutlass::Status status = gemm_operator(args, nullptr, stream);
          MATX_ASSERT(status == cutlass::Status::kSuccess, matxMatMulError);
#else
          MATX_THROW(matxNotSupported, "CUTLASS not enabled!");
#endif
        }
        else {
          MATX_ASSERT(PROV < PROVIDER_TYPE_SENTINEL, matxInvalidParameter);
        }
      }
      else { // Rank 4
        static_assert(RANK == 4);
        if constexpr (PROV == PROVIDER_TYPE_CUTLASS) {
#if ENABLE_CUTLASS
          // Loop over outer dimension and launch multiple batches
          for (index_t i = 0; i < a.Size(0); i++) {
            typename CutlassGemm::Arguments args(
                {static_cast<int>(params_.m), static_cast<int>(params_.n),
                 static_cast<int>(params_.k)}, // Gemm Problem dimensions
                {&a_adj(i, 0, 0, 0),
                 static_cast<int>(
                     params_.lda)},     // Tensor-ref for source matrix A
                a_adj.Stride(RANK - 3), // Batch Stride A
                {&b_adj(i, 0, 0, 0),
                 static_cast<int>(
                     params_.ldb)},     // Tensor-ref for source matrix B
                b_adj.Stride(RANK - 3), // Batch Stride B
                {&c_adj(i, 0, 0, 0),
                 static_cast<int>(
                     params_.ldc)},     // Tensor-ref for source matrix C
                c_adj.Stride(RANK - 3), // Batch Stride C
                {&c_adj(i, 0, 0, 0),
                 static_cast<int>(
                     params_.ldc)}, // Tensor-ref for destination matrix D (may
                                    // be different memory than source C matrix)
                c_adj.Stride(RANK - 3), // Batch Stride C
                {alpha, beta},
                params_.batch // Batch Dimension
            );                // Scalars used in the Epilogue

            CutlassGemm gemm_operator;
            cutlass::Status status = gemm_operator(args, nullptr, stream);
            MATX_ASSERT(status == cutlass::Status::kSuccess, matxMatMulError);
          }
#else
          MATX_THROW(matxNotSupported, "CUTLASS not enabled!");
#endif
        }
      }
    }

    // If the tensors are complex half precisions, we need to convert C back to
    // interleaved format and free all temporary buffers
    if constexpr (is_complex_half_v<T1>) {
      auto c_shape = c.Shape();
      c_shape.SetSize(c.Rank() - 2, c.Size(c.Rank() - 2) * 2);
      tensor_t<typename T3::value_type, RANK> c_planar(
          reinterpret_cast<typename T3::value_type *>(c_adj.Data()), c_shape);

      // Convert A/B to planar layout
      (c = interleaved(c_planar)).run(stream);
      matxFree(a_adj.Data());
      matxFree(b_adj.Data());
      matxFree(c_adj.Data());
    }
  }

  template <MemOrder_t OrderA, MemOrder_t OrderB>
  inline void MatMulDispatchC(const tensor_t<T1, RANK> &a,
                              const tensor_t<T2, RANK> &b,
                              tensor_t<T3, RANK> &c, cudaStream_t stream,
                              float alpha, float beta)
  {
    if (c.Stride(RANK - 1) == 1) {
      MatMulLaunch<OrderA, OrderB, MEM_ORDER_ROW_MAJOR>(a, b, c, stream, alpha,
                                                        beta);
    }
    else if (c.Stride(RANK - 2) == 1) {
      // Generate permutation
      uint32_t perm[RANK];
      std::iota(std::begin(perm), std::end(perm), 0);
      std::swap(perm[RANK - 1], perm[RANK - 2]);

      auto ct = c.Permute(perm);
      MatMulDispatchC<OrderA, MEM_ORDER_COL_MAJOR>(a, b, ct, stream, alpha,
                                                   beta);
    }
    else {
      MATX_THROW(matxNotSupported,
                 "Matrix multiply on Affine Matrix Not supported");
    }
  };

  template <MemOrder_t OrderA>
  inline void MatMulDispatchB(const tensor_t<T1, RANK> &a,
                              const tensor_t<T2, RANK> &b,
                              tensor_t<T3, RANK> &c, cudaStream_t stream,
                              float alpha, float beta)
  {
    if (b.Stride(RANK - 1) == 1) {
      MatMulDispatchC<OrderA, MEM_ORDER_ROW_MAJOR>(a, b, c, stream, alpha,
                                                   beta);
    }
    else if (b.Stride(RANK - 2) == 1) {
      // Generate permutation
      uint32_t perm[RANK];
      std::iota(std::begin(perm), std::end(perm), 0);
      std::swap(perm[RANK - 1], perm[RANK - 2]);

      auto bt = b.Permute(perm);
      MatMulDispatchC<OrderA, MEM_ORDER_COL_MAJOR>(a, bt, c, stream, alpha,
                                                   beta);
    }
    else {
      MATX_THROW(matxNotSupported,
                 "Matrix multiply on Affine Matrix Not supported");
    }
  }

  inline void MatMulDispatchA(const tensor_t<T1, RANK> &a,
                              const tensor_t<T2, RANK> &b,
                              tensor_t<T3, RANK> &c, cudaStream_t stream,
                              float alpha, float beta)
  {
    if (a.Stride(RANK - 1) == 1) {
      MatMulDispatchB<MEM_ORDER_ROW_MAJOR>(a, b, c, stream, alpha, beta);
    }
    else if (a.Stride(RANK - 2) == 1) {
      // Generate permutation
      uint32_t perm[RANK];
      std::iota(std::begin(perm), std::end(perm), 0);
      std::swap(perm[RANK - 1], perm[RANK - 2]);

      auto at = a.Permute(perm);
      MatMulDispatchB<MEM_ORDER_COL_MAJOR>(at, b, c, stream, alpha, beta);
    }
    else {
      MATX_THROW(matxNotSupported,
                 "Matrix multiply on Affine Matrix Not supported");
    }
  }
};

/**
 * Crude hash on GEMM to get a reasonably good delta for collisions. This
 * doesn't need to be perfect, but fast enough to not slow down lookups, and
 * different enough so the common GEMM parameters change
 */
struct MatMulParamsKeyHash {
  std::size_t operator()(const MatMulParams_t &k) const noexcept
  {
    return std::hash<index_t>()(k.m) + std::hash<index_t>()(k.n) +
           std::hash<index_t>()(k.k) + std::hash<index_t>()(k.batch) +
           std::hash<index_t>()(k.prov) +
           std::hash<index_t>()((size_t)k.stream);
  }
};

/**
 * Test GEMM parameters for equality. Unlike the hash, all parameters must
 * match.
 */
struct MatMulParamsKeyEq {
  bool operator()(const MatMulParams_t &l, const MatMulParams_t &t) const
      noexcept
  {
    return l.m == t.m && l.n == t.n && l.k == t.k && l.a_rows == t.a_rows &&
           l.b_rows == t.b_rows && l.c_rows == t.c_rows &&
           l.a_cols == t.a_cols && l.b_cols == t.b_cols &&
           l.c_cols == t.c_cols && l.stream == t.stream && l.lda == t.lda &&
           l.ldb == t.ldb && l.ldc == t.ldc && l.batch == t.batch &&
           l.prov == t.prov && l.dtype == t.dtype && l.opA == t.opA &&
           l.opB == t.opB;
  }
};

// Static caches of GEMMs
static matxCache_t<MatMulParams_t, MatMulParamsKeyHash, MatMulParamsKeyEq>
    gemm_cache;

/**
 * Run a GEMM without a plan
 *
 * Creates a new GEMM plan in the cache if none exists, and uses that to execute
 * the GEMM. This function is preferred over creating a plan directly for both
 * efficiency and simpler code. Since it only uses the signature of the GEMM to
 * decide if a plan is cached, it may be able to reused plans for different
 * A/B/C matrices as long as they were configured with the same dimensions.
 *
 * @tparam T1
 *    Data type of C matrix
 * @tparam T2
 *    Data type of A matrix
 * @tparam T3
 *    Data type of B matrix
 * @tparam RANK
 *    Rank of A/B/C matrices
 * @tparam PROV
 *    Provider type chosen from MatXMatMulProvider_t type
 *
 * @param c
 *   C matrix view
 * @param a
 *   A matrix view
 * @param b
 *   B matrix view
 * @param stream
 *   CUDA stream
 * @param alpha
 *   Scalar multiplier to apply to matrix A
 * @param beta
 *   Scalar multiplier to apply to matrix C on input
 */
template <typename T1, typename T2, typename T3, int RANK,
          MatXMatMulProvider_t PROV = PROVIDER_TYPE_CUBLASLT>
void matmul(tensor_t<T1, RANK> c, const tensor_t<T2, RANK> &a,
            const tensor_t<T3, RANK> &b, cudaStream_t stream = 0,
            float alpha = 1.0, float beta = 0.0)
{
  // Get parameters required by these tensors
  auto params =
      matxMatMulHandle_t<T1, T2, T3, RANK, PROV>::GetGemmParams(c, a, b);
  params.stream = stream;

  // Get cache or new GEMM plan if it doesn't exist
  auto ret = gemm_cache.Lookup(params);
  if (ret == std::nullopt) {
    auto tmp = new matxMatMulHandle_t<T1, T2, T3, RANK, PROV>{c, a, b};
    gemm_cache.Insert(params, static_cast<void *>(tmp));

    // Set the stream on this plan once on creation
    tmp->Exec(c, a, b, stream, alpha, beta);
  }
  else {
    auto gemm_type =
        static_cast<matxMatMulHandle_t<T1, T2, T3, RANK, PROV> *>(ret.value());
    gemm_type->Exec(c, a, b, stream, alpha, beta);
  }
}

} // end namespace matx
