
#pragma once

#include "cuComplex.h"
#include "matx_type_utils.h"
#include <complex>
#include <cuda.h>
#include <iomanip>
#include <stdint.h>
#include <stdio.h>
#include <vector>

namespace matx {

// Tile dims for one block
#define TILE_DIM 32

/* Out of place. Adapted from:
   https://developer.nvidia.com/blog/efficient-matrix-transpose-cuda-cc/. Works
   for both square and rectangular matrices. */
template <typename T, int RANK>
__global__ void transpose_kernel_oop(tensor_t<T, RANK> out,
                                     const tensor_t<T, RANK> in)
{
  extern __shared__ float
      tile[]; // Need to swap complex types also, so cast when needed
  T *shm_tile = reinterpret_cast<T *>(&tile[0]);
  index_t x = blockIdx.x * TILE_DIM + threadIdx.x;
  index_t y = blockIdx.y * TILE_DIM + threadIdx.y;

  if constexpr (RANK == 2) {
    if (x < in.Size(RANK - 1) && y < in.Size(RANK - 2)) {
      shm_tile[threadIdx.y * (TILE_DIM + 1) + threadIdx.x] = in(y, x);
    }

    __syncthreads();

    x = blockIdx.y * TILE_DIM + threadIdx.x;
    y = blockIdx.x * TILE_DIM + threadIdx.y;

    if (y < out.Size(RANK - 2) && x < out.Size(RANK - 1)) {
      out(y, x) = shm_tile[threadIdx.x * (TILE_DIM + 1) + threadIdx.y];
    }
  }
  else if constexpr (RANK == 3) {
    index_t z = blockIdx.z;

    if (x < in.Size(RANK - 1) && y < in.Size(RANK - 2)) {
      shm_tile[threadIdx.y * (TILE_DIM + 1) + threadIdx.x] = in(z, y, x);
    }

    __syncthreads();

    x = blockIdx.y * TILE_DIM + threadIdx.x;
    y = blockIdx.x * TILE_DIM + threadIdx.y;

    if (y < out.Size(RANK - 2) && x < out.Size(RANK - 1)) {
      out(z, y, x) = shm_tile[threadIdx.x * (TILE_DIM + 1) + threadIdx.y];
    }
  }
  else if constexpr (RANK == 4) {
    index_t z = blockIdx.z % in.Size(1);
    index_t w = blockIdx.z / in.Size(1);

    if (x < in.Size(RANK - 1) && y < in.Size(RANK - 2)) {
      shm_tile[threadIdx.y * (TILE_DIM + 1) + threadIdx.x] = in(w, z, y, x);
    }

    __syncthreads();

    x = blockIdx.y * TILE_DIM + threadIdx.x;
    y = blockIdx.x * TILE_DIM + threadIdx.y;

    if (y < out.Size(RANK - 2) && x < out.Size(RANK - 1)) {
      out(w, z, y, x) = shm_tile[threadIdx.x * (TILE_DIM + 1) + threadIdx.y];
    }
  }
}

}; // namespace matx
