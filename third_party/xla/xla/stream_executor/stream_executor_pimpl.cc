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

// Implements the StreamExecutor interface by passing through to its
// implementation_ value (in pointer-to-implementation style), which
// implements StreamExecutorInterface.

#include "xla/stream_executor/stream_executor_pimpl.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/base/const_init.h"
#include "absl/functional/any_invocable.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/notification.h"
#include "xla/stream_executor/blas.h"
#include "xla/stream_executor/command_buffer.h"
#include "xla/stream_executor/fft.h"
#include "xla/stream_executor/platform/port.h"
#include "xla/stream_executor/stream.h"
#include "xla/stream_executor/stream_executor_internal.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/stacktrace.h"
#include "tsl/platform/statusor.h"
#include "tsl/platform/threadpool.h"
#include "tsl/util/env_var.h"

namespace stream_executor {
namespace {

std::string StackTraceIfVLOG10() {
  if (VLOG_IS_ON(10)) {
    return absl::StrCat(" ", tsl::CurrentStackTrace(), "\n");
  } else {
    return "";
  }
}

// Make sure the executor is done with its work; we know (because this isn't
// publicly visible) that all enqueued work is quick.
void BlockOnThreadExecutor(tsl::thread::ThreadPool* executor) {
  absl::Notification n;
  executor->Schedule([&n]() { n.Notify(); });
  n.WaitForNotification();
}

std::atomic_int_fast64_t correlation_id_generator(0);

}  // namespace

template <typename BeginCallT, typename CompleteCallT, typename ReturnT,
          typename... BeginArgsT>
class ScopedTracer {
 public:
  ScopedTracer(StreamExecutor* stream_exec, BeginCallT begin_call,
               CompleteCallT complete_call, const ReturnT* result,
               BeginArgsT... begin_args)
      : stream_exec_(stream_exec),
        complete_call_(complete_call),
        result_(result) {
    if (stream_exec_->tracing_enabled_) {
      correlation_id_ =
          correlation_id_generator.fetch_add(1, std::memory_order_relaxed) - 1;
      Trace(begin_call, begin_args...);
    }
  }

  ~ScopedTracer() {
    if (stream_exec_->tracing_enabled_) {
      Trace(complete_call_, result_);
    }
  }

 private:
  template <typename CallbackT, typename... TraceArgsT>
  void Trace(CallbackT callback, TraceArgsT... args) {
    {
      // Instance tracers held in a block to limit the lock lifetime.
      absl::ReaderMutexLock lock{&stream_exec_->mu_};
      for (TraceListener* listener : stream_exec_->listeners_) {
        (listener->*callback)(correlation_id_,
                              std::forward<TraceArgsT>(args)...);
      }
    }
  }

