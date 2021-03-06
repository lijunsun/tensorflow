/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/stream_executor/cuda/cuda_dnn.h"

#include <dlfcn.h>
#include <functional>
#include <memory>

#include "tensorflow/stream_executor/cuda/cuda_activation.h"
#include "tensorflow/stream_executor/cuda/cuda_diagnostics.h"
#include "tensorflow/stream_executor/cuda/cuda_driver.h"
#include "tensorflow/stream_executor/cuda/cuda_gpu_executor.h"
#include "tensorflow/stream_executor/cuda/cuda_platform_id.h"
#include "tensorflow/stream_executor/cuda/cuda_stream.h"
#include "tensorflow/stream_executor/cuda/cuda_timer.h"
#include "tensorflow/stream_executor/dnn.h"
#include "tensorflow/stream_executor/dso_loader.h"
#include "tensorflow/stream_executor/lib/env.h"
#include "tensorflow/stream_executor/lib/error.h"
#include "tensorflow/stream_executor/lib/initialize.h"
#include "tensorflow/stream_executor/lib/strcat.h"
#include "tensorflow/stream_executor/lib/threadpool.h"
#include "tensorflow/stream_executor/platform/logging.h"
#include "tensorflow/stream_executor/plugin_registry.h"
#include "tensorflow/stream_executor/scratch_allocator.h"
#include "tensorflow/stream_executor/stream.h"
#include "tensorflow/stream_executor/stream_executor_pimpl.h"
// clang-format off
#include "third_party/gpus/cuda/include/cudnn.h"
// clang-format on

namespace {

// Converts (via narrowing) a type T value to a type U, and checks that the
// value has no value change due to the conversion.
template <typename WideT, typename NarrowT>
NarrowT CheckedNarrowing(const WideT& wide) {
  NarrowT narrow = wide;
  CHECK_EQ(narrow, wide)
      << "checked narrowing failed; values not equal post-conversion";
  return narrow;
}

// Returns the "Compatibility" version number from the CuDNN version number.
// This is the number that tries to indicate ABI compatibility.
//
// For example, if cudnn_version is 5107, the compatibility version
// number will be 5100.
size_t cudnnCompatibilityVersion(size_t cudnn_version) {
  return (cudnn_version / 100) * 100;
}

}  // namespace

namespace perftools {
namespace gputools {

using dnn::BatchDescriptor;
using dnn::FilterDescriptor;
using dnn::ConvolutionDescriptor;
using dnn::PoolingDescriptor;

namespace cuda {

PLUGIN_REGISTRY_DEFINE_PLUGIN_ID(kCuDnnPlugin);

string ToString(cudnnStatus_t status) {
  switch (status) {
    case CUDNN_STATUS_SUCCESS:
      return "CUDNN_STATUS_SUCCESS";
    case CUDNN_STATUS_NOT_INITIALIZED:
      return "CUDNN_STATUS_NOT_INITIALIZED";
    case CUDNN_STATUS_ALLOC_FAILED:
      return "CUDNN_STATUS_ALLOC_FAILED";
    case CUDNN_STATUS_BAD_PARAM:
      return "CUDNN_STATUS_BAD_PARAM";
    case CUDNN_STATUS_INTERNAL_ERROR:
      return "CUDNN_STATUS_INTERNAL_ERROR";
    case CUDNN_STATUS_INVALID_VALUE:
      return "CUDNN_STATUS_INVALID_VALUE";
    case CUDNN_STATUS_ARCH_MISMATCH:
      return "CUDNN_STATUS_ARCH_MISMATCH";
    case CUDNN_STATUS_MAPPING_ERROR:
      return "CUDNN_STATUS_MAPPING_ERROR";
    case CUDNN_STATUS_EXECUTION_FAILED:
      return "CUDNN_STATUS_EXECUTION_FAILED";
    case CUDNN_STATUS_NOT_SUPPORTED:
      return "CUDNN_STATUS_NOT_SUPPORTED";
    case CUDNN_STATUS_LICENSE_ERROR:
      return "CUDNN_STATUS_LICENSE_ERROR";
    default:
      return port::StrCat("<unknown cudnn status: ", static_cast<int>(status),
                          ">");
  }
}

namespace dynload {

static port::ThreadPool* InitCudnnThreadpool() {
  port::ThreadPool* cudnn_threadpool_;
  port::ThreadOptions options;
  // TBD(keveman): Conservatively setting the stack size and guard size to 2MB,
  // until we can get some guarantees from NVIDIA on the minimum stack space
  // they will work with.
  options.stack_size = 2 * 1024 * 1024;
  options.guard_size = 2 * 1024 * 1024;
  cudnn_threadpool_ = new port::ThreadPool(port::Env::Default(), options,
                                           "cudnn_threadpool", 1);
  CHECK(cudnn_threadpool_);
  return cudnn_threadpool_;
}

static mutex cudnn_threadpool_mu(LINKER_INITIALIZED);
static port::ThreadPool* GetCudaThreadpool() {
  mutex_lock lock(cudnn_threadpool_mu);
  static port::ThreadPool* cudnn_threadpool = InitCudnnThreadpool();
  return cudnn_threadpool;
}

// Retrieves the CUDNN DSO, dies on failure.
void* GetDsoHandle() {
  static auto result = internal::CachedDsoLoader::GetCudnnDsoHandle();
  return result.ValueOrDie();
}

// Calls cudnnGetVersion in the loaded DSO.
size_t cudnnGetVersion() {
  static void* f = dlsym(GetDsoHandle(), "cudnnGetVersion");
  if (f == nullptr) {
    LOG(FATAL) << "could not find cudnnGetVersion in cudnn DSO; dlerror: "
               << dlerror();
  }
  auto callable = reinterpret_cast<size_t (*)(void)>(f);
  return callable();
}

#define PERFTOOLS_GPUTOOLS_CUDNN_WRAP(__name)                        \
  struct DynLoadShim__##__name {                                     \
    static const char* kName;                                        \
    typedef std::add_pointer<decltype(::__name)>::type FuncPointerT; \
    static FuncPointerT DynLoad() {                                  \
      static void* f = dlsym(GetDsoHandle(), kName);                 \
      if (f == nullptr) {                                            \
        LOG(FATAL) << "could not find " << kName                     \
                   << " in cudnn DSO; dlerror: " << dlerror();       \
      }                                                              \
      return reinterpret_cast<FuncPointerT>(f);                      \
    }                                                                \
    template <typename... Args>                                      \
    cudnnStatus_t operator()(CUDAExecutor* parent, Args... args) {   \
      cuda::ScopedActivateExecutorContext sac{parent};               \
      cudnnStatus_t retval = DynLoad()(args...);                     \
      return retval;                                                 \
    }                                                                \
  } __name;                                                          \
  const char* DynLoadShim__##__name::kName = #__name;

// clang-format off
#define CUDNN_DNN_ROUTINE_EACH(__macro)                   \
  __macro(cudnnGetConvolutionNdForwardOutputDim)          \
  __macro(cudnnGetConvolutionForwardAlgorithm)            \
  __macro(cudnnCreateTensorDescriptor)                    \
  __macro(cudnnDestroyTensorDescriptor)                   \
  __macro(cudnnCreateFilterDescriptor)                    \
  __macro(cudnnSetPoolingNdDescriptor)                    \
  __macro(cudnnDestroyFilterDescriptor)                   \
  __macro(cudnnCreateConvolutionDescriptor)               \
  __macro(cudnnCreatePoolingDescriptor)                   \
  __macro(cudnnDestroyPoolingDescriptor)                  \
  __macro(cudnnDestroyConvolutionDescriptor)              \
  __macro(cudnnCreate)                                    \
  __macro(cudnnDestroy)                                   \
  __macro(cudnnSetStream)                                 \
  __macro(cudnnActivationForward)                         \
  __macro(cudnnConvolutionForward)                        \
  __macro(cudnnConvolutionBackwardBias)                   \
  __macro(cudnnGetConvolutionForwardWorkspaceSize)        \
  __macro(cudnnTransformTensor)                           \
  __macro(cudnnSetConvolutionNdDescriptor)                \
  __macro(cudnnSetTensorNdDescriptor)                     \
  __macro(cudnnSetFilterNdDescriptor)                     \
  __macro(cudnnPoolingForward)                            \
  __macro(cudnnPoolingBackward)                           \
  __macro(cudnnAddTensor)                                 \
  __macro(cudnnConvolutionBackwardData)                   \
  __macro(cudnnConvolutionBackwardFilter)
// clang-format on

CUDNN_DNN_ROUTINE_EACH(PERFTOOLS_GPUTOOLS_CUDNN_WRAP)

// APIs available after R3:
#if CUDNN_VERSION >= 3000
#define CUDNN_DNN_ROUTINE_EACH_AFTER_R3(__macro)              \
  __macro(cudnnGetConvolutionBackwardFilterWorkspaceSize)     \
  __macro(cudnnGetConvolutionBackwardDataAlgorithm)           \
  __macro(cudnnGetConvolutionBackwardFilterAlgorithm)         \
  __macro(cudnnGetConvolutionBackwardDataWorkspaceSize)
CUDNN_DNN_ROUTINE_EACH_AFTER_R3(PERFTOOLS_GPUTOOLS_CUDNN_WRAP)
#undef CUDNN_DNN_ROUTINE_EACH_AFTER_R3
#endif

// APIs in R3 but not in R5
// clang-format off
#if CUDNN_VERSION >= 3000 && CUDNN_VERSION < 5000
#define CUDNN_DNN_ROUTINE_EACH_R3(__macro)                    \
  __macro(cudnnAddTensor_v3)                                  \
  __macro(cudnnConvolutionBackwardData_v3)                    \
  __macro(cudnnConvolutionBackwardFilter_v3)
// clang-format on

CUDNN_DNN_ROUTINE_EACH_R3(PERFTOOLS_GPUTOOLS_CUDNN_WRAP)
#undef CUDNN_DNN_ROUTINE_EACH_R3
#endif

// APIs in R5
// clang-format off
#if CUDNN_VERSION >= 5000
#define CUDNN_DNN_ROUTINE_EACH_R5(__macro)                    \
  __macro(cudnnCreateActivationDescriptor)                    \
  __macro(cudnnSetActivationDescriptor)                       \
  __macro(cudnnGetActivationDescriptor)                       \
  __macro(cudnnDestroyActivationDescriptor)
// clang-format on

CUDNN_DNN_ROUTINE_EACH_R5(PERFTOOLS_GPUTOOLS_CUDNN_WRAP)
#undef CUDNN_DNN_ROUTINE_EACH_R5
#endif

#undef CUDNN_DNN_ROUTINE_EACH

}  // namespace dynload

namespace {

cudnnHandle_t ToHandle(void* opaque_handle) {
  return static_cast<cudnnHandle_t>(opaque_handle);
}

cudnnConvolutionFwdAlgo_t ToConvForwardAlgo(dnn::AlgorithmType algorithm) {
  cudnnConvolutionFwdAlgo_t algo = cudnnConvolutionFwdAlgo_t(algorithm);
  switch (algo) {
    case CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM:
    case CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM:
    case CUDNN_CONVOLUTION_FWD_ALGO_GEMM:
    case CUDNN_CONVOLUTION_FWD_ALGO_DIRECT:
    case CUDNN_CONVOLUTION_FWD_ALGO_FFT:
    case CUDNN_CONVOLUTION_FWD_ALGO_FFT_TILING:
#if CUDNN_VERSION >= 5000
    case CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD:
#endif
      return algo;
    default:
      LOG(FATAL) << "Unsupported Cudnn convolution forward algorithm: "
                 << algorithm;
  }
}

cudnnConvolutionBwdDataAlgo_t ToConvBackwardDataAlgo(
    dnn::AlgorithmType algorithm) {
  cudnnConvolutionBwdDataAlgo_t algo = cudnnConvolutionBwdDataAlgo_t(algorithm);
  switch (algo) {
    case CUDNN_CONVOLUTION_BWD_DATA_ALGO_0:
    case CUDNN_CONVOLUTION_BWD_DATA_ALGO_1:
    case CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT:
    case CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT_TILING:
#if CUDNN_VERSION >= 5000
    case CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD:
#endif
      return algo;
    default:
      LOG(FATAL)
          << "Unsupported Cudnn convolution backward algorithm for data: "
          << algorithm;
  }
}

cudnnConvolutionBwdFilterAlgo_t ToConvBackwardFilterAlgo(
    dnn::AlgorithmType algorithm) {
  cudnnConvolutionBwdFilterAlgo_t algo =
      cudnnConvolutionBwdFilterAlgo_t(algorithm);
  switch (algo) {
    case CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0:
    case CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1:
    case CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT:
    case CUDNN_CONVOLUTION_BWD_FILTER_ALGO_3:
      return algo;
    default:
      LOG(FATAL)
          << "Unsupported Cudnn convolution backward algorithm for filter: "
          << algorithm;
  }
}

}  // namespace

CudnnSupport::CudnnSupport(CUDAExecutor* parent)
    : parent_(parent), dnn_handle_(nullptr) {}

CudnnSupport::~CudnnSupport() {
  auto status = dynload::cudnnDestroy(parent_, ToHandle(dnn_handle_));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "could not destroy cudnn handle: " << ToString(status);
  }
}

port::Status CudnnSupport::Init() {
  auto status = dynload::cudnnCreate(
      parent_, reinterpret_cast<cudnnHandle_t*>(&dnn_handle_));
  if (status == CUDNN_STATUS_SUCCESS) {
    // Check whether loaded version of CuDNN matches what the source
    // was built with.
    size_t loaded_version = dynload::cudnnGetVersion();
    size_t loaded_compat_version = cudnnCompatibilityVersion(loaded_version);
    size_t compiled_compat_version = cudnnCompatibilityVersion(CUDNN_VERSION);
    bool library_loaded_matches_source =
        (loaded_compat_version == compiled_compat_version);
    if (!library_loaded_matches_source) {
      const string error =
          port::StrCat("Loaded runtime CuDNN library: ", loaded_version,
                       " (compatibility version ", loaded_compat_version,
                       ") but source was compiled with ", CUDNN_VERSION,
                       " (compatibility version ", compiled_compat_version,
                       ").  If using a binary install, upgrade your CuDNN "
                       "library to match.  If building from sources, "
                       "make sure the library loaded at runtime matches a "
                       "compatible version specified during compile "
                       "configuration.");
      LOG(ERROR) << error;
      return port::Status{port::error::INTERNAL, error};
    }

    return port::Status::OK();
  }

  LOG(ERROR) << "could not create cudnn handle: " << ToString(status);
  if (status == CUDNN_STATUS_NOT_INITIALIZED) {
    // This is the error code that the driver returns when we're not running a
    // sufficient CUDA driver -- cudnn requires 6.5+ compatibility, which
    // starts with the 340.XX driver series.
    auto result = cuda::Diagnostician::FindKernelDriverVersion();
    if (!result.ok()) {
      LOG(ERROR) << "error retrieving driver version: "
                 << DriverVersionStatusToString(result);
    } else {
      const auto& version = result.ValueOrDie();
      LOG(INFO) << "running driver version: " << DriverVersionToString(version);
      // OS X kernel driver does not report version accurately
#if !defined(__APPLE__)
      if (std::get<0>(version) < 340) {
        LOG(ERROR)
            << "cudnn library is only supported on 340.XX+ driver versions";
      }
#endif
    }
  }

  return port::Status{port::error::INTERNAL,
                      port::StrCat("cudnn library could not create a handle: ",
                                   ToString(status))};
}

// Turns a BatchDescriptor structure into a cudnn tensor handle within a scope.
class ScopedTensorDescriptor {
 public:
  ScopedTensorDescriptor(CUDAExecutor* parent,
                         const BatchDescriptor& batch_descriptor,
                         cudnnDataType_t elem_type)
      : parent_(parent), handle_(nullptr) {
    cudnnStatus_t status =
        dynload::cudnnCreateTensorDescriptor(parent_, &handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not create cudnn tensor descriptor: "
                 << ToString(status);
    }

    switch (batch_descriptor.layout()) {
      case dnn::DataLayout::kBatchYXDepth:
      case dnn::DataLayout::kBatchDepthYX:
        break;
      default:
        LOG(FATAL) << "Unsupported tensor format "
                   << DataLayoutString(batch_descriptor.layout());
        break;
    }

    const int nd = batch_descriptor.ndims() + 2;
    // cuDNN requires the strides and dims to be ordered as BDYX.
    std::vector<int64> strides64 =
        batch_descriptor.full_strides(dnn::DataLayout::kBatchDepthYX);
    std::vector<int64> dims64 =
        batch_descriptor.full_dims(dnn::DataLayout::kBatchDepthYX);

    // cuDNN requires arrays of ints.
    std::vector<int> strides(nd);
    std::vector<int> dims(nd);
    std::transform(strides64.cbegin(), strides64.cend(), strides.begin(),
                   &CheckedNarrowing<int64, int>);
    std::transform(dims64.cbegin(), dims64.cend(), dims.begin(),
                   &CheckedNarrowing<int64, int>);
    status = dynload::cudnnSetTensorNdDescriptor(
        parent_, handle_, elem_type, nd, dims.data(), strides.data());

    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not set cudnn tensor descriptor: "
                 << ToString(status);
    }
  }

