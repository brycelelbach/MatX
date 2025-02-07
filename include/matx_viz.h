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

#include "matx_error.h"
#include "matx_dim.h"
#include "matx_tensor.h"
#include "matx_pybind.h"
#include <cstdio>
#include <numeric>

namespace matx {
namespace viz{

using namespace pybind11::literals;

/**
 * Create a line plot from a tensor view
 *
 * Generates either an HTML page or launches a browser displaying a line plot. The line order
 * uses columns to generate each line of data, and rows are separate lines.
 *
 * @tparam T
 *   Type of tensor
 * @tparam RANK
 *   Rank of tensor
 * 
 * @param ten
 *   Input tensor
 * @param title
 *   Title of plot
 * @param xlabel
 *   X axis label
 * @param ylabel
 *   Y axis label
 * @param out_fname
 *   Output file name. If blank, a new window will open with the plot in a browser window
 */
template <typename T, int RANK>
void line(const tensor_t<T, RANK> &ten, 
          const std::string &title,
          const std::string &xlabel,
          const std::string &ylabel,
          const std::string &out_fname = "") {
  std::unique_ptr<MatXPybind> pb;  
  auto px = py::module_::import("plotly.express");   
  auto np = py::module_::import("numpy");   

  auto np_ten = MatXPybind::GetEmptyNumpy(ten);
  pb->TensorViewToNumpy(np_ten, ten);

  auto labels = py::dict("index"_a=xlabel, "value"_a=ylabel);
  auto fig = px.attr("line")(np_ten, "labels"_a = labels, "title"_a = title);  
  if (out_fname == "") {
    fig.attr("show")();
  }
  else {
    fig.attr("write_html")(out_fname);
  }
}

/**
 * Create a scatter plot from a tensor view
 *
 * Generates either an HTML page or launches a browser displaying a scatter plot from X/Y values. The
 * two input tensors must be rank 1, and must match in size.
 *
 * @tparam T
 *   Type of tensor
 * @tparam RANK
 *   Rank of tensor
 * 
 * @param x
 *   X input tensor
 * @param y
 *   Y input tensor
 * @param title
 *   Title of plot
 * @param xlabel
 *   X axis label
 * @param ylabel
 *   Y axis label
 * @param out_fname
 *   Output file name. If blank, a new window will open with the plot in a browser window
 */
template <typename T, int RANK>
void scatter(const tensor_t<T, RANK> &x, 
          const tensor_t<T, RANK> &y,
          const std::string &title,
          const std::string &xlabel,
          const std::string &ylabel,
          const std::string &out_fname = "") {
  std::unique_ptr<MatXPybind> pb;  
  auto px = py::module_::import("plotly.express");   
  auto np = py::module_::import("numpy");   

  MATX_ASSERT(RANK == 1, matxInvalidDim);
  MATX_ASSERT(x.Size(0) == y.Size(0), matxInvalidDim);

  auto np_x_ten = MatXPybind::GetEmptyNumpy(x);
  auto np_y_ten = MatXPybind::GetEmptyNumpy(y);
  pb->TensorViewToNumpy(np_x_ten, x);
  pb->TensorViewToNumpy(np_y_ten, y);  

  auto labels = py::dict("index"_a=xlabel, "value"_a=ylabel);
  auto fig = px.attr("scatter")("x"_a=np_x_ten, "y"_a=np_y_ten,"labels"_a = labels, "title"_a = title);  
  if (out_fname == "") {
    fig.attr("show")();
  }
  else {
    fig.attr("write_html")(out_fname);
  }
}

/**
 * Create a bar plot from a tensor view using increasing X values
 *
 * Generates either an HTML page or launches a browser displaying a bar plot from X values. The
 * input tensor must be rank 1 currently.
 *
 * @tparam T
 *   Type of tensor
 * @tparam RANK
 *   Rank of tensor
 * 
 * @param y
 *   Y input tensor
 * @param title
 *   Title of plot
 * @param ylabel
 *   Y axis label
 * @param out_fname
 *   Output file name. If blank, a new window will open with the plot in a browser window
 */
template <typename T, int RANK>
void bar(const tensor_t<T, RANK> &y, 
          const std::string &title,
          const std::string &ylabel,
          const std::string &out_fname = "") {
  std::unique_ptr<MatXPybind> pb;  
  auto px = py::module_::import("plotly.express");   
  auto np = py::module_::import("numpy");   

  MATX_ASSERT(RANK == 1, matxInvalidDim);

  auto np_y_ten = MatXPybind::GetEmptyNumpy(y);
  pb->TensorViewToNumpy(np_y_ten, y);

  auto labels = py::dict("y"_a=ylabel);
  auto fig = px.attr("bar")("y"_a=np_y_ten, "labels"_a = labels, "title"_a = title);  
  
  if (out_fname == "") {
    fig.attr("show")();
  }
  else {
    fig.attr("write_html")(out_fname);
  }
}

/**
 * Create a bar plot from a tensor view using both X and Y values
 *
 * Generates either an HTML page or launches a browser displaying a bar plot from X/Y values. The
 * input tensors must be rank 1 currently.
 *
 * @tparam T
 *   Type of tensor
 * @tparam RANK
 *   Rank of tensor
 * 
 * @param x
 *   X input tensor
 * @param y
 *   Y input tensor
 * @param title
 *   Title of plot
 * @param xlabel
 *   X axis label
 * @param ylabel
 *   Y axis label
 * @param out_fname
 *   Output file name. If blank, a new window will open with the plot in a browser window
 */
template <typename T, int RANK>
void bar( const tensor_t<T, RANK> &x, 
          const tensor_t<T, RANK> &y, 
          const std::string &title,
          const std::string &xlabel,
          const std::string &ylabel,
          const std::string &out_fname = "") {
  std::unique_ptr<MatXPybind> pb;  
  auto px = py::module_::import("plotly.express");   
  auto np = py::module_::import("numpy");   

  MATX_ASSERT(RANK == 1, matxInvalidDim);
  MATX_ASSERT(x.Size(0) == y.Size(0), matxInvalidDim);

  auto np_x_ten = MatXPybind::GetEmptyNumpy(x);
  auto np_y_ten = MatXPybind::GetEmptyNumpy(y);
  pb->TensorViewToNumpy(np_x_ten, x);
  pb->TensorViewToNumpy(np_y_ten, y);

  auto labels = py::dict("x"_a=xlabel, "y"_a=ylabel);
  auto fig = px.attr("bar")("x"_a=np_x_ten, "y"_a=np_y_ten,"labels"_a = labels, "title"_a = title);  
  if (out_fname == "") {
    fig.attr("show")();
  }
  else {
    fig.attr("write_html")(out_fname);
  }
}


/**
 * Create a contour plot from a tensor view
 *
 * Generates either an HTML page or launches a browser displaying a contour plot. Three tensor
 * are required for a contour plot for the values of each axis and the Z value at each point. The
 * Z tensor must be one rank higher than the x/y tensors with the outer dimensions matching X/Y.
 *
 * @tparam T
 *   Type of tensor
 * @tparam RANK
 *   Rank of tensor
 * 
 * @param x
 *   Tensor with X axis points
 * @param y
 *   Tensor with Y axis points
 * @param z
 *   Tensor with Z axis points
 * @param out_fname
 *   Output file name. If blank, a new window will open with the plot in a browser window
 */
template <typename T, int RANK>
void contour( const tensor_t<T, RANK-1> &x, 
              const tensor_t<T, RANK-1> &y, 
              const tensor_t<T, RANK> &z,
              const std::string &out_fname = "") {
  std::unique_ptr<MatXPybind> pb;  
  auto go = py::module_::import("plotly.graph_objects");   
  auto np = py::module_::import("numpy");   

  auto np_x_ten = MatXPybind::GetEmptyNumpy(x);
  auto np_y_ten = MatXPybind::GetEmptyNumpy(y);
  auto np_z_ten = MatXPybind::GetEmptyNumpy(z);
  pb->TensorViewToNumpy(np_x_ten, x);
  pb->TensorViewToNumpy(np_y_ten, y);
  pb->TensorViewToNumpy(np_z_ten, z);

  auto data = go.attr("Contour")("z"_a = np_z_ten, "y"_a = np_y_ten, "x"_a = np_x_ten);
  auto fig = go.attr("Figure")("data"_a = data);
  if (out_fname == "") {
    fig.attr("show")();
  }
  else {
    fig.attr("write_html")(out_fname);
  }  
}


}; // viz
}; // matx