  StreamExecutor* stream_exec_;
  CompleteCallT complete_call_;
  const ReturnT* result_;
  int64_t correlation_id_;
};

template <typename BeginCallT, typename CompleteCallT, typename ReturnT,
          typename... BeginArgsT>
ScopedTracer<BeginCallT, CompleteCallT, ReturnT, BeginArgsT...>
MakeScopedTracer(StreamExecutor* stream_exec, BeginCallT begin_call,
                 CompleteCallT complete_call, ReturnT* result,
                 BeginArgsT... begin_args) {
  return ScopedTracer<BeginCallT, CompleteCallT, ReturnT, BeginArgsT...>(
      stream_exec, begin_call, complete_call, result,
      std::forward<BeginArgsT>(begin_args)...);
}

#define SCOPED_TRACE(LOC, ...) \
  auto tracer =                \
      MakeScopedTracer(this, &LOC##Begin, &LOC##Complete, ##__VA_ARGS__);

/* static */ absl::Mutex StreamExecutor::static_mu_{absl::kConstInit};

// Get per-device memory limit in bytes. Returns 0 if
// TF_PER_DEVICE_MEMORY_LIMIT_MB environment variable is not set.
static int64_t GetMemoryLimitBytes() {
  int64_t value;
  TF_CHECK_OK(
      tsl::ReadInt64FromEnvVar("TF_PER_DEVICE_MEMORY_LIMIT_MB", 0, &value));
  return value * (1ll << 20);
}

StreamExecutor::StreamExecutor(
    const Platform* platform,
    std::unique_ptr<internal::StreamExecutorInterface> implementation,
    int device_ordinal)
    : platform_(platform),
      implementation_(std::move(implementation)),
      device_ordinal_(device_ordinal),
      background_threads_(new tsl::thread::ThreadPool(
          tsl::Env::Default(), "stream_executor", kNumBackgroundThreads)),
      live_stream_count_(0),
      tracing_enabled_(false),
      memory_limit_bytes_(GetMemoryLimitBytes()),
      allocator_(this) {}

StreamExecutor::~StreamExecutor() {
  BlockOnThreadExecutor(background_threads_.get());

  if (live_stream_count_.load() != 0) {
    LOG(WARNING) << "Not all streams were deallocated at executor destruction "
                 << "time. This may lead to unexpected/bad behavior - "
                 << "especially if any stream is still active!";
  }
}

StreamExecutor::PlatformSpecificHandle
StreamExecutor::platform_specific_handle() const {
  PlatformSpecificHandle handle;
  handle.context = implementation_->platform_specific_context();
  return handle;
}

tsl::Status StreamExecutor::Init(DeviceOptions device_options) {
  TF_RETURN_IF_ERROR(
      implementation_->Init(device_ordinal_, std::move(device_options)));
  return ::tsl::OkStatus();
}

tsl::Status StreamExecutor::Init() { return Init(DeviceOptions::Default()); }

tsl::Status StreamExecutor::GetKernel(const MultiKernelLoaderSpec& spec,
                                      Kernel* kernel) {
  return implementation_->GetKernel(spec, kernel);
}

void StreamExecutor::UnloadKernel(const Kernel* kernel) {
  implementation_->UnloadKernel(kernel);
}

tsl::Status StreamExecutor::LoadModule(const MultiModuleLoaderSpec& spec,
                                       ModuleHandle* module_handle) {
  return implementation_->LoadModule(spec, module_handle);
}

bool StreamExecutor::UnloadModule(ModuleHandle module_handle) {
  return implementation_->UnloadModule(module_handle);
}

tsl::StatusOr<std::shared_ptr<DeviceMemoryBase>>
StreamExecutor::CreateOrShareConstant(Stream* stream,
                                      const std::vector<uint8_t>& content) {
  return implementation_->CreateOrShareConstant(stream, std::move(content));
}

void StreamExecutor::Deallocate(DeviceMemoryBase* mem) {
  VLOG(1) << "Called StreamExecutor::Deallocate(mem=" << mem->opaque()
          << ") mem->size()=" << mem->size() << StackTraceIfVLOG10();

  implementation_->Deallocate(mem);
  mem->Reset(nullptr, 0);
}

bool StreamExecutor::CanEnablePeerAccessTo(StreamExecutor* other) {
  return implementation_->CanEnablePeerAccessTo(other->implementation_.get());
}

tsl::Status StreamExecutor::EnablePeerAccessTo(StreamExecutor* other) {
  return implementation_->EnablePeerAccessTo(other->implementation_.get());
}

const DeviceDescription& StreamExecutor::GetDeviceDescription() const {
  absl::MutexLock lock(&mu_);
  if (device_description_ != nullptr) {
    return *device_description_;
  }

  device_description_ = CreateDeviceDescription();
  return *device_description_;
}

int64_t StreamExecutor::GetDeviceLoad() const {
  return implementation_->GetDeviceLoad();
}

tsl::Status StreamExecutor::GetConvolveRunners(
    bool use_cudnn_frontend, dnn::ConvolutionKind kind,
    dnn::DataType input_type, dnn::DataType output_type, Stream* stream,
    const dnn::BatchDescriptor& input_descriptor, DeviceMemoryBase input_data,
    const dnn::FilterDescriptor& filter_descriptor,
    DeviceMemoryBase filter_data, const dnn::BatchDescriptor& output_descriptor,
    DeviceMemoryBase output_data,
    const dnn::ConvolutionDescriptor& convolution_descriptor, bool use_fallback,
    ScratchAllocator* scratch_allocator, const NumericOptions& numeric_options,
    std::vector<std::unique_ptr<const dnn::ConvRunner>>* out_exec_plans) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::errors::Unimplemented("DNN library is not found.");
  }
  return dnn_support->GetConvolveRunners(
      use_cudnn_frontend, kind, input_type, output_type, stream,
      input_descriptor, input_data, filter_descriptor, filter_data,
      output_descriptor, output_data, convolution_descriptor, use_fallback,
      scratch_allocator, numeric_options, out_exec_plans);
}