  ~ScopedTensorDescriptor() {
    cudnnStatus_t status =
        dynload::cudnnDestroyTensorDescriptor(parent_, handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(ERROR) << "could not destroy cudnn tensor descriptor: "
                 << ToString(status);
    }
  }

  cudnnTensorDescriptor_t handle() const { return handle_; }

 private:
  CUDAExecutor* parent_;            // Parent executor. Not owned.
  cudnnTensorDescriptor_t handle_;  // Owned.

  SE_DISALLOW_COPY_AND_ASSIGN(ScopedTensorDescriptor);
};

// Turns a FilterDescriptor structure into a cudnn filter handle within a scope.
class ScopedFilterDescriptor {
 public:
  ScopedFilterDescriptor(CUDAExecutor* parent,
                         const FilterDescriptor& filter_descriptor,
                         const BatchDescriptor& batch_descriptor,
                         cudnnDataType_t elem_type)
      : parent_(parent), handle_(nullptr) {
    cudnnStatus_t status =
        dynload::cudnnCreateFilterDescriptor(parent_, &handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not create cudnn filter descriptor: "
                 << ToString(status);
    }

#if CUDNN_VERSION >= 5000
    // TODO(b/23032134): Even if the filter layout is not supported,
    // cudnnSetFilter4DDescriptor_v4 will return CUDNN_STATUS_SUCCESS because it
    // does not take layout as an input. Maybe force cuDNN by giving wrong
    // inputs intentionally?
    cudnnTensorFormat_t format;
    switch (filter_descriptor.layout()) {
      case dnn::FilterLayout::kOutputInputYX:
        format = CUDNN_TENSOR_NCHW;
        break;
      default:
        LOG(FATAL) << "Unsupported filter format "
                   << FilterLayoutString(filter_descriptor.layout());
        break;
    }
#endif

    std::vector<int> dims(2 + filter_descriptor.ndims());
    dims[0] = filter_descriptor.output_feature_map_count();
    dims[1] = filter_descriptor.input_feature_map_count();
    const auto& spatial_dims = filter_descriptor.input_filter_dims();
    std::copy(spatial_dims.begin(), spatial_dims.end(), dims.begin() + 2);

    status = dynload::cudnnSetFilterNdDescriptor(parent_, handle_, elem_type,
#if CUDNN_VERSION >= 5000
                                                 format,
#endif
                                                 dims.size(), dims.data());
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not set cudnn filter descriptor: "
                 << ToString(status);
    }
  }

  ~ScopedFilterDescriptor() {
    cudnnStatus_t status =
        dynload::cudnnDestroyFilterDescriptor(parent_, handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(ERROR) << "could not destroy cudnn filter descriptor: "
                 << ToString(status);
    }
  }

  cudnnFilterDescriptor_t handle() const { return handle_; }

 private:
  // Parent executor object. Not owned.
  CUDAExecutor* parent_;

  // cudnn filter descriptor this object creates. Owned.
  cudnnFilterDescriptor_t handle_;

  SE_DISALLOW_COPY_AND_ASSIGN(ScopedFilterDescriptor);
};

