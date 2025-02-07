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

#include <cstdint>
#include <type_traits>

#include "matx_allocator.h"
#include "matx_error.h"
#include "matx_shape.h"
#include "matx_tensor.h"
#include "matx_type_utils.h"

namespace matx {
namespace signal {

/* Operator for perfoming the 2*exp(-j*pi*k/(2N)) part of the DCT */
template <typename O, typename I> class dctOp : public BaseOp<dctOp<O, I>> {
private:
  O out_;
  I in_;
  index_t N_;

public:
  dctOp(O out, I in, index_t N) : out_(out), in_(in), N_(N) {}

  __device__ inline void operator()(index_t idx)
  {
    out_(idx) =
        in_(idx).real() * 2.0f * cuda::std::cos(-1 * M_PI * idx / (2.0 * N_)) -
        in_(idx).imag() * 2.0f * cuda::std::sin(-1 * M_PI * idx / (2.0 * N_));
  }

  __host__ __device__ inline index_t Size(uint32_t i) const
  {
    return out_.Size(i);
  }

  static inline constexpr __host__ __device__ int32_t Rank()
  {
    return O::Rank();
  }
};

/**
 * Discrete Cosine Transform
 *
 * Computes the DCT of input sequence "in". The input and output ranks must be
 *1, and the sizes must match.
 *
 * @tparam T
 *   Input data type
 * @tparam RANK
 *   Rank of input and output tensor. Must be 1
 *
 * @param out
 *   Output tensor
 * @param in
 *   Input tensor
 * @param stream
 *   CUDA stream
 *
 **/
template <typename T, int RANK>
void dct(tensor_t<T, RANK> &out, tensor_t<T, RANK> &in,
         const cudaStream_t stream = 0)
{
  MATX_ASSERT(RANK == 1, matxInvalidDim);
  index_t N = in.Size(RANK - 1);

  tensor_t<cuda::std::complex<T>, 1> tmp{{N + 1}};
  fft(tmp, in);
  auto s = tmp.Slice({0}, {N});
  dctOp(out, s, N).run(stream);
}

}; // namespace signal
}; // namespace matx