tsl::Status StreamExecutor::GetGraphConvolveRunners(
    dnn::ConvolutionKind kind, dnn::DataType input_type,
    dnn::DataType output_type, Stream* stream,
    const dnn::BatchDescriptor& input_descriptor,
    const dnn::FilterDescriptor& filter_descriptor,
    const dnn::BatchDescriptor& output_descriptor,
    const dnn::ConvolutionDescriptor& convolution_descriptor, bool use_fallback,
    const NumericOptions& numeric_options,
    std::vector<std::unique_ptr<const dnn::GraphConvRunner>>* out_exec_plans,
    std::string serialized_graph) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::errors::Unimplemented("DNN library is not found.");
  }
  return dnn_support->GetGraphConvolveRunners(
      kind, input_type, output_type, stream, input_descriptor,
      filter_descriptor, output_descriptor, convolution_descriptor,
      use_fallback, numeric_options, out_exec_plans, serialized_graph);
}

tsl::Status StreamExecutor::GetFusedConvolveRunners(
    bool use_cudnn_frontend, dnn::ConvolutionKind kind,
    dnn::DataType input_type, dnn::DataType bias_type,
    dnn::DataType output_type, double conv_input_scale, double side_input_scale,
    double leakyrelu_alpha, Stream* stream,
    const dnn::BatchDescriptor& input_descriptor,
    const dnn::FilterDescriptor& filter_descriptor,
    const dnn::BatchDescriptor& bias_descriptor,
    const dnn::BatchDescriptor& output_descriptor,
    const dnn::ConvolutionDescriptor& convolution_descriptor, bool use_fallback,
    dnn::ActivationMode activation_mode, const NumericOptions& numeric_options,
    std::vector<std::unique_ptr<const dnn::FusedConvRunner>>* out_exec_plans) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::errors::Unimplemented("DNN library is not found.");
  }
  return dnn_support->GetFusedConvolveRunners(
      use_cudnn_frontend, kind, input_type, bias_type, output_type,
      conv_input_scale, side_input_scale, leakyrelu_alpha, stream,
      input_descriptor, filter_descriptor, bias_descriptor, output_descriptor,
      convolution_descriptor, use_fallback, activation_mode, numeric_options,
      out_exec_plans);
}

tsl::Status StreamExecutor::GetFusedMatmulRunners(
    bool use_cudnn_frontend, dnn::DataType input_type, dnn::DataType bias_type,
    dnn::DataType output_type, Stream* stream, bool trans_a, bool trans_b,
    uint64_t m, uint64_t n, uint64_t k, int64_t lda, int64_t ldb, int64_t ldc,
    dnn::ActivationMode activation_mode, bool use_fallback,
    const NumericOptions& numeric_options,
    std::vector<std::unique_ptr<const dnn::FusedMatmulRunner>>*
        out_exec_plans) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::errors::Unimplemented("DNN library is not found.");
  }

  return dnn_support->GetFusedMatmulRunners(
      use_cudnn_frontend, input_type, bias_type, output_type, stream, trans_a,
      trans_b, m, n, k, lda, ldb, ldc, activation_mode, use_fallback,
      numeric_options, out_exec_plans);
}

bool StreamExecutor::GetMIOpenConvolveAlgorithms(
    dnn::ConvolutionKind kind, dnn::DataType element_type, Stream* stream,
    const dnn::BatchDescriptor& input_descriptor, DeviceMemoryBase input_data,
    const dnn::FilterDescriptor& filter_descriptor,
    DeviceMemoryBase filter_data, const dnn::BatchDescriptor& output_descriptor,
    DeviceMemoryBase output_data,
    const dnn::ConvolutionDescriptor& convolution_descriptor,
    ScratchAllocator* scratch_allocator,
    std::vector<dnn::ProfileResult>* out_algorithms) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return false;
  }
  return dnn_support->GetMIOpenConvolveAlgorithms(
      kind, element_type, stream, input_descriptor, input_data,
      filter_descriptor, filter_data, output_descriptor, output_data,
      convolution_descriptor, scratch_allocator, out_algorithms);
}