// Turns a ConvolutionDescriptor structure into a cudnn convolution handle
// within a scope.
class ScopedConvolutionDescriptor {
 public:
  ScopedConvolutionDescriptor(
      CUDAExecutor* parent, const ConvolutionDescriptor& convolution_descriptor,
      cudnnDataType_t data_type)
      : parent_(parent), handle_(nullptr) {
    cudnnStatus_t status =
        dynload::cudnnCreateConvolutionDescriptor(parent_, &handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not create cudnn convolution descriptor: "
                 << ToString(status);
    }
    const auto& strides64 = convolution_descriptor.strides();
    const auto& padding64 = convolution_descriptor.padding();

    // cuDNN requires arrays of ints.
    std::vector<int> strides(convolution_descriptor.ndims());
    std::vector<int> padding(convolution_descriptor.ndims());
    std::transform(strides64.cbegin(), strides64.cend(), strides.begin(),
                   &CheckedNarrowing<int64, int>);
    std::transform(padding64.cbegin(), padding64.cend(), padding.begin(),
                   &CheckedNarrowing<int64, int>);
    std::vector<int> upscale(convolution_descriptor.ndims(), 1);

    status = dynload::cudnnSetConvolutionNdDescriptor(
        parent_, handle_, convolution_descriptor.ndims(), padding.data(),
        strides.data(), upscale.data(),
        // NOTE(keveman): cuDNN supports convolution and cross correlation.
        // However, almost all the use cases do cross correlation, so just
        // hard coding it here.
        CUDNN_CROSS_CORRELATION, data_type);

    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not set cudnn convolution descriptor: "
                 << ToString(status);
    }
  }

  ~ScopedConvolutionDescriptor() {
    cudnnStatus_t status =
        dynload::cudnnDestroyConvolutionDescriptor(parent_, handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(ERROR) << "could not destroy cudnn convolution descriptor: "
                 << ToString(status);
    }
  }

  cudnnConvolutionDescriptor_t handle() const { return handle_; }

 private:
  CUDAExecutor* parent_;                 // Parent executor. Not owned.
  cudnnConvolutionDescriptor_t handle_;  // Owned.

  SE_DISALLOW_COPY_AND_ASSIGN(ScopedConvolutionDescriptor);
};

// Turns a PoolingDescriptor structure into a cudnn pooling descriptor handle
// within a scope.
class ScopedPoolingDescriptor {
 public:
  ScopedPoolingDescriptor(CUDAExecutor* parent,
                          const PoolingDescriptor& pooling_descriptor)
      : parent_(parent), handle_(nullptr) {
    cudnnStatus_t status =
        dynload::cudnnCreatePoolingDescriptor(parent_, &handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not create cudnn pooling descriptor: "
                 << ToString(status);
    }

    const std::vector<int64> strides64 = pooling_descriptor.strides();
    const std::vector<int64> padding64 = pooling_descriptor.padding();
    const std::vector<int64> shape64 = pooling_descriptor.window();

    const int nd = pooling_descriptor.ndims();
    std::vector<int> shape(nd);
    std::vector<int> padding(nd);
    std::vector<int> strides(nd);
    std::transform(strides64.cbegin(), strides64.cend(), strides.begin(),
                   &CheckedNarrowing<int64, int>);
    std::transform(padding64.cbegin(), padding64.cend(), padding.begin(),
                   &CheckedNarrowing<int64, int>);
    std::transform(shape64.cbegin(), shape64.cend(), shape.begin(),
                   &CheckedNarrowing<int64, int>);
    status = dynload::cudnnSetPoolingNdDescriptor(
        parent_, handle_,
        (pooling_descriptor.mode() == dnn::PoolingMode::kMaximum
             ? CUDNN_POOLING_MAX
             : CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING),
#if CUDNN_VERSION >= 5000
        // Always propagate nans.
        CUDNN_PROPAGATE_NAN,
#endif
        nd, shape.data(), padding.data(), strides.data());
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not set cudnn pooling descriptor: "
                 << ToString(status);
    }
  }
  ~ScopedPoolingDescriptor() {
    cudnnStatus_t status =
        dynload::cudnnDestroyPoolingDescriptor(parent_, handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(ERROR) << "could not destroy cudnn pooling descriptor: "
                 << ToString(status);
    }
  }

  cudnnPoolingDescriptor_t handle() const { return handle_; }

 private:
  CUDAExecutor* parent_;             // Parent executor. Not owned.
  cudnnPoolingDescriptor_t handle_;  // Owned.

  SE_DISALLOW_COPY_AND_ASSIGN(ScopedPoolingDescriptor);
};

#if CUDNN_VERSION >= 5000
// Turns a ActivationDescriptor structure into a cudnn activation
// descriptor handle within a scope.
class ScopedActivationDescriptor {
 public:
  ScopedActivationDescriptor(CUDAExecutor* parent,
                             dnn::ActivationMode activation_mode,
                             double value_max)
      : parent_(parent), handle_(nullptr) {
    cudnnStatus_t status =
        dynload::cudnnCreateActivationDescriptor(parent_, &handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not create cudnn activation descriptor: "
                 << ToString(status);
    }

    double relu_ceiling = 0.0;
    cudnnActivationMode_t mode;
    switch (activation_mode) {
      case dnn::ActivationMode::kRelu6:
        relu_ceiling = 6.0;
        mode = CUDNN_ACTIVATION_CLIPPED_RELU;
        break;
      case dnn::ActivationMode::kReluX:
        relu_ceiling = value_max;
        mode = CUDNN_ACTIVATION_CLIPPED_RELU;
        break;
      case dnn::ActivationMode::kRelu:
        mode = CUDNN_ACTIVATION_RELU;
        break;
      case dnn::ActivationMode::kSigmoid:
        mode = CUDNN_ACTIVATION_SIGMOID;
        break;
      case dnn::ActivationMode::kTanh:
        mode = CUDNN_ACTIVATION_TANH;
        break;
      default:
        LOG(FATAL) << "unrecognized activation mode: "
                   << static_cast<int>(activation_mode);
    }

    // Always propagate nans.
    cudnnNanPropagation_t nan_propagation = CUDNN_PROPAGATE_NAN;
    status = dynload::cudnnSetActivationDescriptor(
        parent_, handle_,
        mode, nan_propagation, relu_ceiling);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(FATAL) << "could not set cudnn activation descriptor: "
                 << ToString(status);
    }
  }

  ~ScopedActivationDescriptor() {
    cudnnStatus_t status =
        dynload::cudnnDestroyActivationDescriptor(parent_, handle_);
    if (status != CUDNN_STATUS_SUCCESS) {
      LOG(ERROR) << "could not destroy cudnn activation descriptor: "
                 << ToString(status);
    }
  }

  cudnnActivationDescriptor_t handle() const { return handle_; }

 private:
  CUDAExecutor* parent_;                // Parent executor. Not owned.
  cudnnActivationDescriptor_t handle_;  // Owned.

  SE_DISALLOW_COPY_AND_ASSIGN(ScopedActivationDescriptor);
};
#endif

