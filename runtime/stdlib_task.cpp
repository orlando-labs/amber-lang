#include "runtime/concurrency.h"
#include "runtime/stdlib_registry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace amber::runtime {

namespace {

bool require_exact_args_no_keywords_no_block(NativeStdlibCall &call,
                                             std::size_t expected,
                                             const std::string &message) {
  const bool arity_ok = call.require_arity(expected);
  if (!arity_ok || !call.kw_args.empty()) {
    if (!call.kw_args.empty()) {
      call.fault("TypeError", message);
    }
    return false;
  }
  return call.require_no_block();
}

bool capacity_from_args(NativeStdlibCall &call, std::size_t *out) {
  *out = 0;
  if (!call.args.empty()) {
    if (call.args.size() != 1 || !call.args[0].is_integer() ||
        call.args[0].as_integer() < 0) {
      call.fault("TypeError", "capacity must be a non-negative Integer");
      return false;
    }
    *out = static_cast<std::size_t>(call.args[0].as_integer());
  }
  const std::optional<Value> capacity = call.keyword("capacity");
  if (capacity.has_value()) {
    if (!capacity->is_integer() || capacity->as_integer() < 0) {
      call.fault("TypeError", "capacity must be a non-negative Integer");
      return false;
    }
    *out = static_cast<std::size_t>(capacity->as_integer());
  }
  return true;
}

bool duration_from_value(NativeStdlibCall &call, const Value &value,
                         std::chrono::milliseconds *out) {
  if (value.is_integer()) {
    *out = std::chrono::milliseconds(value.as_integer());
    return true;
  }
  if (value.is_float()) {
    *out = std::chrono::milliseconds(
        static_cast<std::int64_t>(value.as_float() * 1000.0));
    return true;
  }
  call.fault("TypeError",
             "duration must be Integer milliseconds or Float seconds");
  return false;
}

std::optional<std::chrono::milliseconds>
timeout_from_keywords(NativeStdlibCall &call) {
  const std::optional<Value> timeout = call.keyword("timeout");
  if (!timeout.has_value()) {
    return std::chrono::milliseconds::max();
  }
  std::chrono::milliseconds value;
  if (!duration_from_value(call, *timeout, &value)) {
    return std::nullopt;
  }
  return value;
}

bool set_fault_from_channel_result(NativeStdlibCall &call,
                                   const RuntimeChannelResult &result) {
  call.fault(result.error_name.empty() ? "ChannelError" : result.error_name,
             result.message.empty() ? "channel operation failed"
                                    : result.message);
  return false;
}

std::optional<RuntimeFlowPartitionPolicy>
threaded_scatter_policy_from_value(NativeStdlibCall &call, const Value &value) {
  const std::optional<std::string> text = call.text_of(value);
  if (!text.has_value()) {
    call.fault("TypeError",
               "threaded scatter must be :atomic, :dynamic, :chunks, "
               ":fixed, :stride, or :items");
    return std::nullopt;
  }
  if (*text == "atomic" || *text == "shared" || *text == "dynamic") {
    return RuntimeFlowPartitionPolicy::Atomic;
  }
  if (*text == "chunks" || *text == "chunk" || *text == "fixed" ||
      *text == "fixed_chunks") {
    return RuntimeFlowPartitionPolicy::Chunks;
  }
  if (*text == "stride" || *text == "strided") {
    return RuntimeFlowPartitionPolicy::Stride;
  }
  if (*text == "items" || *text == "item" || *text == "per_item") {
    return RuntimeFlowPartitionPolicy::Items;
  }
  call.fault("TypeError",
             "threaded scatter must be :atomic, :dynamic, :chunks, "
             ":fixed, :stride, or :items");
  return std::nullopt;
}

bool threaded_scatter_policy_from_keywords(NativeStdlibCall &call,
                                           RuntimeFlowPartitionPolicy *out) {
  *out = RuntimeFlowPartitionPolicy::Atomic;
  if (!call.reject_unknown_keywords({"scatter"})) {
    return false;
  }
  const std::optional<Value> scatter = call.keyword("scatter");
  if (!scatter.has_value()) {
    return true;
  }
  const std::optional<RuntimeFlowPartitionPolicy> policy =
      threaded_scatter_policy_from_value(call, *scatter);
  if (!policy.has_value()) {
    return false;
  }
  *out = *policy;
  return true;
}

SendStatus construct_channel(NativeStdlibCall &call) {
  if (!call.reject_unknown_keywords({"capacity"}) || !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  std::size_t capacity = 0;
  if (!capacity_from_args(call, &capacity)) {
    return SendStatus::Faulted;
  }
  *call.out = Value::channel(std::make_shared<RuntimeChannel>(capacity));
  return SendStatus::Matched;
}

SendStatus construct_mutex(NativeStdlibCall &call) {
  if (!require_exact_args_no_keywords_no_block(
          call, 0, "Mutex.new does not accept keywords")) {
    return SendStatus::Faulted;
  }
  *call.out = Value::mutex(std::make_shared<RuntimeMutex>());
  return SendStatus::Matched;
}

SendStatus construct_atomic(NativeStdlibCall &call) {
  if (!require_exact_args_no_keywords_no_block(
          call, 1, "Atomic.new does not accept keywords")) {
    return SendStatus::Faulted;
  }
  try {
    *call.out = Value::atomic(std::make_shared<RuntimeAtomic>(call.args[0]));
  } catch (const RuntimeTaskFailure &failure) {
    return call.fault(failure.error_name(), failure.message());
  }
  return SendStatus::Matched;
}

SendStatus construct_barrier(NativeStdlibCall &call) {
  if (!require_exact_args_no_keywords_no_block(
          call, 1, "Barrier.new does not accept keywords")) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_integer() || call.args[0].as_integer() <= 0) {
    return call.fault("TypeError", "barrier parties must be positive");
  }
  try {
    *call.out = Value::barrier(std::make_shared<RuntimeBarrier>(
        static_cast<std::size_t>(call.args[0].as_integer())));
  } catch (const RuntimeTaskFailure &failure) {
    return call.fault(failure.error_name(), failure.message());
  }
  return SendStatus::Matched;
}

SendStatus construct_flow(NativeStdlibCall &call) {
  if (!require_exact_args_no_keywords_no_block(
          call, 0, "Flow.new does not accept keywords")) {
    return SendStatus::Faulted;
  }
  *call.out = Value::flow_module(std::make_shared<RuntimeFlowModule>());
  return SendStatus::Matched;
}

SendStatus construct_threaded_collection(NativeStdlibCall &call) {
  if ((call.args.empty() || call.args.size() > 2) || !call.require_no_block()) {
    return call.fault("TypeError",
                      "ThreadedCollection.new expects items and workers");
  }
  RuntimeFlowPartitionPolicy scatter_policy =
      RuntimeFlowPartitionPolicy::Atomic;
  if (!threaded_scatter_policy_from_keywords(call, &scatter_policy)) {
    return SendStatus::Faulted;
  }
  std::vector<Value> items;
  if (!call.sequence_items(call.args[0], &items)) {
    return call.fault("TypeError",
                      "ThreadedCollection.new expects sequence items");
  }
  std::size_t workers = 0;
  if (call.args.size() == 2) {
    if (!call.args[1].is_integer() || call.args[1].as_integer() < 0) {
      return call.fault("TypeError",
                        "ThreadedCollection workers must be non-negative");
    }
    workers = static_cast<std::size_t>(call.args[1].as_integer());
  }
  *call.out =
      Value::threaded_collection(std::make_shared<RuntimeThreadedCollection>(
          std::move(items), workers, RuntimeFlowOptions{}, scatter_policy));
  return SendStatus::Matched;
}

SendStatus channel_instance_send(NativeStdlibCall &call) {
  if (!call.receiver.is_channel()) {
    return SendStatus::NotHandled;
  }
  const std::shared_ptr<RuntimeChannel> channel = call.receiver.as_channel();
  if (channel == nullptr) {
    return call.fault("TypeError", "channel is null");
  }
  if (call.selector == "close") {
    if (!call.require_arity(0) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      if (!call.kw_args.empty()) {
        call.fault("TypeError", "Channel.close does not accept keywords");
      }
      return SendStatus::Faulted;
    }
    *call.out = Value::boolean(channel->close());
    return SendStatus::Matched;
  }
  if (call.selector == "closed?") {
    if (!call.require_arity(0) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      if (!call.kw_args.empty()) {
        call.fault("TypeError", "Channel.closed? does not accept keywords");
      }
      return SendStatus::Faulted;
    }
    *call.out = Value::boolean(channel->closed());
    return SendStatus::Matched;
  }
  if (call.selector == "send") {
    if (!call.require_arity(1) ||
        !call.reject_unknown_keywords({"timeout"}) ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    const std::optional<std::chrono::milliseconds> timeout =
        timeout_from_keywords(call);
    if (!timeout.has_value()) {
      return SendStatus::Faulted;
    }
    if (call.block_suspension_in_property_arm("Channel.send")) {
      return SendStatus::Faulted;
    }
    const RuntimeChannelResult sent = channel->send(call.args[0], *timeout);
    if (!sent.ok) {
      set_fault_from_channel_result(call, sent);
      return SendStatus::Faulted;
    }
    *call.out = Value::boolean(sent.sent);
    return SendStatus::Matched;
  }
  if (call.selector == "recv") {
    if (!call.require_arity(0) ||
        !call.reject_unknown_keywords({"timeout"}) ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    const std::optional<std::chrono::milliseconds> timeout =
        timeout_from_keywords(call);
    if (!timeout.has_value()) {
      return SendStatus::Faulted;
    }
    if (call.block_suspension_in_property_arm("Channel.recv")) {
      return SendStatus::Faulted;
    }
    const RuntimeChannelResult received = channel->recv(*timeout);
    if (!received.ok) {
      set_fault_from_channel_result(call, received);
      return SendStatus::Faulted;
    }
    *call.out = received.value;
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

SendStatus task_type_send(NativeStdlibCall &call) {
  if (call.receiver.is_channel()) {
    return channel_instance_send(call);
  }
  if (!call.receiver.is_native_type()) {
    return call.task_runtime_send();
  }
  if (call.selector != "new") {
    return SendStatus::NotHandled;
  }
  switch (call.kind) {
  case RuntimeNativeTypeKind::Channel:
    return construct_channel(call);
  case RuntimeNativeTypeKind::Mutex:
    return construct_mutex(call);
  case RuntimeNativeTypeKind::Atomic:
    return construct_atomic(call);
  case RuntimeNativeTypeKind::Barrier:
    return construct_barrier(call);
  case RuntimeNativeTypeKind::Flow:
    return construct_flow(call);
  case RuntimeNativeTypeKind::ThreadedCollection:
    return construct_threaded_collection(call);
  default:
    return SendStatus::NotHandled;
  }
}

RuntimeNativeModuleDescriptor task_module_descriptor() {
  return {{{"Flow", RuntimeNativeTypeKind::Flow},
           {"task.flow.Flow", RuntimeNativeTypeKind::Flow},
           {"Channel", RuntimeNativeTypeKind::Channel},
           {"sync.Channel", RuntimeNativeTypeKind::Channel},
           {"Mutex", RuntimeNativeTypeKind::Mutex},
           {"sync.Mutex", RuntimeNativeTypeKind::Mutex},
           {"Atomic", RuntimeNativeTypeKind::Atomic},
           {"sync.Atomic", RuntimeNativeTypeKind::Atomic},
           {"Barrier", RuntimeNativeTypeKind::Barrier},
           {"sync.Barrier", RuntimeNativeTypeKind::Barrier},
           {"ThreadedCollection", RuntimeNativeTypeKind::ThreadedCollection},
           {"task.flow.ThreadedCollection",
            RuntimeNativeTypeKind::ThreadedCollection}},
          {{RuntimeNativeTypeKind::Channel, task_type_send},
           {RuntimeNativeTypeKind::TaskModule, task_type_send},
           {RuntimeNativeTypeKind::Mutex, task_type_send},
           {RuntimeNativeTypeKind::Atomic, task_type_send},
           {RuntimeNativeTypeKind::Barrier, task_type_send},
           {RuntimeNativeTypeKind::Flow, task_type_send},
           {RuntimeNativeTypeKind::ThreadedCollection, task_type_send}},
          {},
          {},
          {{"TaskError", "Exception"},
           {"TaskNotDoneError", "TaskError"},
           {"TaskFailedError", "TaskError"},
           {"TimeoutError", "Exception"},
           {"CancelledError", "Exception"},
           {"ChannelClosedError", "Exception"},
           {"DeadlockError", "Exception"},
           {"OwnershipError", "Exception"},
           {"AtomicError", "Exception"},
           {"AtomicCompatibilityError", "AtomicError"},
           {"FlowError", "Exception"},
           {"FlowCancelledError", "FlowError"},
           {"FlowPartitionError", "FlowError"},
           {"FlowGatherError", "FlowError"},
           {"MoveError", "Exception"},
           {"MovedValueError", "MoveError"}}};
}

} // namespace

void register_task_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types,
                                  RuntimeErrorRegistry *errors) {
  const RuntimeNativeModuleDescriptor descriptor = task_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr) {
    register_runtime_error_descriptor(*errors, descriptor);
  }
}

} // namespace amber::runtime