bool StreamExecutor::GetRnnAlgorithms(
    std::vector<dnn::AlgorithmDesc>* out_algorithms) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return false;
  }
  return dnn_support->GetRnnAlgorithms(out_algorithms);
}

bool StreamExecutor::GetBlasGemmAlgorithms(
    Stream* stream, std::vector<blas::AlgorithmType>* out_algorithms) {
  blas::BlasSupport* blas_support = AsBlas();
  if (!blas_support) {
    return false;
  }
  return blas_support->GetBlasGemmAlgorithms(stream, out_algorithms);
}

tsl::StatusOr<std::unique_ptr<dnn::RnnDescriptor>>
StreamExecutor::createRnnDescriptor(
    int num_layers, int hidden_size, int input_size, int cell_size,
    int batch_size, dnn::RnnInputMode input_mode,
    dnn::RnnDirectionMode direction_mode, dnn::RnnMode rnn_mode,
    dnn::DataType data_type, const dnn::AlgorithmConfig& algorithm_config,
    const NumericOptions& numeric_options, float dropout, uint64_t seed,
    ScratchAllocator* state_allocator, bool use_padded_io) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::Status(absl::StatusCode::kUnknown,
                       "Fail to find the dnn implementation.");
  }
  return dnn_support->createRnnDescriptor(
      num_layers, hidden_size, input_size, cell_size, batch_size, input_mode,
      direction_mode, rnn_mode, data_type, algorithm_config, numeric_options,
      dropout, seed, state_allocator, use_padded_io);
}

tsl::StatusOr<std::unique_ptr<dnn::RnnSequenceTensorDescriptor>>
StreamExecutor::createRnnSequenceTensorDescriptor(int max_seq_length,
                                                  int batch_size, int data_size,
                                                  dnn::DataType data_type) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::Status(absl::StatusCode::kUnknown,
                       "Fail to find the dnn implementation.");
  }
  return dnn_support->createRnnSequenceTensorDescriptor(
      max_seq_length, batch_size, data_size, data_type);
}

tsl::StatusOr<std::unique_ptr<dnn::RnnSequenceTensorDescriptor>>
StreamExecutor::createRnnSequenceTensorDescriptor(
    int max_seq_length, int batch_size, int data_size,
    const absl::Span<const int>& seq_lengths, bool time_major,
    dnn::DataType data_type) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::Status(absl::StatusCode::kUnknown,
                       "Fail to find the dnn implementation.");
  }
  return dnn_support->createRnnSequenceTensorDescriptor(
      max_seq_length, batch_size, data_size, seq_lengths, time_major,
      data_type);
}

tsl::StatusOr<std::unique_ptr<dnn::RnnStateTensorDescriptor>>
StreamExecutor::createRnnStateTensorDescriptor(int num_layer, int batch_size,
                                               int data_size,
                                               dnn::DataType data_type) {
  dnn::DnnSupport* dnn_support = AsDnn();
  if (!dnn_support) {
    return tsl::Status(absl::StatusCode::kUnknown,
                       "Fail to find the dnn implementation.");
  }
  return dnn_support->createRnnStateTensorDescriptor(num_layer, batch_size,
                                                     data_size, data_type);
}

dnn::DnnSupport* StreamExecutor::AsDnn() {
  absl::MutexLock lock(&mu_);
  if (dnn_ != nullptr) {
    return dnn_.get();
  }

  dnn_.reset(implementation_->CreateDnn());
  return dnn_.get();
}

blas::BlasSupport* StreamExecutor::AsBlas() {
  absl::MutexLock lock(&mu_);
  if (blas_ != nullptr) {
    return blas_.get();
  }

  blas_.reset(implementation_->CreateBlas());
  return blas_.get();
}

fft::FftSupport* StreamExecutor::AsFft() {
  absl::MutexLock lock(&mu_);
  if (fft_ != nullptr) {
    return fft_.get();
  }

  fft_.reset(implementation_->CreateFft());
  return fft_.get();
}