template <class T>
bool CudnnSupport::DoConvolveImpl(
    Stream* stream, int cudnn_type,  // Actually cudnnDataType_t.
    const BatchDescriptor& batch_descriptor, const DeviceMemory<T>& input_data,
    const FilterDescriptor& filter_descriptor,
    const DeviceMemory<T>& filter_data,
    const ConvolutionDescriptor& convolution_descriptor,
    const BatchDescriptor& output_descriptor, DeviceMemory<T>* output_data,
    ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  ScopedTensorDescriptor input_nd{parent_, batch_descriptor,
      static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedTensorDescriptor output_nd{parent_, output_descriptor,
      static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedFilterDescriptor filter{parent_, filter_descriptor, batch_descriptor,
      static_cast<cudnnDataType_t>(cudnn_type)};
  // TODO(sesse): Figure out under what circumstances cuDNN would
  // accept CUDNN_DATA_HALF here; probably related to compute capability
  // and cuDNN version; at least cuDNN 4 on TITAN X only supports
  // CUDNN_DATA_FLOAT even for half input.
  ScopedConvolutionDescriptor conv{parent_, convolution_descriptor,
      CUDNN_DATA_FLOAT};

  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(FATAL) << "failed to set stream for cudnn handle: " << ToString(status);
  }
  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  const bool is_profiling = output_profile_result != nullptr;
  cudnnConvolutionFwdAlgo_t algo;
  DeviceMemory<uint8> scratch;

  if (algorithm_config.algorithm() == dnn::kDefaultAlgorithm) {
    // With the default algorithm, use Cudnn's heuristics.
    auto get_algorithm = [&](bool specify_limit)
        SHARED_LOCKS_REQUIRED(dnn_handle_mutex_) {
          cudnnConvolutionFwdPreference_t preference =
              specify_limit ? CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT
                            : CUDNN_CONVOLUTION_FWD_NO_WORKSPACE;

          auto memory_limit_bytes =
              scratch_allocator == nullptr
                  ? 0
                  : scratch_allocator->GetMemoryLimitInBytes(stream);
          if (memory_limit_bytes < 0) {
            memory_limit_bytes = 0;
          }

          cudnnConvolutionFwdAlgo_t algo_to_use;
          status = dynload::cudnnGetConvolutionForwardAlgorithm(
              parent_, ToHandle(dnn_handle_), input_nd.handle(),
              filter.handle(), conv.handle(), output_nd.handle(),
              /*preference=*/preference,
              /*memoryLimitInBytes=*/memory_limit_bytes,
              /*algo=*/&algo_to_use);
          CHECK_EQ(status, CUDNN_STATUS_SUCCESS)
              << "Unable to find a suitable "
                 "algorithm for doing forward "
                 "convolution";
          return algo_to_use;
        };

    algo = get_algorithm(/*specify_limit=*/scratch_allocator != nullptr);

    if (scratch_allocator != nullptr) {
      size_t size_in_bytes;
      status = dynload::cudnnGetConvolutionForwardWorkspaceSize(
          parent_, ToHandle(dnn_handle_), /*srcDesc=*/input_nd.handle(),
          /*filterDesc=*/filter.handle(), /*convDesc=*/conv.handle(),
          /*destDesc=*/output_nd.handle(), /*algo=*/algo,
          /*sizeInBytes=*/&size_in_bytes);
      if (status == CUDNN_STATUS_SUCCESS && size_in_bytes != 0) {
        auto allocated =
            scratch_allocator->AllocateBytes(stream, size_in_bytes);
        if (allocated.ok()) {
          scratch = allocated.ValueOrDie();
        }
      }
    }

    // If we didn't allocate any scratch space (perhaps because of failed
    // allocation), we force a switch back to the "no workspace" algorithm.
    if (scratch == nullptr) {
      algo = get_algorithm(/*specify_limit=*/false);
    }
  } else {
    // An algorithm has been specified.
    algo = ToConvForwardAlgo(algorithm_config.algorithm());

    size_t size_in_bytes;
    status = dynload::cudnnGetConvolutionForwardWorkspaceSize(
        parent_, ToHandle(dnn_handle_), /*srcDesc=*/input_nd.handle(),
        /*filterDesc=*/filter.handle(), /*convDesc=*/conv.handle(),
        /*destDesc=*/output_nd.handle(), /*algo=*/algo,
        /*sizeInBytes=*/&size_in_bytes);
    if (status != CUDNN_STATUS_SUCCESS) {
      if (is_profiling) {
        // Silently return when we are profiling.
        return false;
      }
      LOG(FATAL) << "Cannot query the size of workspace needed for the given "
                    "algorithm: "
                 << algorithm_config.algorithm();
    }
    if (size_in_bytes != 0) {
      if (scratch_allocator == nullptr) {
        LOG(FATAL) << "An allocator must be specified when scratch memory is "
                      "needed";
      }
      auto allocated = scratch_allocator->AllocateBytes(stream, size_in_bytes);
      if (is_profiling && !allocated.ok()) {
        // Silently return when we are profiling.
        return false;
      }
      if (allocated.ok()) {
        scratch = allocated.ValueOrDie();
      }
      if (scratch == nullptr) {
        CHECK(algorithm_config.algorithm_no_scratch() != dnn::kDefaultAlgorithm)
            << "The primary convolution algorithm failed memory allocation, "
               "while a secondary algorithm is not provided.";
        algo = ToConvForwardAlgo(algorithm_config.algorithm_no_scratch());
      }
    }
  }

  std::unique_ptr<CUDATimer> timer;
  if (is_profiling) {
    timer.reset(new CUDATimer(parent_));
    timer->Init();
    // The start and stop of the timer should be as close to the Cudnn call as
    // possible. It is still possible for other threads to issue workload on
    // to this stream. So it could take multiple profiling measurements.
    timer->Start(AsCUDAStream(stream));
  }
  status = dynload::cudnnConvolutionForward(
      parent_, ToHandle(dnn_handle_),
      /*alpha=*/&alpha, /*srcDesc=*/input_nd.handle(),
      /*srcData=*/input_data.opaque(), /*filterDesc=*/filter.handle(),
      /*filterData=*/filter_data.opaque(), /*convDesc=*/conv.handle(),
      /*algo=*/algo, /*workSpace=*/scratch.opaque(),
      /*workSpaceSizeInBytes=*/scratch.size(), /*beta=*/&beta,
      /*destDesc=*/output_nd.handle(), /*destData=*/output_data->opaque());
  if (is_profiling) {
    timer->Stop(AsCUDAStream(stream));
    output_profile_result->set_is_valid(true);
    output_profile_result->set_algorithm(algo);
    output_profile_result->set_elapsed_time_in_ms(
        timer->GetElapsedMilliseconds());
    timer->Destroy();
  }

  if (status != CUDNN_STATUS_SUCCESS) {
    // Silently return when we are profiling.
    if (!is_profiling) {
      LOG(FATAL) << "failed to enqueue convolution on stream: "
                 << ToString(status);
    }
    return false;
  }

  return true;
}

bool CudnnSupport::GetConvolveAlgorithms(
    std::vector<dnn::AlgorithmType>* out_algorithms) {
  out_algorithms->assign({
      // clang-format off
      CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM,
      CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM,
      CUDNN_CONVOLUTION_FWD_ALGO_GEMM,
      CUDNN_CONVOLUTION_FWD_ALGO_DIRECT,
      CUDNN_CONVOLUTION_FWD_ALGO_FFT,
      CUDNN_CONVOLUTION_FWD_ALGO_FFT_TILING,
#if CUDNN_VERSION >= 5000
      CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD,
#endif
      // clang-format on
  });
  return true;
}

bool CudnnSupport::GetConvolveBackwardDataAlgorithms(
    std::vector<dnn::AlgorithmType>* out_algorithms) {
  out_algorithms->assign({
      // clang-format off
      CUDNN_CONVOLUTION_BWD_DATA_ALGO_0,
      CUDNN_CONVOLUTION_BWD_DATA_ALGO_1,
      CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT,
      CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT_TILING,
#if CUDNN_VERSION >= 5000
      CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD,
#endif
      // clang-format on
  });
  return true;
}

bool CudnnSupport::GetConvolveBackwardFilterAlgorithms(
    std::vector<dnn::AlgorithmType>* out_algorithms) {
  out_algorithms->assign({
      // clang-format off
      CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0,
      CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1,
      CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT,
      CUDNN_CONVOLUTION_BWD_FILTER_ALGO_3,
      // clang-format on
  });
  return true;
}

bool CudnnSupport::DoConvolve(
    Stream* stream, const BatchDescriptor& batch_descriptor,
    const DeviceMemory<float>& input_data,
    const FilterDescriptor& filter_descriptor,
    const DeviceMemory<float>& filter_data,
    const ConvolutionDescriptor& convolution_descriptor,
    const BatchDescriptor& output_descriptor, DeviceMemory<float>* output_data,
    ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  return DoConvolveImpl<float>(
      stream, CUDNN_DATA_FLOAT, batch_descriptor, input_data, filter_descriptor,
      filter_data, convolution_descriptor, output_descriptor, output_data,
      scratch_allocator, algorithm_config, output_profile_result);
}

bool CudnnSupport::DoConvolve(
    Stream* stream, const BatchDescriptor& batch_descriptor,
    const DeviceMemory<double>& input_data,
    const FilterDescriptor& filter_descriptor,
    const DeviceMemory<double>& filter_data,
    const ConvolutionDescriptor& convolution_descriptor,
    const BatchDescriptor& output_descriptor,
    DeviceMemory<double>* output_data) {
  LOG(ERROR) << "double-based DNN not yet implemented";
  return false;
}

bool CudnnSupport::DoConvolve(
    Stream* stream, const BatchDescriptor& batch_descriptor,
    const DeviceMemory<Eigen::half>& input_data,
    const FilterDescriptor& filter_descriptor,
    const DeviceMemory<Eigen::half>& filter_data,
    const ConvolutionDescriptor& convolution_descriptor,
    const BatchDescriptor& output_descriptor,
    DeviceMemory<Eigen::half>* output_data, ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  return DoConvolveImpl<Eigen::half>(
      stream, CUDNN_DATA_HALF, batch_descriptor, input_data, filter_descriptor,
      filter_data, convolution_descriptor, output_descriptor, output_data,
      scratch_allocator, algorithm_config, output_profile_result);
}

template<class T>
DeviceMemory<T> CudnnSupport::MaybeTransformLayout(
    Stream* stream,
    int cudnn_type,  // Actually cudnnDataType_t.
    BatchDescriptor* output_descriptor,
    DeviceMemory<T> backward_output_data,
    std::unique_ptr<TemporaryDeviceMemory<T>>* transform_scratch) {
  if (output_descriptor->layout() == dnn::DataLayout::kBatchDepthYX) {
    return backward_output_data;
  }
  CHECK(output_descriptor->layout() == dnn::DataLayout::kBatchYXDepth);
  *transform_scratch =
      stream->AllocateTemporaryArray<T>(backward_output_data.ElementCount())
          .ConsumeValueOrDie();
  BatchDescriptor transformed_output_descriptor;
  transformed_output_descriptor.CloneFrom(*output_descriptor);
  transformed_output_descriptor.set_layout(dnn::DataLayout::kBatchDepthYX);
  ScopedTensorDescriptor orig_out_back_nd{
      parent_, *output_descriptor, static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedTensorDescriptor transformed_out_back_nd{
      parent_, transformed_output_descriptor,
      static_cast<cudnnDataType_t>(cudnn_type)};

  float alpha = 1.0f;
  float beta = 0.0f;
  auto status = dynload::cudnnTransformTensor(
      parent_, ToHandle(dnn_handle_), &alpha, orig_out_back_nd.handle(),
      backward_output_data.opaque(), &beta, transformed_out_back_nd.handle(),
      (*transform_scratch)->mutable_device_memory()->opaque());

  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(FATAL) << "Failed to transform the data layout.";
  }
  output_descriptor->set_layout(dnn::DataLayout::kBatchDepthYX);
  return (*transform_scratch)->device_memory();
}

template <class T>
bool CudnnSupport::DoConvolveBackwardDataImpl(
    Stream* stream,
    int cudnn_type,  // Actually cudnnDataType_t.
    const FilterDescriptor& filter_descriptor,
    const DeviceMemory<T>& filter_data,
    const BatchDescriptor& output_descriptor_in,
    DeviceMemory<T> backward_output_data,
    const ConvolutionDescriptor& convolution_descriptor,
    const BatchDescriptor& input_descriptor,
    DeviceMemory<T>* backward_input_data, ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(FATAL) << "failed to set stream for cudnn handle: " << ToString(status);
  }

  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  // TBD(keveman): remove once cuDNN supports kBatchYXDepth for backward pass.
  BatchDescriptor output_descriptor;
  output_descriptor.CloneFrom(output_descriptor_in);
  std::unique_ptr<TemporaryDeviceMemory<T>> transform_scratch;
  backward_output_data = MaybeTransformLayout(
      stream, cudnn_type, &output_descriptor, backward_output_data,
      &transform_scratch);

  ScopedTensorDescriptor out_back_nd{parent_, output_descriptor,
                                     static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedTensorDescriptor in_back_nd{parent_, input_descriptor,
                                    static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedFilterDescriptor filter{parent_, filter_descriptor, input_descriptor,
                                static_cast<cudnnDataType_t>(cudnn_type)};
  // TODO(sesse): Figure out under what circumstances cuDNN would
  // accept CUDNN_DATA_HALF here; probably related to compute capability
  // and cuDNN version; at least cuDNN 4 on TITAN X only supports
  // CUDNN_DATA_FLOAT even for half input.
  ScopedConvolutionDescriptor conv{parent_, convolution_descriptor,
                                   CUDNN_DATA_FLOAT};

  const bool is_profiling = output_profile_result != nullptr;
  cudnnConvolutionBwdDataAlgo_t algo;
  DeviceMemory<uint8> scratch;

  if (algorithm_config.algorithm() == dnn::kDefaultAlgorithm) {
    // With the default algorithm, use Cudnn's heuristics.
    auto get_algorithm = [&](bool specify_limit) SHARED_LOCKS_REQUIRED(
        dnn_handle_mutex_) -> cudnnConvolutionBwdDataAlgo_t {
      cudnnConvolutionBwdDataPreference_t preference =
          specify_limit ? CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT
                        : CUDNN_CONVOLUTION_BWD_DATA_NO_WORKSPACE;

      auto memory_limit_bytes =
          scratch_allocator == nullptr
              ? 0
              : scratch_allocator->GetMemoryLimitInBytes(stream);
      if (memory_limit_bytes < 0) {
        memory_limit_bytes = 0;
      }

      cudnnConvolutionBwdDataAlgo_t algo_to_use;
      cudnnStatus_t status = dynload::cudnnGetConvolutionBackwardDataAlgorithm(
          parent_, ToHandle(dnn_handle_),
          /*filterDesc=*/filter.handle(),
          /*diffDesc=*/out_back_nd.handle(),
          /*convDesc=*/conv.handle(),
          /*gradDesc=*/in_back_nd.handle(),
          /*preference=*/preference,
          /*memoryLimitInBytes=*/memory_limit_bytes,
          /*algo=*/&algo_to_use);
      CHECK_EQ(status, CUDNN_STATUS_SUCCESS) << "Unable to find a suitable "
                                                "algorithm for doing backward "
                                                "filter convolution";
      return algo_to_use;
    };

    algo = get_algorithm(/*specify_limit=*/scratch_allocator != nullptr);

    if (scratch_allocator != nullptr) {
      size_t size_in_bytes;
      status = dynload::cudnnGetConvolutionBackwardDataWorkspaceSize(
          parent_, ToHandle(dnn_handle_),
          /*filterDesc=*/filter.handle(),
          /*diffDesc=*/out_back_nd.handle(),
          /*convDesc=*/conv.handle(),
          /*gradDesc=*/in_back_nd.handle(),
          /*algo=*/algo,
          /*sizeInBytes=*/&size_in_bytes);
      if (status == CUDNN_STATUS_SUCCESS && size_in_bytes != 0) {
        auto allocated =
            scratch_allocator->AllocateBytes(stream, size_in_bytes);
        if (allocated.ok()) {
          scratch = allocated.ValueOrDie();
        }
      }
    }

    // If we didn't allocate any scratch space (perhaps because of failed
    // allocation), we force a switch back to the "no workspace" algorithm.
    if (scratch == nullptr) {
      algo = get_algorithm(/*specify_limit=*/false);
    }
  } else {
    // An algorithm has been specified.
    algo = ToConvBackwardDataAlgo(algorithm_config.algorithm());
    size_t size_in_bytes;
    status = dynload::cudnnGetConvolutionBackwardDataWorkspaceSize(
        parent_, ToHandle(dnn_handle_),
        /*filterDesc=*/filter.handle(),
        /*diffDesc=*/out_back_nd.handle(),
        /*convDesc=*/conv.handle(),
        /*gradDesc=*/in_back_nd.handle(),
        /*algo=*/algo,
        /*sizeInBytes=*/&size_in_bytes);
    if (status != CUDNN_STATUS_SUCCESS) {
      if (is_profiling) {
        // Silently return when we are profiling.
        return false;
      }
      LOG(FATAL) << "Cannot query the size of workspace needed for the given "
                    "algorithm: "
                 << algorithm_config.algorithm();
    }
    if (size_in_bytes != 0) {
      if (scratch_allocator == nullptr) {
        LOG(FATAL) << "An allocator must be specified when scratch memory is "
                      "needed";
      }
      auto allocated = scratch_allocator->AllocateBytes(stream, size_in_bytes);
      if (is_profiling && !allocated.ok()) {
        // Silently return when we are profiling.
        return false;
      }
      if (allocated.ok()) {
        scratch = allocated.ValueOrDie();
      }
      if (scratch == nullptr) {
        CHECK(algorithm_config.algorithm_no_scratch() != dnn::kDefaultAlgorithm)
            << "The primary convolution algorithm failed memory allocation, "
               "while a secondary algorithm is not provided.";
        algo = ToConvBackwardDataAlgo(algorithm_config.algorithm_no_scratch());
      }
    }
  }

  std::unique_ptr<CUDATimer> timer;
  if (is_profiling) {
    timer.reset(new CUDATimer(parent_));
    timer->Init();
    // The start and stop of the timer should be as close to the Cudnn call as
    // possible. It is still possible for other threads to issue workload on
    // to this stream. So it could take multiple profiling measurements.
    timer->Start(AsCUDAStream(stream));
  }

#if CUDNN_VERSION >= 5000
  status = dynload::cudnnConvolutionBackwardData(
#else
  status = dynload::cudnnConvolutionBackwardData_v3(
#endif
      parent_, ToHandle(dnn_handle_),
      /*alpha=*/&alpha,
      /*filterDesc=*/filter.handle(),
      /*filterData=*/filter_data.opaque(),
      /*diffDesc=*/out_back_nd.handle(),
      /*diffData=*/backward_output_data.opaque(),
      /*convDesc=*/conv.handle(),
      /*algo=*/algo,
      /*workSpace=*/scratch.opaque(),
      /*workSpaceSizeInBytes=*/scratch.size(),
      /*beta=*/&beta,
      /*gradDesc=*/in_back_nd.handle(),
      /*gradData=*/backward_input_data->opaque());
  if (is_profiling) {
    timer->Stop(AsCUDAStream(stream));
    output_profile_result->set_is_valid(true);
    output_profile_result->set_algorithm(algo);
    output_profile_result->set_elapsed_time_in_ms(
        timer->GetElapsedMilliseconds());
    timer->Destroy();
  }
  if (status != CUDNN_STATUS_SUCCESS) {
    // Silently return when we are profiling.
    if (!is_profiling) {
      LOG(FATAL) << "failed to enqueue convolution on stream: "
                 << ToString(status);
    }
    return false;
  }
  return true;
}

bool CudnnSupport::DoConvolveBackwardData(
    Stream* stream, const FilterDescriptor& filter_descriptor,
    const DeviceMemory<float>& filter_data,
    const BatchDescriptor& output_descriptor_in,
    DeviceMemory<float> backward_output_data,
    const ConvolutionDescriptor& convolution_descriptor,
    const BatchDescriptor& input_descriptor,
    DeviceMemory<float>* backward_input_data,
    ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  return DoConvolveBackwardDataImpl(
      stream, CUDNN_DATA_FLOAT, filter_descriptor, filter_data,
      output_descriptor_in, backward_output_data, convolution_descriptor,
      input_descriptor, backward_input_data, scratch_allocator,
      algorithm_config, output_profile_result);
}

bool CudnnSupport::DoConvolveBackwardData(
    Stream* stream, const FilterDescriptor& filter_descriptor,
    const DeviceMemory<Eigen::half>& filter_data,
    const BatchDescriptor& output_descriptor_in,
    DeviceMemory<Eigen::half> backward_output_data,
    const ConvolutionDescriptor& convolution_descriptor,
    const BatchDescriptor& input_descriptor,
    DeviceMemory<Eigen::half>* backward_input_data,
    ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  return DoConvolveBackwardDataImpl(
      stream, CUDNN_DATA_HALF, filter_descriptor, filter_data,
      output_descriptor_in, backward_output_data, convolution_descriptor,
      input_descriptor, backward_input_data, scratch_allocator,
      algorithm_config, output_profile_result);
}

template <class T>
bool CudnnSupport::DoConvolveBackwardFilterImpl(
    Stream* stream, int cudnn_type,  // Actually cudnnDataType_t.
    const dnn::BatchDescriptor& input_descriptor,
    const DeviceMemory<T>& input_data,
    const dnn::BatchDescriptor& output_descriptor_in,
    DeviceMemory<T> backward_output_data,
    const dnn::ConvolutionDescriptor& convolution_descriptor,
    const dnn::FilterDescriptor& filter_descriptor,
    DeviceMemory<T>* backward_filter_data, ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(FATAL) << "failed to set stream for cudnn handle: " << ToString(status);
  }

  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  // TBD(keveman): remove once cuDNN supports kBatchYXDepth for backward pass.
  BatchDescriptor output_descriptor;
  output_descriptor.CloneFrom(output_descriptor_in);
  std::unique_ptr<TemporaryDeviceMemory<T>> transform_scratch;
  backward_output_data = MaybeTransformLayout(
      stream, static_cast<cudnnDataType_t>(cudnn_type),
      &output_descriptor, backward_output_data,
      &transform_scratch);

  ScopedTensorDescriptor out_back_nd{parent_, output_descriptor,
        static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedTensorDescriptor input_nd{parent_, input_descriptor,
          static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedFilterDescriptor filter{parent_, filter_descriptor, input_descriptor,
        static_cast<cudnnDataType_t>(cudnn_type)};
  // TODO(sesse): Figure out under what circumstances cuDNN would
  // accept CUDNN_DATA_HALF here; probably related to compute capability
  // and cuDNN version; at least cuDNN 4 on TITAN X only supports
  // CUDNN_DATA_FLOAT even for half input.
  ScopedConvolutionDescriptor conv{parent_, convolution_descriptor,
      CUDNN_DATA_FLOAT};

  const bool is_profiling = output_profile_result != nullptr;
  cudnnConvolutionBwdFilterAlgo_t algo;
  DeviceMemory<uint8> scratch;

  if (algorithm_config.algorithm() == dnn::kDefaultAlgorithm) {
    // With the default algorithm, use Cudnn's heuristics.

    // Lambda that retrieves the algorithm.
    // specify_limit will occur when we have a scratch allocator and it succeeds
    // in allocating; otherwise, we'll fall back to the "no workspace" version.
    auto get_algorithm = [&](bool specify_limit) SHARED_LOCKS_REQUIRED(
        dnn_handle_mutex_) {
      cudnnConvolutionBwdFilterPreference_t preference =
          specify_limit ? CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT
                        : CUDNN_CONVOLUTION_BWD_FILTER_NO_WORKSPACE;

      auto memory_limit_bytes =
          scratch_allocator == nullptr
              ? 0
              : scratch_allocator->GetMemoryLimitInBytes(stream);
      if (memory_limit_bytes < 0) {
        memory_limit_bytes = 0;
      }

      cudnnConvolutionBwdFilterAlgo_t algo_to_use;
      cudnnStatus_t status =
          dynload::cudnnGetConvolutionBackwardFilterAlgorithm(
              parent_, ToHandle(dnn_handle_),
              /*srcDesc=*/input_nd.handle(),
              /*diffDesc=*/out_back_nd.handle(),
              /*convDesc=*/conv.handle(),
              /*gradDesc=*/filter.handle(),
              /*preference=*/preference,
              /*memoryLimitInBytes=*/memory_limit_bytes,
              /*algo=*/&algo_to_use);
      CHECK_EQ(status, CUDNN_STATUS_SUCCESS) << "Unable to find a suitable "
                                                "algorithm for doing backward "
                                                "filter convolution";
      return algo_to_use;
    };

    algo = get_algorithm(/*specify_limit=*/scratch_allocator != nullptr);

    if (scratch_allocator != nullptr) {
      size_t size_in_bytes;
      status = dynload::cudnnGetConvolutionBackwardFilterWorkspaceSize(
          parent_, ToHandle(dnn_handle_), /*srcDesc=*/input_nd.handle(),
          /*diffDesc=*/out_back_nd.handle(), /*convDesc=*/conv.handle(),
          /*gradDesc=*/filter.handle(), /*algo=*/algo,
          /*sizeInBytes=*/&size_in_bytes);
      if (status == CUDNN_STATUS_SUCCESS && size_in_bytes != 0) {
        auto allocated =
            scratch_allocator->AllocateBytes(stream, size_in_bytes);
        if (allocated.ok()) {
          scratch = allocated.ValueOrDie();
        }
      }
    }

    // If we didn't allocate any scratch space (perhaps because of failed
    // allocation), we force a switch back to the "no workspace" algorithm.
    if (scratch == nullptr) {
      algo = get_algorithm(/*specify_limit=*/false);
    }
  } else {
    // An algorithm has been specified.
    algo = ToConvBackwardFilterAlgo(algorithm_config.algorithm());

    size_t size_in_bytes;
    status = dynload::cudnnGetConvolutionBackwardFilterWorkspaceSize(
        parent_, ToHandle(dnn_handle_), /*srcDesc=*/input_nd.handle(),
        /*diffDesc=*/out_back_nd.handle(), /*convDesc=*/conv.handle(),
        /*gradDesc=*/filter.handle(), /*algo=*/algo,
        /*sizeInBytes=*/&size_in_bytes);
    if (status != CUDNN_STATUS_SUCCESS) {
      if (is_profiling) {
        // Silently return when we are profiling.
        return false;
      }
      LOG(FATAL) << "Cannot query the size of workspace needed for the given "
                    "algorithm: "
                 << algorithm_config.algorithm();
    }
    if (size_in_bytes != 0) {
      if (scratch_allocator == nullptr) {
        LOG(FATAL) << "An allocator must be specified when scratch memory is "
                      "needed";
      }
      auto allocated = scratch_allocator->AllocateBytes(stream, size_in_bytes);
      if (is_profiling && !allocated.ok()) {
        // Silently return when we are profiling.
        return false;
      }
      if (allocated.ok()) {
        scratch = allocated.ValueOrDie();
      }
      if (scratch == nullptr) {
        CHECK(algorithm_config.algorithm_no_scratch() != dnn::kDefaultAlgorithm)
            << "The primary convolution algorithm failed memory allocation, "
               "while a secondary algorithm is not provided.";
        algo =
            ToConvBackwardFilterAlgo(algorithm_config.algorithm_no_scratch());
      }
    }
  }

  std::unique_ptr<CUDATimer> timer;
  if (is_profiling) {
    timer.reset(new CUDATimer(parent_));
    timer->Init();
    // The start and stop of the timer should be as close to the Cudnn call as
    // possible. It is still possible for other threads to issue workload on
    // to this stream. So it could take multiple profiling measurements.
    timer->Start(AsCUDAStream(stream));
  }

#if CUDNN_VERSION >= 5000
  status = dynload::cudnnConvolutionBackwardFilter(
#else
  status = dynload::cudnnConvolutionBackwardFilter_v3(
#endif
      parent_, ToHandle(dnn_handle_), /*alpha=*/&alpha,
      /*srcDesc=*/input_nd.handle(),
      /*srcData=*/input_data.opaque(),
      /*diffDesc=*/out_back_nd.handle(),
      /*diffData=*/backward_output_data.opaque(),
      /*convDesc=*/conv.handle(),
      /*algo=*/algo,
      /*workSpace=*/scratch.opaque(),
      /*workSpaceSizeInBytes=*/scratch.size(),
      /*beta=*/&beta,
      /*gradDesc=*/filter.handle(),
      /*gradData=*/backward_filter_data->opaque());
  if (is_profiling) {
    timer->Stop(AsCUDAStream(stream));
    output_profile_result->set_is_valid(true);
    output_profile_result->set_algorithm(algo);
    output_profile_result->set_elapsed_time_in_ms(
        timer->GetElapsedMilliseconds());
    timer->Destroy();
  }
  if (status != CUDNN_STATUS_SUCCESS) {
    // Silently return when we are profiling.
    if (!is_profiling) {
      LOG(FATAL) << "failed to enqueue convolution on stream: "
                 << ToString(status);
    }
    return false;
  }
  return true;
}

bool CudnnSupport::DoConvolveBackwardFilter(
    Stream* stream, const dnn::BatchDescriptor& input_descriptor,
    const DeviceMemory<float>& input_data,
    const dnn::BatchDescriptor& output_descriptor_in,
    DeviceMemory<float> backward_output_data,
    const dnn::ConvolutionDescriptor& convolution_descriptor,
    const dnn::FilterDescriptor& filter_descriptor,
    DeviceMemory<float>* backward_filter_data,
    ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  return DoConvolveBackwardFilterImpl(
      stream, CUDNN_DATA_FLOAT, input_descriptor, input_data,
      output_descriptor_in, backward_output_data, convolution_descriptor,
      filter_descriptor, backward_filter_data, scratch_allocator,
      algorithm_config, output_profile_result);
}

bool CudnnSupport::DoConvolveBackwardFilter(
    Stream* stream, const dnn::BatchDescriptor& input_descriptor,
    const DeviceMemory<Eigen::half>& input_data,
    const dnn::BatchDescriptor& output_descriptor_in,
    DeviceMemory<Eigen::half> backward_output_data,
    const dnn::ConvolutionDescriptor& convolution_descriptor,
    const dnn::FilterDescriptor& filter_descriptor,
    DeviceMemory<Eigen::half>* backward_filter_data,
    ScratchAllocator* scratch_allocator,
    const dnn::AlgorithmConfig& algorithm_config,
    dnn::ProfileResult* output_profile_result) {
  return DoConvolveBackwardFilterImpl(
      stream, CUDNN_DATA_HALF, input_descriptor, input_data,
      output_descriptor_in, backward_output_data, convolution_descriptor,
      filter_descriptor, backward_filter_data, scratch_allocator,
      algorithm_config, output_profile_result);
}

template <class T>
bool CudnnSupport::DoConvolveBackwardBiasImpl(
    Stream* stream, int cudnn_type,  // Actually cudnnDataType_t.
    const dnn::BatchDescriptor& input_descriptor,
    const DeviceMemory<T>& input_data,
    const dnn::BatchDescriptor& bias_descriptor,
    DeviceMemory<T>* backward_bias_data) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(FATAL) << "failed to set stream for cudnn handle: " << ToString(status);
  }

  ScopedTensorDescriptor input_nd{parent_, input_descriptor,
                                  static_cast<cudnnDataType_t>(cudnn_type)};
  ScopedTensorDescriptor bias_nd{parent_, bias_descriptor,
                                 static_cast<cudnnDataType_t>(cudnn_type)};

  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  status = dynload::cudnnConvolutionBackwardBias(
      parent_, ToHandle(dnn_handle_), &alpha, input_nd.handle(),
      input_data.opaque(), &beta, bias_nd.handle(),
      backward_bias_data->opaque());
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(FATAL) << "failed to enqueue backward convolution on stream: "
               << ToString(status);
    return false;
  }
  return true;
}

bool CudnnSupport::DoConvolveBackwardBias(
    Stream* stream, const BatchDescriptor& input_descriptor,
    const DeviceMemory<double>& input_data,
    const BatchDescriptor& bias_descriptor,
    DeviceMemory<double>* backward_bias_data) {
  return DoConvolveBackwardBiasImpl(stream, CUDNN_DATA_DOUBLE, input_descriptor,
                                    input_data, bias_descriptor,
                                    backward_bias_data);
}

bool CudnnSupport::DoConvolveBackwardBias(
    Stream* stream, const BatchDescriptor& input_descriptor,
    const DeviceMemory<float>& input_data,
    const BatchDescriptor& bias_descriptor,
    DeviceMemory<float>* backward_bias_data) {
  return DoConvolveBackwardBiasImpl(stream, CUDNN_DATA_FLOAT, input_descriptor,
                                    input_data, bias_descriptor,
                                    backward_bias_data);
}

bool CudnnSupport::DoConvolveBackwardBias(
    Stream* stream, const BatchDescriptor& input_descriptor,
    const DeviceMemory<Eigen::half>& input_data,
    const BatchDescriptor& bias_descriptor,
    DeviceMemory<Eigen::half>* backward_bias_data) {
  return DoConvolveBackwardBiasImpl(stream, CUDNN_DATA_HALF, input_descriptor,
                                    input_data, bias_descriptor,
                                    backward_bias_data);
}

bool CudnnSupport::DoMatMul(Stream* stream,
                            const DeviceMemory<float>& input_data,
                            const DeviceMemory<float>& weights,
                            const dnn::BatchDescriptor& input_dimensions,
                            const dnn::BatchDescriptor& output_dimensions,
                            DeviceMemory<float>* output_data) {
  if (input_dimensions.count() != output_dimensions.count()) {
    LOG(ERROR) << "MatMul input and output dimensions are not compatible.";
    return false;
  }

  // We do not permute the input or output, instead we just
  // reinterpret the layout. We are working with row-major matrices
  // and the rows of the input and output correspond to batch, so
  // batch has to be outermost in both the input and output.
  //
  // By adding transposes to the BLAS gemm call we could perhaps make
  // the kYXDepthBatch layout work as well, but there has been no need
  // for that so far.
  if (input_dimensions.layout() != dnn::DataLayout::kBatchYXDepth &&
      input_dimensions.layout() != dnn::DataLayout::kBatchDepthYX) {
    LOG(ERROR) << "Unsupported MatMul input layout.";
    return false;
  }
  if (output_dimensions.layout() != dnn::DataLayout::kBatchYXDepth &&
      output_dimensions.layout() != dnn::DataLayout::kBatchDepthYX) {
    LOG(ERROR) << "Unsupported MatMul output layout.";
    return false;
  }

  if (output_dimensions.width() == 1 && output_dimensions.height() == 1) {
    // This is a fast path that also supports the kBatchYXDepth layout.

    // The matrices here are in row-major format while BLAS expects
    // column-major, i.e. our matrices are transposed as far as BLAS
    // is concerned. So we need to compute output^T =
    // input^T*weights^T. There is no parameter for transposing the
    // output in BLAS gemm, but instead we can transpose both sides of
    // the equality to see that this is equivalent to
    // output=weights*input. So we only need to swap the order of
    // weights and input in the matrix product to correct for the
    // row-major versus column-major difference.
    const float alpha = 1.0f;  // Take the matrix product without scaling it.
    const float beta = 0.0f;   // Ignore the original values in output_data.
    const int64 m = output_dimensions.NodesAcrossFeatureMaps();
    const int64 n = input_dimensions.count();
    const int64 k = input_dimensions.NodesAcrossFeatureMaps();
    stream->ThenBlasGemm(blas::Transpose::kNoTranspose,
                         blas::Transpose::kNoTranspose, m, n, k, alpha, weights,
                         m, input_data, k, beta, output_data, m);
  } else {
    // This is a slower and more complex path that supports output
    // width() * height() > 1, though it only supports the
    // kBatchYXDepth layout. Does support kBatchDepthYX if output
    // feature_map_count() == 1, as then there is no difference
    // between the two layouts.
    //
    // The operation here is the same as above, except that we have to
    // do the matrix multiplication for each (y,x) output coordinate
    // separately. We then interpret weights as containing K = width()
    // * height() different matrices, which we all multiply onto the
    // matrix from input_data, yielding K matrix products. We then
    // combine these together into one matrix by concatenating all the
    // first rows of these matrices, then all the seconds rows and so
    // on. We can do this with a batched matrix multiplication, where
    // the result is written to a different submatrix of the output
    // for each matrix multiplication.
    //
    // The reason that we only support the kBatchYXDepth output layout
    // is that we have to do something in the depth for each (y,x)
    // coordinate. The kBatchYXDepth layout has the depth information
    // for each point (y,x) in contiguous memory while the
    // kBatchDepthYX layout does not.
    //
    // TODO(broune): Consider a special case for when output depth ==
    // 1, as then possibly this could all be done as one matrix
    // multiplication instead of a batched one, which should be
    // faster. Another possibility would be to add a weights layout
    // parameter and then support kBatchDepthYX for a different
    // weights layout.
    if (output_dimensions.layout() != dnn::DataLayout::kBatchYXDepth &&
        !(output_dimensions.layout() == dnn::DataLayout::kBatchDepthYX &&
          output_dimensions.feature_map_count() == 1)) {
      LOG(ERROR) << "Unsupported MatMul output layout.";
      return false;
    }

    const float alpha = 1.0f;  // Take the matrix product without scaling it.
    const float beta = 0.0f;   // Ignore the original values in output_data.
    const uint64 m = output_dimensions.feature_map_count();
    const uint64 n = input_dimensions.count();
    const uint64 k = input_dimensions.NodesAcrossFeatureMaps();
    const int lda = m;
    const int ldb = k;
    const int ldc = output_dimensions.NodesAcrossFeatureMaps();
    const int batch_count = output_dimensions.NodesPerFeatureMap();

    std::vector<DeviceMemory<float>> a(batch_count);
    std::vector<DeviceMemory<float>> b(batch_count);
    std::vector<DeviceMemory<float>> c(batch_count);
    for (int i = 0; i < batch_count; ++i) {
      const int weights_offset = i * input_dimensions.NodesAcrossFeatureMaps() *
                                 output_dimensions.feature_map_count();
      a[i] = DeviceMemory<float>::MakeFromByteSize(
          const_cast<float*>(reinterpret_cast<const float*>(weights.opaque())) +
              weights_offset,
          weights.ElementCount() - weights_offset);

      b[i] = input_data;

      const int output_offset = i * output_dimensions.feature_map_count();
      c[i] = DeviceMemory<float>::MakeFromByteSize(
          const_cast<float*>(
              reinterpret_cast<const float*>(output_data->opaque())) +
              output_offset,
          output_data->ElementCount() - output_offset);
    }
    const auto toPtrs = [](std::vector<DeviceMemory<float>>& v) {
      std::vector<DeviceMemory<float>*> ptrs;
      for (auto& mem : v) {
        ptrs.push_back(&mem);
      }
      return ptrs;
    };

    stream->ThenBlasGemmBatched(blas::Transpose::kNoTranspose,
                                blas::Transpose::kNoTranspose, m, n, k, alpha,
                                toPtrs(a), lda, toPtrs(b), ldb, beta, toPtrs(c),
                                ldc, batch_count);
  }

  return stream->ok();
}

bool CudnnSupport::DoBiasAdd(Stream* stream,
                             const DeviceMemory<float>& input_data,
                             const DeviceMemory<float>& biases,
                             const dnn::BatchDescriptor& dimensions,
                             DeviceMemory<float>* output_data) {
  ScopedTensorDescriptor input_descriptor{parent_, dimensions,
                                          CUDNN_DATA_FLOAT};

  BatchDescriptor bias_dimensions;
  bias_dimensions.set_count(1)
      .set_feature_map_count(dimensions.feature_map_count())
      .set_height(1)
      .set_width(1)
      .set_layout(dnn::DataLayout::kBatchYXDepth);
  ScopedTensorDescriptor bias_descriptor{parent_, bias_dimensions,
                                         CUDNN_DATA_FLOAT};

  // cudnnAddTensor after R3 is in-place, so we need to copy input_data to
  // output_data before doing the addition, unless the input and
  // output are at the same address.
  if (input_data.opaque() != output_data->opaque()) {
    stream->ThenMemcpy(output_data, input_data,
                       dimensions.ElementCount() * sizeof(float));
    if (!stream->ok()) {
      LOG(ERROR)
          << "stream " << stream
          << " could not enqueue a tensor copy as part of bias addition.";
      return false;
    }
  }

  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to set stream for cudnn handle: " << ToString(status);
    return false;
  }

  const float alpha = 1.0f;
  const float beta = 1.0f;

#if CUDNN_VERSION >= 5000
  status = dynload::cudnnAddTensor(
#else
  status = dynload::cudnnAddTensor_v3(
#endif
      parent_, ToHandle(dnn_handle_), &alpha, bias_descriptor.handle(),
      biases.opaque(), &beta, input_descriptor.handle(),
      output_data->opaque());

  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "stream " << stream << " could not enqueue bias addition.";
    return false;
  }

  return true;
}

bool CudnnSupport::DoActivate(Stream* stream,
                              dnn::ActivationMode activation_mode,
                              const dnn::BatchDescriptor& dimensions,
                              const DeviceMemory<float>& input_data,
                              DeviceMemory<float>* output_data) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to set stream for cudnn handle: " << ToString(status);
    return false;
  }

