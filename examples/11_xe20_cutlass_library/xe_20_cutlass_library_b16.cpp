/***************************************************************************************************
 * Copyright (C) 2025 Intel Corporation, All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***************************************************************************************************/



#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "cute/tensor.hpp"
#include "cutlass/cutlass.h"
#include "cutlass/numeric_types.h"
#include "cutlass/tensor_ref.h"
#include "cutlass/util/host_tensor.h"
#include "cutlass/util/reference/host/tensor_fill.h"
#include "cutlass/util/reference/device/tensor_fill.h"
#include "cutlass/util/device_memory.h"

#include "cutlass/gemm/gemm.h"
#include "cutlass/gemm/device/gemm_universal.h"
#include "cutlass/gemm/device/gemm_universal_adapter.h"
#include "cutlass/gemm/kernel/gemm_universal.hpp"
//#include "cutlass/gemm/device/gemm_sparse.h"
#include "cutlass/gemm/collective/collective_builder.hpp"
#include "cutlass/epilogue/collective/collective_builder.hpp"
#include "cutlass/epilogue/collective/default_epilogue.hpp"
#include "cutlass/epilogue/thread/linear_combination.h"
#include "cutlass/epilogue/thread/activation.h"
#include "cutlass/gemm/dispatch_policy.hpp"
#include "cutlass/gemm/kernel/tile_scheduler.hpp"
#include "cutlass/tensor_ref.h"
#include "cutlass/util/distribution.h"
#include "cutlass/util/packed_stride.hpp"
#include "cutlass/util/tensor_view_io.h"


// We compile all models with -fvisibility=hidden. Any symbols that need to be
// exposed in the final shared library must be declared with PT_EXPORT to make
// them visible.
#ifdef __GNUC__ // Applies to any compiler with GNU extensions (clang and g++)
#define PT_EXPORT __attribute__((__visibility__("default")))
#else
#ifdef _WIN32
#define PT_EXPORT __declspec(dllexport)
#else
#define PT_EXPORT
#endif
#endif

using namespace cute;
#define CUTLASS_CHECK(status)                                                      \
{                                                                                  \
  cutlass::Status error = status;                                                  \
  if (error != cutlass::Status::kSuccess) {                                        \
    auto msg = std::string("[") + __FILE__ + "] Got cutlass error: " +             \
        cutlassGetStatusString(error) + " at: " + std::to_string(__LINE__);        \
    throw std::runtime_error(msg);                                                 \
  }                                                                                \
}

// Used as pass-through functor in EVT just for type casting / rounding
template <typename T>
struct identity_op {
  CUTLASS_HOST_DEVICE
  T operator()(T val) const { return val; }
};



using cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_epilogue =
  typename cutlass::epilogue::collective::CollectiveBuilder<
    cutlass::arch::Xe20, cutlass::arch::OpClassTensorOp,
    cute::Shape<cute::_256, cute::_256, cute::_32>,
    cute::Shape<cute::_1, cute::_1, cute::_1>,
    cutlass::epilogue::collective::EpilogueTileAuto,
    float, float,
    float, cutlass::layout::RowMajor, 4,
    float, cutlass::layout::RowMajor, 4,
    cutlass::epilogue::collective::EpilogueScheduleAuto,
    cutlass::epilogue::fusion::LinearCombination<
      float,
      float,
      float,
      float
    >
  >::CollectiveOp;

using cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_mainloop =
  typename cutlass::gemm::collective::CollectiveBuilder<
    cutlass::arch::Xe20, cutlass::arch::OpClassTensorOp,
    cutlass::bfloat16_t, cutlass::layout::ColumnMajor, 8,
    cutlass::bfloat16_t, cutlass::layout::ColumnMajor, 8,
    float,
    cute::Shape<cute::_256, cute::_256, cute::_32>,
    cute::Shape<cute::_1, cute::_1, cute::_1>,
    cutlass::gemm::collective::StageCountAuto,
    cutlass::gemm::collective::KernelScheduleAuto
  >::CollectiveOp;

// Gemm operator cutlass3x_xe11_tensorop_gemm_bf16_128x256_16x0_tn_align2
using cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_base = cutlass::gemm::kernel::GemmUniversal<
    cute::Shape<int,int,int,int>,
    cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_mainloop,
    cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_epilogue,
    cutlass::gemm::PersistentScheduler>;