tsl::Status StreamExecutor::Launch(Stream* stream, const ThreadDim& thread_dims,
                                   const BlockDim& block_dims,
                                   const Kernel& kernel,
                                   const KernelArgs& args) {
  SubmitTrace(&TraceListener::LaunchSubmit, stream, thread_dims, block_dims,
              kernel, args);

  return implementation_->Launch(stream, thread_dims, block_dims, kernel, args);
}

tsl::Status StreamExecutor::Submit(Stream* stream,
                                   const CommandBuffer& command_buffer) {
  return implementation_->Submit(stream, command_buffer);
}

tsl::Status StreamExecutor::BlockHostUntilDone(Stream* stream) {
  tsl::Status result;
  SCOPED_TRACE(TraceListener::BlockHostUntilDone, &result, stream);

  result = implementation_->BlockHostUntilDone(stream);
  return result;
}

tsl::Status StreamExecutor::GetStatus(Stream* stream) {
  return implementation_->GetStatus(stream);
}

DeviceMemoryBase StreamExecutor::Allocate(uint64_t size, int64_t memory_space) {
  if (memory_limit_bytes_ > 0 &&
      static_cast<int64_t>(size) > memory_limit_bytes_) {
    LOG(WARNING) << "Not enough memory to allocate " << size << " on device "
                 << device_ordinal_
                 << " within provided limit.  limit=" << memory_limit_bytes_
                 << "]";
    return DeviceMemoryBase();
  }
  DeviceMemoryBase buf = implementation_->Allocate(size, memory_space);
  VLOG(1) << "Called StreamExecutor::Allocate(size=" << size
          << ", memory_space=" << memory_space << ") returns " << buf.opaque()
          << StackTraceIfVLOG10();

  return buf;
}

void* StreamExecutor::GetUntypedSubBuffer(DeviceMemoryBase* parent,
                                          uint64_t offset, uint64_t size) {
  return implementation_->GetSubBuffer(parent, offset, size);
}

tsl::StatusOr<DeviceMemoryBase> StreamExecutor::GetUntypedSymbol(
    const std::string& symbol_name, ModuleHandle module_handle) {
  // If failed to get the symbol, opaque/bytes are unchanged. Initialize them to
  // be nullptr/0 for consistency with DeviceMemory semantics.
  void* opaque = nullptr;
  size_t bytes = 0;
  if (GetSymbol(symbol_name, module_handle, &opaque, &bytes)) {
    return DeviceMemoryBase(opaque, bytes);
  }

  return tsl::Status(
      absl::StatusCode::kNotFound,
      absl::StrCat("Check if module containing symbol ", symbol_name,
                   " is loaded (module_handle = ",
                   reinterpret_cast<uintptr_t>(module_handle.id()), ")"));
}

bool StreamExecutor::GetSymbol(const std::string& symbol_name,
                               ModuleHandle module_handle, void** mem,
                               size_t* bytes) {
  return implementation_->GetSymbol(symbol_name, module_handle, mem, bytes);
}

void* StreamExecutor::UnifiedMemoryAllocate(uint64_t bytes) {
  void* buffer = implementation_->UnifiedMemoryAllocate(bytes);
  VLOG(1) << "Called StreamExecutor::UnifiedMemoryAllocate(size=" << bytes
          << ") returns " << buffer << StackTraceIfVLOG10();
  return buffer;
}

void StreamExecutor::UnifiedMemoryDeallocate(void* location) {
  VLOG(1) << "Called StreamExecutor::UnifiedMemoryDeallocate(location="
          << location << ")" << StackTraceIfVLOG10();

  return implementation_->UnifiedMemoryDeallocate(location);
}

void* StreamExecutor::HostMemoryAllocate(uint64_t size) {
  void* buffer = implementation_->HostMemoryAllocate(size);
  VLOG(1) << "Called StreamExecutor::HostMemoryAllocate(size=" << size
          << ") returns " << buffer << StackTraceIfVLOG10();
  return buffer;
}