#if CUDNN_VERSION >= 5000
  ScopedActivationDescriptor activation_desc{parent_, activation_mode,
                                             dimensions.value_max()};
#else
  cudnnActivationMode_t mode;
  switch (activation_mode) {
    case dnn::ActivationMode::kRelu6:
      // TODO(leary) should probably do a post-pass to clip at 6?
      LOG(WARNING) << "user requested Relu6, but providing Relu instead";
      mode = CUDNN_ACTIVATION_RELU;
      break;
    case dnn::ActivationMode::kReluX:
      // TODO(broune) should probably do a post-pass to clip at X?
      LOG(WARNING) << "user requested ReluX, but providing Relu instead";
      mode = CUDNN_ACTIVATION_RELU;
      break;
    case dnn::ActivationMode::kRelu:
      mode = CUDNN_ACTIVATION_RELU;
      break;
    case dnn::ActivationMode::kSigmoid:
      mode = CUDNN_ACTIVATION_SIGMOID;
      break;
    case dnn::ActivationMode::kTanh:
      mode = CUDNN_ACTIVATION_TANH;
      break;
    default:
      LOG(ERROR) << "unrecognized activation mode: "
                 << static_cast<int>(activation_mode);
      return false;
  }
#endif

  ScopedTensorDescriptor input_nd{parent_, dimensions, CUDNN_DATA_FLOAT};
  // Alpha is the input scaling factor.
  float alpha = 1.0;
  // Beta is the output scaling factor.
  float beta = 0.0;
  status = dynload::cudnnActivationForward(
      parent_, ToHandle(dnn_handle_),
#if CUDNN_VERSION >= 5000
      activation_desc.handle(),
#else
      mode,
#endif
      &alpha, input_nd.handle(), input_data.opaque(), &beta, input_nd.handle(),
      output_data->opaque());
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "stream " << stream
               << " could not enqueue activation: " << ToString(status);
    return false;
  }

  return true;
}