// Define named type
struct cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8 :
public cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_base { };


  using cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_device_type = cutlass::gemm::device::GemmUniversalAdapter<cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8>;

// When workspace_size is not a nullptr, populates requested workspace_size and returns.
// Otherwise, computes the Gemm kernel using the given workspace ptr.
extern "C" {
PT_EXPORT int sycl_tla_gemm_xe20_bf16(const uint16_t* X, const uint16_t* W, uint16_t* Y, const int M, const int N, const int K, const int B, const int lda, const int ldb, const int ldc, const int ldd, const int X_offset, const int W_offset, const int Y_offset, const uint8_t swizzle, size_t* workspace_size, uint8_t* workspace, sycl::queue* stream) {
  try {
  using ElementComputeEpilogue = cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_device_type::ElementAccumulator;
  using coord_t = cutlass::gemm::GemmCoord::Index;
  static cutlass::KernelHardwareInfo hw_info;
  if (hw_info.sm_count == 0) {
    hw_info.sm_count = cutlass::KernelHardwareInfo::query_device_multiprocessor_count(0);
    CUTLASS_TRACE_HOST("Query result for SM count per device: " << hw_info.sm_count);
  }

  // Initialize GemmUniversal3xInstance arguments using constructor
  cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_device_type::Arguments arguments{
    cutlass::gemm::GemmUniversalMode::kGemm,  // GemmUniversalMode mode
    {
      static_cast<coord_t>(M),
      static_cast<coord_t>(N),
      static_cast<coord_t>(K),
      static_cast<coord_t>(B)
    }, // ProblemShape problem_shape
    {
      (cutlass::bfloat16_t*)(X + X_offset),  // ElementA const* ptr_A
      cute::make_tuple(cute::Int<1>{}, int64_t(lda), int64_t(0)),  // StrideA dA (column-major: stride_m=1, stride_n=lda, batch=0)
      (cutlass::bfloat16_t*)(W + W_offset),  // ElementB const* ptr_B
      cute::make_tuple(int64_t(ldb), cute::Int<1>{}, int64_t(0)),  // StrideB dB (column-major: stride_m=ldb, stride_n=1, batch=0)
    },  // MainloopArguments mainloop

    // see https://tinyurl.com/4rk89z48
    {
      {ElementComputeEpilogue(1), ElementComputeEpilogue(0)},  // thread, typename FusionCallbacks::Arguments ( EVT ) or ThreadEpilogueOp::Params (non-EVT )
      nullptr,  // ElementC const* ptr_C
      cute::make_tuple(int64_t(0), cute::Int<1>{}, int64_t(0)),  // StrideC dC (row-major: stride_m, stride_n=1, batch=0)
      (float*)(Y + Y_offset),  // ElementD ptr_D (output is float, not bfloat16)
      cute::make_tuple(int64_t(ldd), cute::Int<1>{}, int64_t(0)),  // StrideD dD (row-major: stride_m=ldd, stride_n=1, batch=0)
    },  // EpilogueArguments epilogue,
    hw_info
  };
  arguments.scheduler.max_swizzle_size = swizzle;
  cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8_device_type gemm_op;
  if (workspace_size) {
    *workspace_size = gemm_op.get_workspace_size(arguments);
    return 0;
  }
  // check for null pointers after workspace size, since querying workspace size doesn't require valid data pointers
#ifndef CUTLASS_BACKEND_DISABLE_CHECKS
  {
    auto status = gemm_op.can_implement(arguments);
    CUTLASS_CHECK(status);
  }
#endif
#ifdef CUTLASS_DEBUG_TRACE_LEVEL
#if CUTLASS_DEBUG_TRACE_LEVEL == 1
  {
    // Print the maximum number of active blocks per SM for the kernel if CUTLASS_DEBUG_TRACE_LEVEL == 1
    // we don't need a print statement, it's happening inside the function.
    gemm_op.maximum_active_blocks();
  }
#endif
#endif
  {
    auto status = gemm_op.initialize(arguments, workspace, stream);
    CUTLASS_CHECK(status);
  }
  {
    auto status = gemm_op(stream);
    CUTLASS_CHECK(status);
  }
  }
  catch (std::exception& e) {
    std::cerr << "Runtime error: " << e.what() << std::endl;
    return -1;
  }
  catch (...) {
    return -1;
  }
  return 0;
}
}

// configuration name: cutlass3x_xe20_tensorop_gemm_bf16_256x256_32x0_nn_align8