void StreamExecutor::HostMemoryDeallocate(void* location) {
  VLOG(1) << "Called StreamExecutor::HostMemoryDeallocate(location=" << location
          << ")" << StackTraceIfVLOG10();

  return implementation_->HostMemoryDeallocate(location);
}

bool StreamExecutor::SynchronizeAllActivity() {
  VLOG(1) << "Called StreamExecutor::SynchronizeAllActivity()"
          << StackTraceIfVLOG10();
  bool ok = implementation_->SynchronizeAllActivity();

  // This should all be quick and infallible work, so we can perform the
  // synchronization even in the case of failure.
  BlockOnThreadExecutor(background_threads_.get());

  return ok;
}

tsl::Status StreamExecutor::SynchronousMemZero(DeviceMemoryBase* location,
                                               uint64_t size) {
  VLOG(1) << "Called StreamExecutor::SynchronousMemZero(location=" << location
          << ", size=" << size << ")" << StackTraceIfVLOG10();

  return implementation_->SynchronousMemZero(location, size);
}

bool StreamExecutor::SynchronousMemcpy(DeviceMemoryBase* device_dst,
                                       const DeviceMemoryBase& device_src,
                                       uint64_t size) {
  VLOG(1) << "Called StreamExecutor::SynchronousMemcpy(device_dst="
          << device_dst->opaque() << ", device_src=" << device_src.opaque()
          << ", size=" << size << ") D2D" << StackTraceIfVLOG10();

  tsl::Status status = implementation_->SynchronousMemcpyDeviceToDevice(
      device_dst, device_src, size);
  if (!status.ok()) {
    LOG(ERROR) << "synchronous memcpy: " << status;
  }
  return status.ok();
}

tsl::Status StreamExecutor::SynchronousMemcpyD2H(
    const DeviceMemoryBase& device_src, int64_t size, void* host_dst) {
  VLOG(1) << "Called StreamExecutor::SynchronousMemcpyD2H(device_src="
          << device_src.opaque() << ", size=" << size
          << ", host_dst=" << host_dst << ")" << StackTraceIfVLOG10();

  tsl::Status result;
  SCOPED_TRACE(TraceListener::SynchronousMemcpyD2H, &result, device_src, size,
               host_dst);

  result = implementation_->SynchronousMemcpy(host_dst, device_src, size);
  if (!result.ok()) {
    result = tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrFormat("failed to synchronously memcpy device-to-host: device "
                        "%p to host %p size %d: %s",
                        device_src.opaque(), host_dst, size,
                        result.ToString()));
  }

  return result;
}

tsl::Status StreamExecutor::SynchronousMemcpyH2D(const void* host_src,
                                                 int64_t size,
                                                 DeviceMemoryBase* device_dst) {
  VLOG(1) << "Called StreamExecutor::SynchronousMemcpyH2D(host_src=" << host_src
          << ", size=" << size << ", device_dst=" << device_dst->opaque() << ")"
          << StackTraceIfVLOG10();

  tsl::Status result;
  SCOPED_TRACE(TraceListener::SynchronousMemcpyH2D, &result, host_src, size,
               device_dst);

  result = implementation_->SynchronousMemcpy(device_dst, host_src, size);
  if (!result.ok()) {
    result = tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrFormat("failed to synchronously memcpy host-to-device: host "
                        "%p to device %p size %d: %s",
                        host_src, device_dst->opaque(), size,
                        result.ToString()));
  }

  return result;
}

bool StreamExecutor::Memcpy(Stream* stream, void* host_dst,
                            const DeviceMemoryBase& device_src, uint64_t size) {
  return implementation_->Memcpy(stream, host_dst, device_src, size);
}

bool StreamExecutor::Memcpy(Stream* stream, DeviceMemoryBase* device_dst,
                            const void* host_src, uint64_t size) {
  return implementation_->Memcpy(stream, device_dst, host_src, size);
}

bool StreamExecutor::MemcpyDeviceToDevice(Stream* stream,
                                          DeviceMemoryBase* device_dst,
                                          const DeviceMemoryBase& device_src,
                                          uint64_t size) {
  return implementation_->MemcpyDeviceToDevice(stream, device_dst, device_src,
                                               size);
}