bool CudnnSupport::DoPoolForward(
    Stream* stream, const dnn::PoolingDescriptor& pooling_dimensions,
    const dnn::BatchDescriptor& input_dimensions,
    const DeviceMemory<float>& input_data,
    const dnn::BatchDescriptor& output_dimensions,
    DeviceMemory<float>* output_data) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to set stream for cudnn handle: " << ToString(status);
    return false;
  }

  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  ScopedTensorDescriptor src_desc{parent_, input_dimensions, CUDNN_DATA_FLOAT};
  ScopedTensorDescriptor dest_desc{parent_, output_dimensions,
                                   CUDNN_DATA_FLOAT};
  ScopedPoolingDescriptor pooling_desc{parent_, pooling_dimensions};
  status = dynload::cudnnPoolingForward(
      parent_, ToHandle(dnn_handle_), pooling_desc.handle(), &alpha,
      src_desc.handle(), input_data.opaque(), &beta, dest_desc.handle(),
      output_data->opaque());
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to enqueue forward pooling on stream: "
               << ToString(status);
    return false;
  }
  return true;
}

bool CudnnSupport::DoPoolForward(
    Stream* stream, const dnn::PoolingDescriptor& pooling_dimensions,
    const dnn::BatchDescriptor& input_dimensions,
    const DeviceMemory<Eigen::half>& input_data,
    const dnn::BatchDescriptor& output_dimensions,
    DeviceMemory<Eigen::half>* output_data) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to set stream for cudnn handle: " << ToString(status);
    return false;
  }

  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  ScopedTensorDescriptor src_desc{parent_, input_dimensions, CUDNN_DATA_HALF};
  ScopedTensorDescriptor dest_desc{parent_, output_dimensions, CUDNN_DATA_HALF};
  ScopedPoolingDescriptor pooling_desc{parent_, pooling_dimensions};
  status = dynload::cudnnPoolingForward(
      parent_, ToHandle(dnn_handle_), pooling_desc.handle(), &alpha,
      src_desc.handle(), input_data.opaque(), &beta, dest_desc.handle(),
      output_data->opaque());
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to enqueue forward pooling on stream: "
               << ToString(status);
    return false;
  }
  return true;
}

