#pragma once

#include "runtime/value.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace amber::runtime {

class RuntimeHeap;
struct RuntimePinToken;

enum class RuntimeIoWaitInterest {
  Other,
  Read,
  Write,
  Accept,
  Connect,
  Flush,
  Close,
  Metadata,
  Open
};

struct RuntimeIoWaitRecord {
  std::uint64_t wait_id = 0;
  std::uint64_t task_id = 0;
  std::uint64_t strand_id = 0;
  std::uint64_t worker_id = 0;
  std::uint64_t resource_id = 0;
  RuntimeIoWaitInterest interest = RuntimeIoWaitInterest::Other;
  std::string operation;
  std::string resource;
  bool has_timeout = false;
  std::int64_t timeout_millis = 0;
};

using RuntimeIoWaitObserver =
    std::function<void(const RuntimeIoWaitRecord &, bool entering)>;

std::uint64_t current_runtime_worker_id();
std::uint64_t current_runtime_strand_id();
std::uint64_t current_runtime_task_id();
bool current_runtime_task_cancel_requested();
bool current_runtime_task_sync_active();

class RuntimeTaskFailure : public std::exception {
public:
  RuntimeTaskFailure(std::string error_name, std::string message);
  const char *what() const noexcept override;
  const std::string &error_name() const;
  const std::string &message() const;

private:
  std::string error_name_;
  std::string message_;
  std::string what_;
};

class RuntimeTaskCancelled : public std::exception {
public:
  RuntimeTaskCancelled();
  const char *what() const noexcept override;
};

void throw_if_runtime_task_cancelled();

class RuntimeWorkerScope {
public:
  explicit RuntimeWorkerScope(std::uint64_t worker_id);
  RuntimeWorkerScope(const RuntimeWorkerScope &) = delete;
  RuntimeWorkerScope &operator=(const RuntimeWorkerScope &) = delete;
  ~RuntimeWorkerScope();

private:
  std::uint64_t previous_worker_id_ = 0;
};

class RuntimeStrandScope {
public:
  explicit RuntimeStrandScope(std::uint64_t strand_id);
  RuntimeStrandScope(const RuntimeStrandScope &) = delete;
  RuntimeStrandScope &operator=(const RuntimeStrandScope &) = delete;
  ~RuntimeStrandScope();

private:
  std::uint64_t previous_strand_id_ = 0;
};

enum class RuntimeStrandState {
  New,
  Runnable,
  Running,
  Sleeping,
  Waiting,
  Done,
  Finished = Done,
  Failed,
  Cancelled
};

struct RuntimeSchedulerConfig {
  std::size_t worker_count = 0;
  std::uint64_t first_worker_id = 1;
};

struct RuntimeSchedulerStats {
  std::uint64_t worker_count = 0;
  std::uint64_t strands_created = 0;
  std::uint64_t strands_completed = 0;
  std::uint64_t strands_failed = 0;
  std::uint64_t global_queue_enqueues = 0;
  std::uint64_t local_queue_enqueues = 0;
  std::uint64_t worker_dequeues = 0;
  std::uint64_t explicit_wakes = 0;
  std::uint64_t timer_wakes = 0;
  std::uint64_t coalesced_wakes = 0;
  std::uint64_t stale_timer_wakes = 0;
  std::uint64_t max_parallel_running = 0;
  std::uint64_t runnable_queue_depth = 0;
  std::uint64_t timer_queue_depth = 0;
  std::uint64_t sleeping_strands = 0;
  std::uint64_t tasks_created = 0;
  std::uint64_t tasks_completed = 0;
  std::uint64_t tasks_failed = 0;
  std::uint64_t tasks_cancelled = 0;
  std::uint64_t task_joins = 0;
  std::uint64_t task_join_timeouts = 0;
  std::uint64_t task_cancellation_requests = 0;
  std::uint64_t task_wait_state_entries = 0;
  // Layer B cooperative suspension: a running strand parked itself (releasing
  // its worker) rather than blocking it. park_resumes counts re-dispatches of a
  // previously parked strand.
  std::uint64_t parks = 0;
  std::uint64_t park_resumes = 0;
  std::uint64_t structured_child_tasks = 0;
  std::uint64_t first_failure_cancellations = 0;
  std::uint64_t supervisor_one_for_one_failures = 0;
  std::uint64_t supervisor_one_for_all_cancellations = 0;
  std::uint64_t supervisor_rest_for_one_cancellations = 0;
};

