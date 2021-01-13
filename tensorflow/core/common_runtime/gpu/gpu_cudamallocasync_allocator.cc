/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifdef GOOGLE_CUDA
#include "tensorflow/stream_executor/cuda/cuda_activation.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#endif  // GOOGLE_CUDA

#include "tensorflow/core/common_runtime/gpu/gpu_cudamallocasync_allocator.h"

#include "tensorflow/core/common_runtime/gpu/gpu_id.h"
#include "tensorflow/core/common_runtime/gpu/gpu_id_utils.h"
#include "tensorflow/core/common_runtime/gpu/gpu_init.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/platform/stream_executor.h"
#include "tensorflow/core/util/env_var.h"

namespace tensorflow {

GPUcudaMallocAsyncAllocator::GPUcudaMallocAsyncAllocator(
    Allocator* allocator, PlatformGpuId platform_gpu_id, size_t pool_size,
    bool reserve_memory)
    : base_allocator_(allocator), cuda_stream_(nullptr),
      name_(absl::StrCat("gpu_async_", platform_gpu_id.value())) {
  stream_exec_ =
      GpuIdUtil::ExecutorForPlatformGpuId(platform_gpu_id).ValueOrDie();

#if CUDA_VERSION < 11020
  LOG(ERROR) << "TF_GPU_ALLOCATOR=cuda_malloc_async need CUDA 11.2 or higher to compile.";
#else

  // WAR an CUDA 11.2 driver bug for multiple-GPU. It currently
  // request that the context on GPU 0 is initialized. Which isn't the
  // case for TF+horovod.
  if (platform_gpu_id.value() > 0) {
    auto stream_0 = GpuIdUtil::ExecutorForPlatformGpuId(PlatformGpuId(0)).ValueOrDie();
    se::cuda::ScopedActivateExecutorContext scoped_activation{stream_0};
    void* ptr;
    cudaMalloc(&ptr, 1024);
    cudaFree(ptr);
  }

  se::cuda::ScopedActivateExecutorContext scoped_activation{stream_exec_};
  int cuda_malloc_async_supported;
  cudaDeviceGetAttribute(&cuda_malloc_async_supported,
                         cudaDevAttrMemoryPoolsSupported,
                         platform_gpu_id.value());
  if (!cuda_malloc_async_supported) {
    LOG(ERROR) << "TF_GPU_ALLOCATOR=cuda_malloc_async isn't currently supported."
               << " Possible causes: device not supported, driver too old, "
               << " OS not supported, CUDA version too old.";
  }

  cudaError_t cerr = cudaStreamCreate(&cuda_stream_);
  if (cerr != cudaSuccess) {
    LOG(ERROR) << "could not allocate CUDA stream for context : "
               << cudaGetErrorString(cerr);
  }

  cerr = cudaDeviceGetDefaultMemPool(&pool_, platform_gpu_id.value());
  if (cerr != cudaSuccess) {
    LOG(ERROR) << "could not get the default CUDA pool : "
               << cudaGetErrorString(cerr);
  }
  VLOG(1) << Name() << " CudaMallocAsync initialized on platform: "
          << platform_gpu_id.value() << " with pool size of: "
          << pool_size << " this ptr: " << this;
  cerr = cudaMemPoolSetAttribute(pool_, cudaMemPoolAttrReleaseThreshold,
                                 (void*)&pool_size);
  stats_.bytes_limit = static_cast<int64>(pool_size);
  if (cerr != cudaSuccess) {
    LOG(ERROR) << "could not set the default CUDA pool memory threshold : "
               << cudaGetErrorString(cerr);
  }

  // If in TF_DETERMINISTIC_OPS is set, then make the allocator behave
  // determistically.
  bool deterministic_ops = false;
  TF_CHECK_OK(tensorflow::ReadBoolFromEnvVar("TF_DETERMINISTIC_OPS",
                                             /*default_val=*/false,
                                             &deterministic_ops));
  if (deterministic_ops) {
    cudaMemPoolSetAttribute(pool_, cudaMemPoolReuseAllowOpportunistic, 0);
    cudaMemPoolSetAttribute(pool_, cudaMemPoolReuseAllowInternalDependencies, 0);
  }
#endif

  VLOG(2) << Name() << " GPUcudaMallocAsyncAllocator PoolSize " << pool_size;
  if (reserve_memory) {
    void* ptr = AllocateRaw(0, pool_size);
    DeallocateRaw(ptr);
    VLOG(2) << Name() << " GPUcudaMallocAsyncAllocator Pre-filled the pool";
    ClearStats();
  }
}

GPUcudaMallocAsyncAllocator::~GPUcudaMallocAsyncAllocator() {
  delete base_allocator_;
  cuStreamDestroy(cuda_stream_);
}

void* GPUcudaMallocAsyncAllocator::AllocateRaw(size_t alignment,
                                               size_t num_bytes) {
#if CUDA_VERSION < 11020 || !defined(GOOGLE_CUDA)
  return nullptr;
#else
  se::cuda::ScopedActivateExecutorContext scoped_activation{stream_exec_};
  void* rv = 0;
  cudaError_t res = cudaMallocFromPoolAsync(&rv, num_bytes, pool_, cuda_stream_);
  if (res != cudaSuccess) {
    size_t free, total;
    cudaMemGetInfo(&free, &total);
    LOG(ERROR) << Name() << " cudaMallocAsync failed to allocate " << num_bytes
               << " Free Total: " << free << " " << total
               << ". Error: " << cudaGetErrorString(res)
               << " \nStats: \n" << stats_.DebugString();
    return nullptr;
  }

  // Update stats.
  ++stats_.num_allocs;
  stats_.bytes_in_use += num_bytes;
  stats_.peak_bytes_in_use =
      std::max(stats_.peak_bytes_in_use, stats_.bytes_in_use);
  stats_.largest_alloc_size =
      std::max<std::size_t>(stats_.largest_alloc_size, num_bytes);
  size_map_[rv] = num_bytes;
  VLOG(10) << Name() << " Allocated " << num_bytes << " at " << rv;
  return rv;
#endif
}
void GPUcudaMallocAsyncAllocator::DeallocateRaw(void* ptr) {
#if CUDA_VERSION < 11020 || !defined(GOOGLE_CUDA)
#else
  cudaError_t res = cudaFreeAsync(ptr, cuda_stream_);
  if (res != cudaSuccess) {
    size_t free, total;
    se::cuda::ScopedActivateExecutorContext scoped_activation{stream_exec_};
    cudaMemGetInfo(&free, &total);
    LOG(ERROR) << "cudaFreeAsync failed to free " << ptr
               << ". Error: " << cudaGetErrorString(res)
               << " \n Free Total " << free << " " << total
               << " \nStats: \n" << stats_.DebugString();
  }

  // Updates the stats.
  size_t size = size_map_[ptr];
  stats_.bytes_in_use -= size;
  size_map_.erase(ptr);

  VLOG(10) << Name() << " Freed ptr: " << ptr;
#endif  // GOOGLE_CUDA
}

bool GPUcudaMallocAsyncAllocator::TracksAllocationSizes() const {
  return true;
}

size_t GPUcudaMallocAsyncAllocator::RequestedSize(const void* ptr) const {
  CHECK(ptr);
  return size_map_.at(ptr);
}

size_t GPUcudaMallocAsyncAllocator::AllocatedSize(const void* ptr) const {
  CHECK(ptr);
  return size_map_.at(ptr);
}

absl::optional<AllocatorStats> GPUcudaMallocAsyncAllocator::GetStats() {
  mutex_lock l(lock_);
  return stats_;
}

void GPUcudaMallocAsyncAllocator::ClearStats() {
  mutex_lock l(lock_);
  stats_.num_allocs = 0;
  stats_.peak_bytes_in_use = stats_.bytes_in_use;
  stats_.largest_alloc_size = 0;
}

}  // namespace tensorflow
