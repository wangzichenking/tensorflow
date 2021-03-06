/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/python/local_client.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "include/pybind11/pybind11.h"
#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/executable_run_options.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/python/types.h"
#include "tensorflow/compiler/xla/service/cpu/custom_call_target_registry.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/lib/traceme.h"

namespace xla {

namespace py = pybind11;

// Registers a 'fn_capsule' as a CPU custom call target.
// 'fn_capsule' is a void* pointer encapsulated in a PyCapsule object, with name
// "xla._CPU_CUSTOM_CALL_TARGET".
Status RegisterCpuCustomCallTarget(const std::string& fn_name,
                                   py::capsule capsule) {
  static const char* const kName = "xla._CPU_CUSTOM_CALL_TARGET";
  if (absl::string_view(capsule.name()) != kName) {
    return InvalidArgument(
        "Argument to RegisterCpuCustomCallTargetRegistry was not a "
        "xla._CPU_CUSTOM_CALL_TARGET capsule.");
  }
  cpu::CustomCallTargetRegistry::Global()->Register(
      std::string(fn_name.begin(), fn_name.end()), static_cast<void*>(capsule));
  return Status::OK();
}

StatusOr<std::unique_ptr<PyLocalClient>> PyLocalClient::Get(
    const std::string& platform_name) {
  TF_ASSIGN_OR_RETURN(se::Platform * platform,
                      PlatformUtil::GetPlatform(platform_name));
  if (platform->VisibleDeviceCount() <= 0) {
    return InvalidArgument("Platform %s has no visible devices.",
                           platform_name);
  }
  LocalClientOptions options;
  options.set_platform(platform);
  TF_ASSIGN_OR_RETURN(LocalClient * client,
                      ClientLibrary::GetOrCreateLocalClient(options));
  return absl::make_unique<PyLocalClient>(client);
}

PyLocalClient::PyLocalClient(LocalClient* client)
    : client_(client),
      h2d_transfer_pool_(tensorflow::Env::Default(), "py_xla_h2d_transfer",
                         client->device_count()) {
  execute_threads_.reserve(client->device_count());
  for (int i = 0; i < client->device_count(); ++i) {
    execute_threads_.push_back(absl::make_unique<WorkerThread>(
        tensorflow::Env::Default(), "py_xla_execute"));
  }
}

Status PyLocalClient::TransferToInfeed(const LiteralSlice& literal,
                                       int device_ordinal) {
  py::gil_scoped_release gil_release;
  return client_->TransferToInfeedLocal(literal, device_ordinal);
}

StatusOr<pybind11::object> PyLocalClient::TransferFromOutfeed(
    const Shape& shape, int device_ordinal) {
  Literal literal;
  {
    py::gil_scoped_release gil_release;
    TF_ASSIGN_OR_RETURN(
        literal, client_->TransferFromOutfeedLocal(shape, device_ordinal));
  }
  return LiteralToPython(absl::make_unique<Literal>(std::move(literal)));
}

static StatusOr<PyLocalBuffer> TransferHostToDeviceAsync(
    const PythonBufferTree& tree, int device_ordinal, PyLocalClient* client,
    se::Stream* stream) {
  DeviceMemoryAllocator* allocator =
      client->client()->backend().memory_allocator();
  TransferManager* transfer_manager =
      client->client()->backend().transfer_manager();
  TF_ASSIGN_OR_RETURN(
      Shape shape, transfer_manager->ChooseCompactLayoutForShape(tree.shape));
  TF_ASSIGN_OR_RETURN(ScopedShapedBuffer buffer,
                      transfer_manager->AllocateScopedShapedBuffer(
                          shape, allocator, device_ordinal));
  TF_RETURN_IF_ERROR(
      transfer_manager->WriteTupleIndexTablesAsync(stream, buffer));

  auto it = tree.leaves.begin();
  for (const ShapeUtil::IndexedShape& indexed_shape :
       ShapeUtil::GetLeafShapes(shape)) {
    TF_RET_CHECK(it != tree.leaves.end());
    ShapedBuffer leaf(
        indexed_shape.shape,
        transfer_manager->HostShapeToDeviceShape(indexed_shape.shape),
        client->client()->platform(), device_ordinal);
    leaf.buffers().CopySubtreeFrom(buffer.buffers(), indexed_shape.index, {});
    TF_RETURN_IF_ERROR(
        transfer_manager->TransferLiteralToDeviceAsync(stream, *it, leaf));
    ++it;
  }
  return PyLocalBuffer(std::move(buffer), client);
}

/* static */
StatusOr<PyLocalBuffer> PyLocalBuffer::FromPython(const py::object& argument,
                                                  PyLocalClient* client,
                                                  int device_ordinal) {
  tensorflow::profiler::TraceMe traceme("PyLocalBuffer::FromPython");
  TF_ASSIGN_OR_RETURN(PythonBufferTree tree, GetPythonBufferTree(argument));

  // We are done manipulating Python objects; release the GIL.
  py::gil_scoped_release gil_release;
  VLOG(1) << "PyLocalBuffer::FromPython: shape: " << tree.shape.ToString()
          << " device ordinal: " << device_ordinal;

  TF_ASSIGN_OR_RETURN(
      StreamPool::Ptr stream,
      client->client()->mutable_backend()->BorrowStream(device_ordinal));
  TF_ASSIGN_OR_RETURN(
      PyLocalBuffer buffer,
      TransferHostToDeviceAsync(tree, device_ordinal, client, stream.get()));
  stream->BlockHostUntilDone();
  return buffer;
}

/*static */ StatusOr<std::vector<PyLocalBuffer>>
PyLocalBuffer::FromPythonValues(
    const std::vector<std::pair<py::object, int>>& arguments,
    PyLocalClient* client) {
  tensorflow::profiler::TraceMe traceme("PyLocalBuffer::FromPythonValues");
  int num_arguments = static_cast<int>(arguments.size());
  std::vector<PyLocalBuffer> outputs(num_arguments);
  if (num_arguments == 0) {
    return outputs;
  }

  struct H2DTransfer {
    PythonBufferTree tree;
    StreamPool::Ptr stream;
    StatusOr<PyLocalBuffer> buffer;
  };

  std::vector<H2DTransfer> transfers(num_arguments);
  for (int i = 0; i < num_arguments; ++i) {
    TF_ASSIGN_OR_RETURN(transfers[i].tree,
                        GetPythonBufferTree(arguments[i].first));
  }
  // We are done manipulating Python objects; release the GIL.
  py::gil_scoped_release gil_release;

  for (int i = 0; i < num_arguments; ++i) {
    int device_ordinal = arguments[i].second;
    TF_ASSIGN_OR_RETURN(
        transfers[i].stream,
        client->client()->mutable_backend()->BorrowStream(device_ordinal));
  }

  auto transfer_h2d = [&](int i) -> StatusOr<PyLocalBuffer> {
    int device_ordinal = arguments[i].second;
    return TransferHostToDeviceAsync(transfers[i].tree, device_ordinal, client,
                                     transfers[i].stream.get());
  };

  // We perform the transfers on a thread pool in case XLA needs to do any
  // host-side preprocessing of the input data.
  if (num_arguments == 1) {
    transfers[0].buffer = transfer_h2d(0);
  } else {
    absl::BlockingCounter counter(num_arguments - 1);
    for (int i = 1; i < num_arguments; ++i) {
      client->h2d_transfer_pool()->Schedule([&, i]() {
        transfers[i].buffer = transfer_h2d(i);
        counter.DecrementCount();
      });
    }
    // Perform the first transfer on the main thread.
    transfers[0].buffer = transfer_h2d(0);
    counter.Wait();
  }

  // First, wait for all transfers to complete. We wait for all to complete
  // since currently we maintain the invariant that the device's view of the
  // state matches the host's view of the state. Returning early would mean that
  // we might deallocate device-side memory before a transfer completes, which
  // violates that invariant.
  for (int i = 0; i < num_arguments; ++i) {
    transfers[i].stream->BlockHostUntilDone();
  }
  for (int i = 0; i < num_arguments; ++i) {
    TF_ASSIGN_OR_RETURN(outputs[i], std::move(transfers[i].buffer));
  }
  return outputs;
}

PyLocalBuffer::PyLocalBuffer(ScopedShapedBuffer shaped_buffer,
                             PyLocalClient* client)
    : shaped_buffer_(std::move(shaped_buffer)), client_(client) {}

const ScopedShapedBuffer* PyLocalBuffer::shaped_buffer() const {
  return &shaped_buffer_.value();
}

ScopedShapedBuffer PyLocalBuffer::Release() {
  ScopedShapedBuffer result = std::move(*shaped_buffer_);
  shaped_buffer_ = absl::nullopt;
  return result;
}

const Shape& PyLocalBuffer::shape() const {
  return shaped_buffer()->on_device_shape();
}

StatusOr<py::object> PyLocalBuffer::ToPython() const {
  tensorflow::profiler::TraceMe traceme("PyLocalBuffer::ToPython");
  auto literal = absl::make_unique<Literal>();
  {
    py::gil_scoped_release gil_release;
    TF_ASSIGN_OR_RETURN(
        *literal, client_->client()->ShapedBufferToLiteral(*shaped_buffer()));
  }
  return LiteralToPython(std::move(literal));
}

StatusOr<std::vector<PyLocalBuffer>> PyLocalBuffer::DestructureTuple() {
  tensorflow::profiler::TraceMe traceme("PyLocalBuffer::DestructureTuple");
  const Shape tuple_shape = shape();

  if (!tuple_shape.IsTuple()) {
    return InvalidArgument(
        "Attemped to destructure a PyLocalBuffer that did not have a tuple "
        "shape; shape: %s",
        ShapeUtil::HumanString(tuple_shape));
  }

  DeviceMemoryAllocator* allocator = shaped_buffer()->memory_allocator();
  ScopedShapedBuffer tuple_buffer = Release();

  // Extract some metadata we use to construct scoped buffers.
  const se::Platform* platform = tuple_buffer.platform();
  int device_ordinal = tuple_buffer.device_ordinal();

  ShapeTree<se::DeviceMemoryBase>& shape_tree = tuple_buffer.buffers();
  std::vector<PyLocalBuffer> results;
  for (int64 i = 0; i < ShapeUtil::TupleElementCount(tuple_shape); ++i) {
    // Create a shaped buffer for this destructured tuple element.
    const Shape& subshape = ShapeUtil::GetSubshape(tuple_shape, {i});
    VLOG(3) << "Starting tuple element " << i << " subshape: " << subshape;
    ShapedBuffer shaped_buffer(subshape, subshape, platform, device_ordinal);

    ShapeUtil::ForEachSubshape(
        subshape, [&](const Shape& s, const ShapeIndex& index) {
          ShapeIndex original(index);
          original.push_front(i);
          se::DeviceMemoryBase* device_memory =
              shape_tree.mutable_element(original);
          shaped_buffer.set_buffer(*device_memory, index);
          *device_memory = se::DeviceMemoryBase();
        });

    VLOG(3) << "Completed tuple element: " << i;
    results.push_back(PyLocalBuffer(
        ScopedShapedBuffer(std::move(shaped_buffer), allocator), client_));
  }
  return results;
}

PyLocalExecutable::PyLocalExecutable(
    std::unique_ptr<LocalExecutable> executable,
    DeviceAssignment device_assignment, PyLocalClient* client)
    : executable_(std::move(executable)),
      device_assignment_(std::move(device_assignment)),
      client_(client) {}

std::vector<int> PyLocalExecutable::DeviceOrdinals() const {
  int num_replicas = device_assignment_.replica_count();
  std::vector<int> device_ordinals;
  device_ordinals.reserve(num_replicas);
  for (int i = 0; i < num_replicas; ++i) {
    device_ordinals.push_back(device_assignment_(i, 0));
  }
  return device_ordinals;
}

StatusOr<PyLocalBuffer> PyLocalExecutable::Execute(
    absl::Span<PyLocalBuffer* const> argument_handles) {
  tensorflow::profiler::TraceMe traceme("LocalExecutable::Execute");
  if (num_replicas() != 1) {
    return InvalidArgument(
        "Attempted to execute computation with %d replicas using Execute()",
        num_replicas());
  }
  StatusOr<ScopedShapedBuffer> result_buffer_status;
  const int device_ordinal = device_assignment_(0, 0);
  VLOG(3) << "Replica 0 mapped to device ordinal for execution: "
          << device_ordinal;

  std::vector<const ShapedBuffer*> argument_buffers;
  argument_buffers.reserve(argument_handles.size());
  for (auto& handle : argument_handles) {
    argument_buffers.push_back(handle->shaped_buffer());
  }

  ExecutableRunOptions options;
  options.set_device_ordinal(device_ordinal);
  options.set_allocator(client_->client()->backend().memory_allocator());
  options.set_intra_op_thread_pool(
      client_->client()->backend().eigen_intra_op_thread_pool_device());
  options.set_device_assignment(&device_assignment_);

  result_buffer_status = executable_->Run(argument_buffers, options);

  if (!result_buffer_status.ok()) {
    return result_buffer_status.status();
  }
  return PyLocalBuffer(std::move(result_buffer_status).ValueOrDie(), client_);
}

StatusOr<std::vector<PyLocalBuffer>> PyLocalExecutable::ExecutePerReplica(
    absl::Span<const std::vector<PyLocalBuffer*>> argument_handles) {
  tensorflow::profiler::TraceMe traceme("LocalExecutable::ExecutePerReplica");
  const int num_devices = client_->device_count();

  if (argument_handles.size() != num_replicas()) {
    return InvalidArgument(
        "Attempted to execute with %d replicas when replica count is %d",
        argument_handles.size(), num_devices);
  }
  if (argument_handles.size() > num_devices) {
    return InvalidArgument(
        "Attempted to execute with %d replicas when device count is %d",
        argument_handles.size(), num_devices);
  }

  VLOG(1) << "Executing with " << num_replicas() << " replicas.";

  auto execute =
      [this, &argument_handles](int replica) -> StatusOr<ScopedShapedBuffer> {
    const int device_ordinal = device_assignment_(replica, 0);
    VLOG(3) << "Replica " << replica
            << " mapped to device ordinal for execution: " << device_ordinal;

    std::vector<const ShapedBuffer*> argument_buffers;
    argument_buffers.reserve(argument_handles[replica].size());
    for (auto& handle : argument_handles[replica]) {
      argument_buffers.push_back(handle->shaped_buffer());
    }

    ExecutableRunOptions options;
    options.set_device_ordinal(device_ordinal);
    options.set_allocator(client_->client()->backend().memory_allocator());
    options.set_intra_op_thread_pool(
        client_->client()->backend().eigen_intra_op_thread_pool_device());
    options.set_device_assignment(&device_assignment_);
    StatusOr<ScopedShapedBuffer> result_buffer_status =
        executable_->Run(argument_buffers, options);

    VLOG(1) << "Replica " << replica
            << " completed; ok=" << result_buffer_status.ok();
    if (!result_buffer_status.ok()) {
      LOG(ERROR) << "Execution of replica " << replica
                 << " failed: " << result_buffer_status.status();
    }
    return result_buffer_status;
  };

  VLOG(1) << "Executing replicated computation; num_replicas="
          << num_replicas();
  std::vector<StatusOr<ScopedShapedBuffer>> results(num_replicas());
  if (num_replicas() == 1) {
    // Fast-path if there is only one replica — run the computation on the
    // current thread.
    results[0] = execute(0);
  } else {
    absl::Mutex mu;
    int running GUARDED_BY(mu) = num_replicas();
    int failed GUARDED_BY(mu) = 0;
    Status first_failure_status GUARDED_BY(mu);

    for (int replica = 0; replica < num_replicas(); ++replica) {
      client_->execute_threads().at(replica)->Schedule([&, replica] {
        results[replica] = execute(replica);

        absl::MutexLock lock(&mu);
        --running;
        if (!results[replica].ok()) {
          if (failed == 0) {
            first_failure_status = results[replica].status();
          }
          ++failed;
        }
      });
    }

    auto done_running_or_failed = [&]() {
      mu.AssertHeld();
      return running == 0 || failed > 0;
    };
    absl::MutexLock lock(&mu);
    mu.Await(absl::Condition(&done_running_or_failed));
    if (failed > 0) {
      auto done_running = [&]() {
        mu.AssertHeld();
        return running == 0;
      };
      // If execution does not terminate within a reasonable amount of time, we
      // may be stuck at a cross-replica barrier on-device. Terminate the
      // process since that's the only way we can escape this situation at the
      // moment (b/130629719).
      if (!mu.AwaitWithTimeout(absl::Condition(&done_running),
                               absl::Seconds(10))) {
        LOG(FATAL)
            << "Replicated computation launch failed, but not all replicas "
               "terminated. Aborting process to work around deadlock. Failure "
               "message (there may have been multiple failures, see the "
               "error log for all failures): \n\n"
            << first_failure_status.error_message();
      }
    }
  }
  VLOG(1) << "Replicated execution complete.";

  std::vector<PyLocalBuffer> wrapped_results(num_replicas());
  for (int replica = 0; replica < num_replicas(); ++replica) {
    auto& statusor = results[replica];
    if (!statusor.ok()) {
      return AppendStatus(
          statusor.status(),
          absl::StrFormat(
              "while running replica %d of a replicated computation (other "
              "replicas may have failed as well).",
              replica));
    }
    wrapped_results[replica] =
        PyLocalBuffer(std::move(statusor).ValueOrDie(), client_);
  }
  return wrapped_results;
}

/*static*/ StatusOr<std::unique_ptr<PyLocalExecutable>>
PyLocalExecutable::Compile(const XlaComputation& computation,
                           std::vector<Shape> argument_layouts,
                           const ExecutableBuildOptions* build_options,
                           PyLocalClient* client) {
  tensorflow::profiler::TraceMe traceme("LocalExecutable::Compile");
  std::vector<const Shape*> argument_layout_pointers;
  argument_layout_pointers.reserve(argument_layouts.size());

  // Assign a default layout to any array subshapes that are missing layouts.
  auto assign_layouts = [client](Shape* shape) {
    return ShapeUtil::ForEachMutableSubshapeWithStatus(
        shape, [&](Shape* subshape, const ShapeIndex&) {
          if (subshape->IsArray() && !subshape->has_layout()) {
            LayoutUtil::SetToDefaultLayout(subshape);
            TF_ASSIGN_OR_RETURN(*subshape,
                                client->client()
                                    ->backend()
                                    .transfer_manager()
                                    ->ChooseCompactLayoutForShape(*subshape));
          }
          return Status::OK();
        });
  };

  for (Shape& layout : argument_layouts) {
    argument_layout_pointers.push_back(&layout);
    assign_layouts(&layout);
  }

  ExecutableBuildOptions options;
  if (build_options != nullptr) {
    options = *build_options;
  }

  Shape result_layout;
  if (options.result_layout()) {
    result_layout = *options.result_layout();
  } else {
    TF_ASSIGN_OR_RETURN(ProgramShape program_shape,
                        computation.GetProgramShape());
    result_layout = program_shape.result();
    LayoutUtil::ClearLayout(&result_layout);
  }
  assign_layouts(&result_layout);
  options.set_result_layout(result_layout);

  TF_ASSIGN_OR_RETURN(std::unique_ptr<LocalExecutable> local_executable,
                      client->client()->Compile(
                          computation, argument_layout_pointers, options));
  TF_ASSIGN_OR_RETURN(
      DeviceAssignment device_assignment,
      client->client()->backend().computation_placer()->AssignDevices(
          options.num_replicas(), /*computation_count=*/1));

  return absl::make_unique<PyLocalExecutable>(
      std::move(local_executable), std::move(device_assignment), client);
}

}  // namespace xla