struct RuntimeStrandSnapshot {
  std::uint64_t strand_id = 0;
  RuntimeStrandState state = RuntimeStrandState::New;
  std::uint64_t worker_id = 0;
  std::uint64_t wake_generation = 0;
  bool wake_pending = false;
  std::uint64_t explicit_wakes = 0;
  std::uint64_t timer_wakes = 0;
};

struct RuntimeTaskSnapshot {
  std::uint64_t task_id = 0;
  std::uint64_t parent_task_id = 0;
  RuntimeStrandState state = RuntimeStrandState::New;
  std::uint64_t worker_id = 0;
  bool cancellation_requested = false;
  std::uint64_t total_children = 0;
  std::uint64_t active_children = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeTaskJoinResult {
  bool ok = false;
  bool joined = false;
  bool timed_out = false;
  bool cancelled = false;
  std::uint64_t task_id = 0;
  RuntimeStrandState state = RuntimeStrandState::New;
  std::string error_name;
  std::string message;
};

enum class RuntimeSupervisorPolicy {
  CancelScope,
  OneForOne,
  OneForAll,
  RestForOne
};

struct RuntimeTaskOptions {
  RuntimeSupervisorPolicy policy = RuntimeSupervisorPolicy::CancelScope;
};

struct RuntimeChannelResult {
  bool ok = false;
  bool sent = false;
  bool received = false;
  bool closed = false;
  bool timed_out = false;
  bool cancelled = false;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeChannelStats {
  std::uint64_t capacity = 0;
  std::uint64_t buffered_values = 0;
  std::uint64_t pending_senders = 0;
  std::uint64_t pending_receivers = 0;
  std::uint64_t sends = 0;
  std::uint64_t receives = 0;
  std::uint64_t closes = 0;
  std::uint64_t send_timeouts = 0;
  std::uint64_t receive_timeouts = 0;
  std::uint64_t send_cancellations = 0;
  std::uint64_t receive_cancellations = 0;
  std::uint64_t isolation_rejections = 0;
  bool closed = false;
};

enum class RuntimeAwaitableState { Pending, Ready, Failed, Cancelled };

struct RuntimeAwaitableResult {
  bool ok = false;
  bool ready = false;
  bool timed_out = false;
  bool cancelled = false;
  bool failed = false;
  RuntimeAwaitableState state = RuntimeAwaitableState::Pending;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeAwaitableStats {
  std::uint64_t waits = 0;
  std::uint64_t polls = 0;
  std::uint64_t completions = 0;
  std::uint64_t failures = 0;
  std::uint64_t cancellations = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t native_polls = 0;
  std::uint64_t native_cancellations = 0;
  std::uint64_t native_finishes = 0;
  RuntimeAwaitableState state = RuntimeAwaitableState::Pending;
  bool native_backed = false;
};

class RuntimeAwaitable {
public:
  RuntimeAwaitable();
  RuntimeAwaitable(const RuntimeAwaitable &) = delete;
  RuntimeAwaitable &operator=(const RuntimeAwaitable &) = delete;
  RuntimeAwaitable(RuntimeAwaitable &&) noexcept;
  RuntimeAwaitable &operator=(RuntimeAwaitable &&) noexcept;
  ~RuntimeAwaitable();

  static RuntimeAwaitable ready(Value value = Value::null());
  static RuntimeAwaitable from_native_wait(RuntimeHeap &heap,
                                           const RuntimePinToken &token);