bool CudnnSupport::DoPoolBackward(
    Stream* stream, const dnn::PoolingDescriptor& pooling_dimensions,
    const dnn::BatchDescriptor& input_dimensions,
    const DeviceMemory<float>& input_data,
    const dnn::BatchDescriptor& output_dimensions,
    const DeviceMemory<float>& output_data,
    const DeviceMemory<float>& input_diff_data,
    DeviceMemory<float>* output_diff_data) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to set stream for cudnn handle: " << ToString(status);
    return false;
  }

  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  ScopedTensorDescriptor src_desc{parent_, input_dimensions, CUDNN_DATA_FLOAT};
  ScopedTensorDescriptor dest_desc{parent_, output_dimensions,
                                   CUDNN_DATA_FLOAT};
  ScopedPoolingDescriptor pooling_desc{parent_, pooling_dimensions};
  status = dynload::cudnnPoolingBackward(
      parent_, ToHandle(dnn_handle_), pooling_desc.handle(), &alpha,
      dest_desc.handle(), output_data.opaque(), dest_desc.handle(),
      input_diff_data.opaque(), src_desc.handle(), input_data.opaque(), &beta,
      src_desc.handle(), output_diff_data->opaque());
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to enqueue backward pooling on stream: "
               << ToString(status);
    return false;
  }
  return true;
}