tsl::Status StreamExecutor::MemZero(Stream* stream, DeviceMemoryBase* location,
                                    uint64_t size) {
  return implementation_->MemZero(stream, location, size);
}

tsl::Status StreamExecutor::Memset32(Stream* stream, DeviceMemoryBase* location,
                                     uint32_t pattern, uint64_t size) {
  CHECK_EQ(0, size % 4)
      << "need 32-bit multiple size to fill with 32-bit pattern";
  return implementation_->Memset32(stream, location, pattern, size);
}

bool StreamExecutor::HostCallback(
    Stream* stream, absl::AnyInvocable<tsl::Status() &&> callback) {
  return implementation_->HostCallback(stream, std::move(callback));
}

tsl::Status StreamExecutor::AllocateEvent(Event* event) {
  return implementation_->AllocateEvent(event);
}

tsl::Status StreamExecutor::DeallocateEvent(Event* event) {
  return implementation_->DeallocateEvent(event);
}

tsl::Status StreamExecutor::RecordEvent(Stream* stream, Event* event) {
  return implementation_->RecordEvent(stream, event);
}

tsl::Status StreamExecutor::WaitForEvent(Stream* stream, Event* event) {
  return implementation_->WaitForEvent(stream, event);
}

tsl::Status StreamExecutor::WaitForEventOnExternalStream(std::intptr_t stream,
                                                         Event* event) {
  return implementation_->WaitForEventOnExternalStream(stream, event);
}

Event::Status StreamExecutor::PollForEventStatus(Event* event) {
  return implementation_->PollForEventStatus(event);
}

bool StreamExecutor::AllocateStream(Stream* stream) {
  live_stream_count_.fetch_add(1, std::memory_order_relaxed);
  if (!implementation_->AllocateStream(stream)) {
    auto count = live_stream_count_.fetch_sub(1);
    CHECK_GE(count, 0) << "live stream count should not dip below zero";
    LOG(INFO) << "failed to allocate stream; live stream count: " << count;
    return false;
  }

  return true;
}

void StreamExecutor::DeallocateStream(Stream* stream) {
  dnn::DnnSupport* dnn;
  {
    absl::MutexLock lock(&mu_);
    dnn = dnn_.get();
  }
  if (dnn) {
    dnn->NotifyStreamDestroyed(stream);
  }
  implementation_->DeallocateStream(stream);
  CHECK_GE(live_stream_count_.fetch_sub(1), 0)
      << "live stream count should not dip below zero";
}

bool StreamExecutor::CreateStreamDependency(Stream* dependent, Stream* other) {
  return implementation_->CreateStreamDependency(dependent, other);
}

std::unique_ptr<DeviceDescription> StreamExecutor::CreateDeviceDescription()
    const {
  return implementation_->CreateDeviceDescription().value();
}

bool StreamExecutor::DeviceMemoryUsage(int64_t* free, int64_t* total) const {
  return implementation_->DeviceMemoryUsage(free, total);
}

void StreamExecutor::EnqueueOnBackgroundThread(std::function<void()> task) {
  background_threads_->Schedule(std::move(task));
}

void StreamExecutor::RegisterTraceListener(TraceListener* listener) {
  {
    absl::MutexLock lock(&mu_);
    if (listeners_.find(listener) != listeners_.end()) {
      LOG(INFO) << "Attempt to register already-registered listener, "
                << listener;
    } else {
      listeners_.insert(listener);
    }
  }

  implementation_->RegisterTraceListener(listener);
}

bool StreamExecutor::UnregisterTraceListener(TraceListener* listener) {
  {
    absl::MutexLock lock(&mu_);
    if (listeners_.find(listener) == listeners_.end()) {
      LOG(INFO) << "Attempt to unregister unknown listener, " << listener;
      return false;
    }
    listeners_.erase(listener);
  }

  implementation_->UnregisterTraceListener(listener);
  return true;
}

std::optional<AllocatorStats> StreamExecutor::GetAllocatorStats() {
  return implementation_->GetAllocatorStats();
}

bool StreamExecutor::ClearAllocatorStats() {
  return implementation_->ClearAllocatorStats();
}

Stream* StreamExecutor::FindAllocatedStream(void* gpu_stream) {
  return implementation_->FindAllocatedStream(gpu_stream);
}