  bool complete(Value value = Value::null());
  bool fail(std::string error_name, std::string message);
  bool cancel();
  RuntimeAwaitableResult
  await(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeAwaitableResult poll();
  RuntimeAwaitableState state() const;
  RuntimeAwaitableStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

enum class RuntimeMoveSlotState { Ready, Reserved, Moved };

struct RuntimeMoveReservation {
  std::uint64_t reservation_id = 0;
  Value value = Value::null();
  bool active = false;
};

struct RuntimeMoveResult {
  bool ok = false;
  bool moved = false;
  bool reserved = false;
  RuntimeMoveSlotState state = RuntimeMoveSlotState::Ready;
  RuntimeMoveReservation reservation;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

class RuntimeMoveSlot {
public:
  explicit RuntimeMoveSlot(Value value = Value::null());
  RuntimeMoveSlot(const RuntimeMoveSlot &) = delete;
  RuntimeMoveSlot &operator=(const RuntimeMoveSlot &) = delete;
  RuntimeMoveSlot(RuntimeMoveSlot &&) noexcept;
  RuntimeMoveSlot &operator=(RuntimeMoveSlot &&) noexcept;
  ~RuntimeMoveSlot();

  RuntimeMoveResult read() const;
  RuntimeMoveResult reserve_move();
  RuntimeMoveResult commit_move(RuntimeMoveReservation *reservation);
  RuntimeMoveResult release_move(RuntimeMoveReservation *reservation);
  void reset(Value value);
  RuntimeMoveSlotState state() const;
  bool moved() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeChannel {
public:
  explicit RuntimeChannel(std::size_t capacity = 0);
  RuntimeChannel(const RuntimeChannel &) = delete;
  RuntimeChannel &operator=(const RuntimeChannel &) = delete;
  RuntimeChannel(RuntimeChannel &&) noexcept;
  RuntimeChannel &operator=(RuntimeChannel &&) noexcept;
  ~RuntimeChannel();

  RuntimeChannelResult
  send(const Value &value,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeChannelResult
  send(RuntimeMoveSlot &slot,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeChannelResult
  recv(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  bool close();
  bool closed() const;
  RuntimeChannelStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

enum class RuntimeSelectArmKind { Recv, Send, Await };

struct RuntimeSelectArm {
  RuntimeSelectArmKind kind = RuntimeSelectArmKind::Recv;
  RuntimeChannel *channel = nullptr;
  RuntimeAwaitable *awaitable = nullptr;
  Value value = Value::null();
  RuntimeMoveSlot *move_slot = nullptr;

  static RuntimeSelectArm recv(RuntimeChannel &channel);
  static RuntimeSelectArm send(RuntimeChannel &channel, Value value);
  static RuntimeSelectArm send_moved(RuntimeChannel &channel,
                                     RuntimeMoveSlot &slot);
  static RuntimeSelectArm awaitable_arm(RuntimeAwaitable &awaitable);
};

struct RuntimeSelectResult {
  bool ok = false;
  bool selected = false;
  bool else_selected = false;
  bool timed_out = false;
  bool cancelled = false;
  std::size_t arm_index = 0;
  RuntimeSelectArmKind kind = RuntimeSelectArmKind::Recv;
  RuntimeChannelResult channel_result;
  RuntimeAwaitableResult awaitable_result;
  std::string error_name;
  std::string message;
};

RuntimeSelectResult runtime_select(
    const std::vector<RuntimeSelectArm> &arms,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
    bool has_else = false);

struct RuntimeMutexResult {
  bool ok = false;
  bool locked = false;
  bool unlocked = false;
  bool timed_out = false;
  bool cancelled = false;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeMutexStats {
  std::uint64_t lock_attempts = 0;
  std::uint64_t locks = 0;
  std::uint64_t unlocks = 0;
  std::uint64_t contentions = 0;
  std::uint64_t reentrant_failures = 0;
  std::uint64_t lock_timeouts = 0;
  std::uint64_t lock_cancellations = 0;
  std::uint64_t waiting_lockers = 0;
  std::uint64_t owner_id = 0;
  bool locked = false;
};

class RuntimeMutex {
public:
  RuntimeMutex();
  RuntimeMutex(const RuntimeMutex &) = delete;
  RuntimeMutex &operator=(const RuntimeMutex &) = delete;
  RuntimeMutex(RuntimeMutex &&) noexcept;
  RuntimeMutex &operator=(RuntimeMutex &&) noexcept;
  ~RuntimeMutex();

  RuntimeMutexResult
  lock(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeMutexResult unlock();
  RuntimeMutexResult synchronize(
      std::function<Value()> function,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  bool locked() const;
  bool owned() const;
  RuntimeMutexStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeAtomic {
public:
  explicit RuntimeAtomic(std::int64_t value = 0);
  explicit RuntimeAtomic(Value value);
  RuntimeAtomic(const RuntimeAtomic &) = delete;
  RuntimeAtomic &operator=(const RuntimeAtomic &) = delete;
  RuntimeAtomic(RuntimeAtomic &&) noexcept;
  RuntimeAtomic &operator=(RuntimeAtomic &&) noexcept;
  ~RuntimeAtomic();

  struct Result {
    bool ok = false;
    bool matched = false;
    bool updated = false;
    Value value = Value::null();
    std::uint64_t attempts = 0;
    std::string error_name;
    std::string message;
  };

  std::int64_t get() const;
  void set(std::int64_t value);
  bool compare_and_set(std::int64_t expected, std::int64_t desired);
  Value get_value() const;
  Result set_value(Value value);
  Result compare_and_set_value(const Value &expected, Value desired);
  Result update(std::function<Value(const Value &)> function);

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

struct RuntimeBarrierResult {
  bool ok = false;
  bool passed = false;
  bool last = false;
  bool timed_out = false;
  bool cancelled = false;
  std::uint64_t generation = 0;
  std::uint64_t parties = 0;
  std::uint64_t waiting = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeBarrierStats {
  std::uint64_t parties = 0;
  std::uint64_t waiting = 0;
  std::uint64_t generation = 0;
  std::uint64_t arrivals = 0;
  std::uint64_t passes = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t cancellations = 0;
};

class RuntimeBarrier {
public:
  explicit RuntimeBarrier(std::size_t parties);
  RuntimeBarrier(const RuntimeBarrier &) = delete;
  RuntimeBarrier &operator=(const RuntimeBarrier &) = delete;
  RuntimeBarrier(RuntimeBarrier &&) noexcept;
  RuntimeBarrier &operator=(RuntimeBarrier &&) noexcept;
  ~RuntimeBarrier();

  RuntimeBarrierResult
  wait(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeBarrierStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

bool runtime_value_is_shareable(const Value &value);

class RuntimeScheduler {
public:
  using StrandFunction = std::function<void()>;

  explicit RuntimeScheduler(std::size_t worker_count = 0);
  explicit RuntimeScheduler(RuntimeSchedulerConfig config);
  RuntimeScheduler(const RuntimeScheduler &) = delete;
  RuntimeScheduler &operator=(const RuntimeScheduler &) = delete;
  RuntimeScheduler(RuntimeScheduler &&) noexcept;
  RuntimeScheduler &operator=(RuntimeScheduler &&) noexcept;
  ~RuntimeScheduler();

  void start();
  void shutdown();
  std::uint64_t spawn_strand(StrandFunction function);
  std::uint64_t spawn_sleeping_strand(std::chrono::milliseconds delay,
                                      StrandFunction function);
  std::uint64_t spawn_task(StrandFunction function);
  std::uint64_t spawn_task(RuntimeTaskOptions options, StrandFunction function);
  std::uint64_t spawn_sleeping_task(std::chrono::milliseconds delay,
                                    StrandFunction function);
  std::uint64_t spawn_sleeping_task(std::chrono::milliseconds delay,
                                    RuntimeTaskOptions options,
                                    StrandFunction function);
  bool wake_strand(std::uint64_t strand_id);

  // Layer B cooperative suspension. Called by the currently-running strand
  // (from within its strand function) to request that, when the function
  // returns, the strand be parked and its worker released to run other strands,
  // rather than finalized. The same strand function is re-invoked when the
  // strand is woken, so it must resume its work (e.g. continue a persisted VM)
  // rather than restart it. With `wake_after` a timer wake is scheduled;
  // otherwise the strand stays parked until an explicit wake_strand(). Returns
  // false when there is no current scheduler strand, in which case the caller
  // must fall back to blocking.
  bool park_current(
      std::optional<std::chrono::milliseconds> wake_after = std::nullopt);

  bool cancel_task(std::uint64_t task_id);
  bool task_cancel_requested(std::uint64_t task_id) const;
  RuntimeTaskJoinResult join_task(
      std::uint64_t task_id,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  bool wait_until_idle(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
  RuntimeSchedulerStats stats() const;
  std::optional<RuntimeStrandSnapshot>
  strand_snapshot(std::uint64_t strand_id) const;
  std::optional<RuntimeTaskSnapshot> task_snapshot(std::uint64_t task_id) const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

enum class RuntimeTaskHandleState {
  Inactive,
  New,
  Runnable,
  Running,
  Sleeping,
  Waiting,
  Done,
  Failed,
  Cancelled
};

struct RuntimeTaskHandleSnapshot {
  bool active = false;
  std::uint64_t task_id = 0;
  std::uint64_t strand_id = 0;
  RuntimeTaskHandleState state = RuntimeTaskHandleState::Inactive;
  bool ready = false;
  bool succeeded = false;
  bool cancelled = false;
  bool failed = false;
  bool running = false;
  bool cancellation_requested = false;
  std::string error_name;
  std::string message;
};

struct RuntimeTaskPublicResult {
  bool ok = false;
  bool ready = false;
  bool timed_out = false;
  bool cancelled = false;
  bool failed = false;
  RuntimeTaskHandleState state = RuntimeTaskHandleState::Inactive;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeTaskFailureInfo {
  bool ready = false;
  bool failed = false;
  bool cancelled = false;
  RuntimeTaskHandleState state = RuntimeTaskHandleState::Inactive;
  std::string error_name;
  std::string message;
};

class RuntimeTaskHandle {
public:
  RuntimeTaskHandle();

  bool active() const;
  std::uint64_t task_id() const;
  std::uint64_t strand_id() const;
  RuntimeTaskHandleState state() const;
  RuntimeTaskHandleSnapshot snapshot() const;

  RuntimeTaskPublicResult wait(std::chrono::milliseconds timeout =
                                   std::chrono::milliseconds::max()) const;
  RuntimeTaskPublicResult result() const;
  RuntimeTaskFailureInfo failure() const;

  bool cancel() const;
  bool resume() const;
  bool cancelled() const;
  bool done() const;
  bool running() const;
  bool failed() const;

private:
  struct State;

  RuntimeTaskHandle(std::shared_ptr<RuntimeScheduler> scheduler,
                    std::uint64_t task_id, std::shared_ptr<State> state);

  std::shared_ptr<RuntimeScheduler> scheduler_;
  std::uint64_t task_id_ = 0;
  std::shared_ptr<State> state_;

  friend class RuntimeTaskModule;
};

class RuntimeTaskModule {
public:
  using TaskFunction = std::function<Value()>;

  explicit RuntimeTaskModule(std::size_t worker_count = 0);
  explicit RuntimeTaskModule(RuntimeSchedulerConfig config);

  RuntimeTaskHandle async(TaskFunction function);
  RuntimeTaskHandle spawn(TaskFunction function);

  Value sync(TaskFunction function) const;
  bool sync_active() const;
  void sleep(std::chrono::milliseconds duration) const;
  void yield_current() const;

  RuntimeScheduler &scheduler();
  const RuntimeScheduler &scheduler() const;

private:
  enum class SpawnKind { SameStrand, NewStrand };

  RuntimeTaskHandle spawn_with_kind(SpawnKind kind, TaskFunction function);

  std::shared_ptr<RuntimeScheduler> scheduler_;
};

enum class RuntimeFlowFailurePolicy { First, Collect, Ignore };

enum class RuntimeFlowIsolationMode { Checked, Unchecked };

enum class RuntimeFlowPartitionPolicy { Items, Chunks, Stride, Atomic };

struct RuntimeFlowOptions {
  std::size_t workers = 0;
  bool ordered = true;
  RuntimeFlowFailurePolicy failure_policy = RuntimeFlowFailurePolicy::First;
  RuntimeFlowIsolationMode isolation = RuntimeFlowIsolationMode::Checked;
  RuntimeFlowPartitionPolicy partition_policy =
      RuntimeFlowPartitionPolicy::Items;
  std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
};

struct RuntimeFlowFailure {
  std::size_t index = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeFlowGatherResult {
  bool ok = false;
  bool failed = false;
  bool timed_out = false;
  bool cancelled = false;
  std::vector<Value> values;
  std::vector<RuntimeFlowFailure> failures;
  std::uint64_t completed_count = 0;
  std::uint64_t failed_count = 0;
  std::uint64_t cancelled_count = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeFlowReduceResult {
  bool ok = false;
  bool failed = false;
  Value value = Value::null();
  RuntimeFlowGatherResult gather;
  std::string error_name;
  std::string message;
};

struct RuntimeFlowStats {
  std::uint64_t flows = 0;
  std::uint64_t gathers = 0;
  std::uint64_t worker_tasks = 0;
  std::uint64_t completed_workers = 0;
  std::uint64_t failed_workers = 0;
  std::uint64_t cancelled_workers = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t reductions = 0;
  std::uint64_t broadcasts = 0;
  std::uint64_t isolation_rejections = 0;
};

class RuntimeFlowModule {
public:
  using MapFunction = std::function<Value(const Value &, std::size_t)>;
  using ReduceFunction = std::function<Value(const Value &, const Value &)>;
  using BroadcastFunction = std::function<Value(const Value &, std::size_t)>;

  explicit RuntimeFlowModule(std::size_t worker_count = 0);
  explicit RuntimeFlowModule(RuntimeSchedulerConfig config);
  RuntimeFlowModule(const RuntimeFlowModule &) = delete;
  RuntimeFlowModule &operator=(const RuntimeFlowModule &) = delete;
  RuntimeFlowModule(RuntimeFlowModule &&) noexcept;
  RuntimeFlowModule &operator=(RuntimeFlowModule &&) noexcept;
  ~RuntimeFlowModule();

  RuntimeFlowGatherResult gather(std::vector<RuntimeTaskHandle> handles,
                                 RuntimeFlowOptions options = {});
  RuntimeFlowGatherResult scatter(std::vector<Value> partitions,
                                  MapFunction function,
                                  RuntimeFlowOptions options = {});
  RuntimeFlowGatherResult scatter_map(std::vector<Value> items,
                                      MapFunction function,
                                      RuntimeFlowOptions options = {});
  RuntimeFlowReduceResult scatter_reduce(std::vector<Value> items, Value init,
                                         MapFunction map, ReduceFunction reduce,
                                         RuntimeFlowOptions options = {});
  RuntimeFlowGatherResult broadcast(Value value, std::size_t workers,
                                    BroadcastFunction function,
                                    RuntimeFlowOptions options = {});
  RuntimeFlowStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

struct RuntimeThreadedCollectionStats {
  std::uint64_t operations = 0;
  std::uint64_t each_operations = 0;
  std::uint64_t map_operations = 0;
  std::uint64_t filter_operations = 0;
  std::uint64_t filter_map_operations = 0;
  std::uint64_t flat_map_operations = 0;
  std::uint64_t combination_operations = 0;
  std::uint64_t permutation_operations = 0;
  std::uint64_t generated_values = 0;
  RuntimeFlowStats flow;
};

class RuntimeThreadedCollection {
public:
  using EachFunction = std::function<void(const Value &, std::size_t)>;
  using MapFunction = std::function<Value(const Value &, std::size_t)>;
  using PredicateFunction = std::function<bool(const Value &, std::size_t)>;
  using FlatMapFunction =
      std::function<std::vector<Value>(const Value &, std::size_t)>;

  explicit RuntimeThreadedCollection(std::vector<Value> items,
                                     std::size_t workers = 0,
                                     RuntimeFlowOptions options = {},
                                     RuntimeFlowPartitionPolicy scatter_policy =
                                         RuntimeFlowPartitionPolicy::Atomic);
  RuntimeThreadedCollection(const RuntimeThreadedCollection &) = delete;
  RuntimeThreadedCollection &
  operator=(const RuntimeThreadedCollection &) = delete;
  RuntimeThreadedCollection(RuntimeThreadedCollection &&) noexcept;
  RuntimeThreadedCollection &operator=(RuntimeThreadedCollection &&) noexcept;
  ~RuntimeThreadedCollection();

  RuntimeFlowGatherResult each(EachFunction function);
  RuntimeFlowGatherResult map(MapFunction function);
  RuntimeFlowGatherResult filter_map(MapFunction function);
  RuntimeFlowGatherResult select(PredicateFunction function);
  RuntimeFlowGatherResult reject(PredicateFunction function);
  RuntimeFlowGatherResult flat_map(FlatMapFunction function);
  RuntimeFlowGatherResult combination(std::size_t count);
  RuntimeFlowGatherResult permutation(std::size_t count);
  RuntimeThreadedCollectionStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

// Layer B observability/test hook: process-wide count of cooperative task
// parks (a task body suspended at a suspension point such as task.sleep,
// releasing its worker, rather than blocking it). Monotonic; snapshot it before
// and after a run to assert the cooperative path was taken.
std::uint64_t runtime_cooperative_task_park_count();

} // namespace amber::runtime