bool CudnnSupport::DoPoolBackward(
    Stream* stream, const dnn::PoolingDescriptor& pooling_dimensions,
    const dnn::BatchDescriptor& input_dimensions,
    const DeviceMemory<Eigen::half>& input_data,
    const dnn::BatchDescriptor& output_dimensions,
    const DeviceMemory<Eigen::half>& output_data,
    const DeviceMemory<Eigen::half>& input_diff_data,
    DeviceMemory<Eigen::half>* output_diff_data) {
  mutex_lock lock{dnn_handle_mutex_};
  auto status = dynload::cudnnSetStream(parent_, ToHandle(dnn_handle_),
                                        AsCUDAStreamValue(stream));
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to set stream for cudnn handle: " << ToString(status);
    return false;
  }

  // Alpha is the scaling factor for input.
  float alpha = 1.0;
  // Beta is the scaling factor for output.
  float beta = 0.0;

  ScopedTensorDescriptor src_desc{parent_, input_dimensions, CUDNN_DATA_HALF};
  ScopedTensorDescriptor dest_desc{parent_, output_dimensions, CUDNN_DATA_HALF};
  ScopedPoolingDescriptor pooling_desc{parent_, pooling_dimensions};
  status = dynload::cudnnPoolingBackward(
      parent_, ToHandle(dnn_handle_), pooling_desc.handle(), &alpha,
      dest_desc.handle(), output_data.opaque(), dest_desc.handle(),
      input_diff_data.opaque(), src_desc.handle(), input_data.opaque(), &beta,
      src_desc.handle(), output_diff_data->opaque());
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "failed to enqueue backward pooling on stream: "
               << ToString(status);
    return false;
  }
  return true;
}

bool CudnnSupport::DoNormalize(
    Stream* stream, const dnn::NormalizeDescriptor& normalize_descriptor,
    const DeviceMemory<float>& input_data, DeviceMemory<float>* output_data) {
  LOG(FATAL) << "not yet implemented";  // TODO(leary)
  return false;
}

bool CudnnSupport::DoDepthConcatenate(
    Stream* stream, port::ArraySlice<dnn::BatchDescriptor> input_dimensions,
    port::ArraySlice<const DeviceMemory<float>*> input_data,
    DeviceMemory<float>* output_data) {
  CHECK_EQ(input_dimensions.size(), input_data.size());

  for (const auto& dimensions : input_dimensions) {
    if (dimensions.layout() != dnn::DataLayout::kBatchDepthYX) {
      LOG(ERROR) << "CudnnSupport::DoDepthConcatenate currently only "
                    "supports the kBatchDepthYX layout.";
      return false;
    }
  }

  if (input_dimensions.empty()) {
    return true;  // Nothing to do.
  }

  dnn::BatchDescriptor output_dimensions =
      dnn::BatchDescriptor::DepthConcatenateOutputDescriptor(input_dimensions);

  const int64 area = output_dimensions.width() * output_dimensions.height();
  const auto index = [area](int64 batch, int64 depth, int64 yx,
                            int64 max_depth) {
    return (batch * max_depth + depth) * area + yx;
  };

  std::vector<float> output_host(output_dimensions.ElementCount());
  std::vector<float> tmp;
  int64 depth_sum = 0;
  for (size_t i = 0; i < input_data.size(); ++i) {
    const auto& dimensions = input_dimensions[i];
    tmp.resize(dimensions.ElementCount());
    stream->ThenMemcpyD2H<float>(*input_data[i], &tmp).BlockHostUntilDone();

    for (int64 batch = 0; batch < output_dimensions.count(); ++batch) {
      for (int64 yx = 0; yx < area; ++yx) {
        for (int64 depth = 0; depth < dimensions.feature_map_count(); ++depth) {
          LOG(INFO) << output_dimensions.ElementCount() << ' ' << batch << ' '
                    << yx << ' ' << depth;
          output_host[index(batch, depth + depth_sum, yx,
                            output_dimensions.feature_map_count())] =
              tmp[index(batch, depth, yx, dimensions.feature_map_count())];
        }
      }
    }
    depth_sum += dimensions.feature_map_count();
  }
  stream->ThenMemcpyH2D<float>(output_host, output_data);
  return true;
}

bool CudnnSupport::DoElementwiseOperate(
    Stream* stream, dnn::ElementwiseOperation operation,
    port::ArraySlice<dnn::BatchDescriptor> input_dimensions,
    port::ArraySlice<const DeviceMemory<float>*> input_data,
    const dnn::BatchDescriptor& output_dimensions,
    DeviceMemory<float>* output_data) {
  LOG(FATAL) << "not yet implemented";  // TODO(leary)
  return false;
}

bool CudnnSupport::DoXYPad(Stream* stream,
                           const dnn::BatchDescriptor& dimensions,
                           const DeviceMemory<float>& input_data,
                           int64 left_pad, int64 right_pad, int64 top_pad,
                           int64 bottom_pad, DeviceMemory<float>* output_data) {
  LOG(FATAL) << "not yet implemented";  // TODO(leary)
  return false;
}

bool CudnnSupport::DoXYSlice(Stream* stream,
                             const dnn::BatchDescriptor& dimensions,
                             const DeviceMemory<float>& input_data,
                             int64 left_trim, int64 right_trim, int64 top_trim,
                             int64 bottom_trim,
                             DeviceMemory<float>* output_data) {
  LOG(FATAL) << "not yet implemented";  // TODO(leary)
  return false;
}

bool CudnnSupport::DoMemcpyD2HQuantized(
    Stream* stream, const DeviceMemory<float>& gpu_unquantized_src,
    dnn::QuantizedActivationMode mode, void* host_dst, int64 size) {
  LOG(ERROR) << "quantized memcpy not supported by cuDNN";
  return false;
}

bool CudnnSupport::DoMemcpyH2DQuantized(
    Stream* stream, const void* host_src, int64 size,
    dnn::QuantizedActivationMode mode,
    DeviceMemory<float>* gpu_unquantized_dst) {
  LOG(ERROR) << "quantized memcpy not supported by cuDNN";
  return false;
}

bool CudnnSupport::DeriveOutputBatchDescriptor(
    const BatchDescriptor& batch_descriptor,
    const FilterDescriptor& filter_descriptor,
    const dnn::ConvolutionDescriptor& convolution_descriptor,
    dnn::BatchDescriptor* output_batch_descriptor) {
  ScopedTensorDescriptor input_nd{parent_, batch_descriptor, CUDNN_DATA_FLOAT};
  ScopedFilterDescriptor filter{parent_, filter_descriptor, batch_descriptor,
                                CUDNN_DATA_FLOAT};
  ScopedConvolutionDescriptor conv{parent_, convolution_descriptor,
                                   CUDNN_DATA_FLOAT};

  int dn = batch_descriptor.ndims() + 2;
  std::vector<int> dims(dn);  // in BDYX
  auto status = dynload::cudnnGetConvolutionNdForwardOutputDim(
      parent_, conv.handle(), input_nd.handle(), filter.handle(), dn,
      dims.data());
  if (status != CUDNN_STATUS_SUCCESS) {
    LOG(ERROR) << "could not get output tensor for convolution: "
               << ToString(status);
    return false;
  }

  output_batch_descriptor->set_count(dims[0])
      .set_feature_map_count(dims[1])
      .set_layout(batch_descriptor.layout());

  for (int i = 0; i < batch_descriptor.ndims(); i++) {
    output_batch_descriptor->set_spatial_dim(static_cast<dnn::DimIndex>(i),
                                             dims.rbegin()[i]);
  }

  return true;
}

}  // namespace cuda

namespace gpu = ::perftools::gputools;

void initialize_cudnn() {
  gpu::port::Status status =
      gpu::PluginRegistry::Instance()
          ->RegisterFactory<gpu::PluginRegistry::DnnFactory>(
              gpu::cuda::kCudaPlatformId, gpu::cuda::kCuDnnPlugin, "cuDNN",
              [](gpu::internal::StreamExecutorInterface*
                     parent) -> gpu::dnn::DnnSupport* {
                gpu::cuda::CUDAExecutor* cuda_executor =
                    dynamic_cast<gpu::cuda::CUDAExecutor*>(parent);
                if (cuda_executor == nullptr) {
                  LOG(ERROR)
                      << "Attempting to initialize an instance of the cuBLAS "
                      << "support library with a non-CUDA StreamExecutor";
                  return nullptr;
                }

                gpu::cuda::CudnnSupport* dnn =
                    new gpu::cuda::CudnnSupport(cuda_executor);
                if (!dnn->Init().ok()) {
                  // Note: Init() will log a more specific error.
                  delete dnn;
                  return nullptr;
                }
                return dnn;
              });

  if (!status.ok()) {
    LOG(ERROR) << "Unable to register cuDNN factory: "
               << status.error_message();
  }

  // Prime the cuDNN DSO. The loader will log more information.
  auto statusor = gpu::internal::CachedDsoLoader::GetCudnnDsoHandle();
  if (!statusor.ok()) {
    LOG(INFO) << "Unable to load cuDNN DSO";
  }

  gpu::PluginRegistry::Instance()->SetDefaultFactory(gpu::cuda::kCudaPlatformId,
                                                     gpu::PluginKind::kDnn,
                                                     gpu::cuda::kCuDnnPlugin);
}

}  // namespace gputools
}  // namespace perftools

REGISTER_MODULE_INITIALIZER(register_cudnn,
                            { perftools::gputools::initialize_cudnn(); });