template <typename TraceCallT, typename... ArgsT>
void StreamExecutor::SubmitTrace(TraceCallT trace_call, ArgsT&&... args) {
  if (tracing_enabled_) {
    {
      // instance tracers held in a block to limit the lock lifetime.
      absl::ReaderMutexLock lock(&mu_);
      for (TraceListener* listener : listeners_) {
        (listener->*trace_call)(std::forward<ArgsT>(args)...);
      }
    }
  }
}

internal::StreamExecutorInterface* StreamExecutor::implementation() {
  return implementation_->GetUnderlyingExecutor();
}

StreamExecutorMemoryAllocator::StreamExecutorMemoryAllocator(
    StreamExecutor* executor)
    : DeviceMemoryAllocator(executor->platform()) {
  stream_executors_ = {executor};
}

StreamExecutorMemoryAllocator::StreamExecutorMemoryAllocator(
    const Platform* platform,
    absl::Span<StreamExecutor* const> stream_executors)
    : DeviceMemoryAllocator(platform),
      stream_executors_(stream_executors.begin(), stream_executors.end()) {}

tsl::StatusOr<OwningDeviceMemory> StreamExecutorMemoryAllocator::Allocate(
    int device_ordinal, uint64_t size, bool retry_on_failure,
    int64_t memory_space) {
  TF_ASSIGN_OR_RETURN(StreamExecutor * executor,
                      GetStreamExecutor(device_ordinal));
  DeviceMemoryBase result =
      executor->AllocateArray<uint8_t>(size, memory_space);
  if (size > 0 && result == nullptr) {
    return tsl::errors::ResourceExhausted(absl::StrFormat(
        "Failed to allocate request for %s (%uB) on device ordinal %d",
        tsl::strings::HumanReadableNumBytes(size), size, device_ordinal));
  }
  VLOG(3) << absl::StreamFormat("Allocated %s (%uB) on device ordinal %d: %p",
                                tsl::strings::HumanReadableNumBytes(size), size,
                                device_ordinal, result.opaque());
  return OwningDeviceMemory(result, device_ordinal, this);
}

tsl::Status StreamExecutorMemoryAllocator::Deallocate(int device_ordinal,
                                                      DeviceMemoryBase mem) {
  if (!mem.is_null()) {
    TF_ASSIGN_OR_RETURN(StreamExecutor * executor,
                        GetStreamExecutor(device_ordinal));
    VLOG(3) << absl::StreamFormat("Freeing %p on device ordinal %d",
                                  mem.opaque(), device_ordinal);
    executor->Deallocate(&mem);
  }
  return ::tsl::OkStatus();
}

tsl::StatusOr<StreamExecutor*> StreamExecutorMemoryAllocator::GetStreamExecutor(
    int device_ordinal) const {
  if (device_ordinal < 0) {
    return tsl::errors::InvalidArgument(absl::StrFormat(
        "device ordinal value (%d) must be non-negative", device_ordinal));
  }
  for (StreamExecutor* se : stream_executors_) {
    if (se->device_ordinal() == device_ordinal) {
      return se;
    }
  }
  return tsl::errors::NotFound(
      absl::StrFormat("Device %s:%d present but not supported",
                      platform()->Name(), device_ordinal));
}

bool StreamExecutorMemoryAllocator::AllowsAsynchronousDeallocation() const {
  return false;
}

tsl::StatusOr<Stream*> StreamExecutorMemoryAllocator::GetStream(
    int device_ordinal) {
  CHECK(!AllowsAsynchronousDeallocation())
      << "The logic below only works for synchronous allocators";
  TF_ASSIGN_OR_RETURN(StreamExecutor * executor,
                      GetStreamExecutor(device_ordinal));
  Stream* out = [&] {
    absl::MutexLock lock(&mutex_);
    if (!streams_.count(device_ordinal)) {
      auto p = streams_.emplace(std::piecewise_construct,
                                std::forward_as_tuple(device_ordinal),
                                std::forward_as_tuple(executor));
      p.first->second.Init();
      return &p.first->second;
    }
    return &streams_.at(device_ordinal);
  }();
  return out;
}

}  // namespace stream_executor
