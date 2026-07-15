#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/reactor.h"
#include "runtime/vm.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib task test failed: " << message << "\n";
    std::exit(1);
  }
}

bool wait_for_condition(const std::function<bool()> &condition,
                        std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::yield();
  }
  return condition();
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-task-source-test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }

  amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emit_result.module));
  if (!decoded.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult
execute_source_or_die(const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "source module should have init code");
  return amber::runtime::execute_code(module, module.init.entry_code_id);
}

void expect_integer(const amber::runtime::Value &value, std::int64_t expected,
                    const std::string &message) {
  expect(value.is_integer(), message + " should be integer");
  expect(value.as_integer() == expected, message + " value");
}

void expect_bool(const amber::runtime::Value &value, bool expected,
                 const std::string &message) {
  expect(value.is_bool(), message + " should be bool");
  expect(value.as_bool() == expected, message + " value");
}

void expect_channel_integer(const amber::runtime::RuntimeChannelResult &result,
                            std::int64_t expected, const std::string &message) {
  expect(result.ok && result.received, message + " should receive");
  expect_integer(result.value, expected, message);
}

void expect_integer_values(const std::vector<amber::runtime::Value> &values,
                           const std::vector<std::int64_t> &expected,
                           const std::string &message) {
  expect(values.size() == expected.size(), message + " size");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    expect_integer(values[index], expected[index],
                   message + " item " + std::to_string(index));
  }
}

void expect_integer_list_value(const amber::runtime::Value &value,
                               const std::vector<std::int64_t> &expected,
                               const std::string &message) {
  expect(value.is_list() && value.as_list() != nullptr,
         message + " should be list");
  expect_integer_values(value.as_list()->items, expected, message);
}

void expect_integer_list_values(
    const std::vector<amber::runtime::Value> &values,
    const std::vector<std::vector<std::int64_t>> &expected,
    const std::string &message) {
  expect(values.size() == expected.size(), message + " size");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    expect_integer_list_value(values[index], expected[index],
                              message + " item " + std::to_string(index));
  }
}

void expect_state(amber::runtime::RuntimeTaskHandleState actual,
                  amber::runtime::RuntimeTaskHandleState expected,
                  const std::string &message) {
  expect(actual == expected, message);
}

void test_std010_task_async_spawn_and_wait_return_values() {
  amber::runtime::RuntimeTaskModule task(2);

  const amber::runtime::RuntimeTaskHandle same_strand =
      task.async([]() { return amber::runtime::Value::integer(21); });
  const amber::runtime::RuntimeTaskHandle new_strand =
      task.spawn([]() { return amber::runtime::Value::integer(42); });

  expect(same_strand.active(), "task.async should return active handle");
  expect(new_strand.active(), "task.spawn should return active handle");
  expect(same_strand.task_id() != 0 && new_strand.task_id() != 0,
         "task handles should expose stable task ids");
  expect(same_strand.strand_id() == same_strand.task_id(),
         "same-strand handle should expose runtime strand id");

  const amber::runtime::RuntimeTaskPublicResult same_result =
      same_strand.wait(std::chrono::milliseconds(1000));
  const amber::runtime::RuntimeTaskPublicResult spawn_result =
      new_strand.wait(std::chrono::milliseconds(1000));

  expect(same_result.ok && same_result.ready, "task.async wait should succeed");
  expect(spawn_result.ok && spawn_result.ready,
         "task.spawn wait should succeed");
  expect_integer(same_result.value, 21, "task.async wait result");
  expect_integer(spawn_result.value, 42, "task.spawn wait result");
  expect(same_strand.done() && new_strand.done(),
         "completed handles should report done");
  expect(!same_strand.failed() && !new_strand.cancelled(),
         "successful handles should not report failure/cancellation");

  const amber::runtime::RuntimeTaskPublicResult nonblocking =
      new_strand.result();
  expect(nonblocking.ok && nonblocking.ready,
         "result() should read completed task without blocking");
  expect_state(nonblocking.state, amber::runtime::RuntimeTaskHandleState::Done,
               "result() should expose done state");
  expect_integer(nonblocking.value, 42, "task.spawn result()");
}

void test_std010_task_wait_timeout_does_not_cancel_child() {
  amber::runtime::RuntimeTaskModule task(2);

  const amber::runtime::RuntimeTaskHandle handle = task.spawn([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return amber::runtime::Value::integer(7);
  });

  const amber::runtime::RuntimeTaskPublicResult timed_out =
      handle.wait(std::chrono::milliseconds(1));
  expect(!timed_out.ok && timed_out.timed_out &&
             timed_out.error_name == "TimeoutError",
         "wait(timeout) should report TimeoutError");
  const bool timeout_state_is_current =
      timed_out.state == amber::runtime::RuntimeTaskHandleState::Runnable ||
      timed_out.state == amber::runtime::RuntimeTaskHandleState::Running ||
      timed_out.state == amber::runtime::RuntimeTaskHandleState::Done;
  expect(timeout_state_is_current,
         "timed-out wait should expose current task state");
  expect(!handle.cancelled(), "wait timeout should not cancel child task");

  const amber::runtime::RuntimeTaskPublicResult joined =
      handle.wait(std::chrono::milliseconds(1000));
  expect(joined.ok && joined.ready, "timed-out child should still be joinable");
  expect_integer(joined.value, 7, "timed-out child eventual result");
}

void test_std010_task_yield_sleep_and_cancel_surface() {
  amber::runtime::RuntimeTaskModule task(2);
  std::atomic<bool> entered{false};

  const amber::runtime::RuntimeTaskHandle handle =
      task.spawn([&task, &entered]() {
        entered = true;
        while (!amber::runtime::current_runtime_task_cancel_requested()) {
          task.yield_current();
        }
        task.sleep(std::chrono::milliseconds(0));
        return amber::runtime::Value::integer(99);
      });

  expect(wait_for_condition([&entered]() { return entered.load(); },
                            std::chrono::milliseconds(1000)),
         "spawned task should enter cancellation loop");
  expect(handle.cancel(), "cancel should request child cancellation");

  const amber::runtime::RuntimeTaskPublicResult cancelled =
      handle.wait(std::chrono::milliseconds(1000));
  expect(!cancelled.ok && cancelled.cancelled &&
             cancelled.error_name == "CancelledError",
         "cancelled child wait should report CancelledError");
  expect_state(cancelled.state,
               amber::runtime::RuntimeTaskHandleState::Cancelled,
               "cancelled child wait should expose cancelled state");
  expect(handle.cancelled(), "cancelled handle should report cancellation");

  const amber::runtime::RuntimeTaskFailureInfo failure = handle.failure();
  expect(failure.ready && failure.cancelled &&
             failure.error_name == "CancelledError",
         "failure() should expose cancellation state");
  expect_state(failure.state, amber::runtime::RuntimeTaskHandleState::Cancelled,
               "failure() should expose cancelled state");

  const amber::runtime::RuntimeTaskPublicResult result = handle.result();
  expect(result.ready && result.cancelled &&
             result.error_name == "CancelledError",
         "result() should expose cancelled state");
  expect_state(result.state, amber::runtime::RuntimeTaskHandleState::Cancelled,
               "result() should expose cancelled state");
}

void test_std010_task_sync_block_suppresses_cooperative_yield() {
  amber::runtime::RuntimeTaskModule task(1);
  std::atomic<int> marker{0};
  std::atomic<bool> release{false};

  const amber::runtime::RuntimeTaskHandle guarded =
      task.spawn([&task, &marker, &release]() {
        return task.sync([&task, &marker, &release]() {
          expect(task.sync_active() &&
                     amber::runtime::current_runtime_task_sync_active(),
                 "task sync block should expose active sync state");
          marker = 1;

          const amber::runtime::Value nested = task.sync([&task]() {
            expect(task.sync_active(),
                   "nested task sync block should keep sync active");
            return amber::runtime::Value::integer(5);
          });
          expect_integer(nested, 5, "nested task sync return value");

          for (int iteration = 0; iteration < 1000; ++iteration) {
            task.yield_current();
            task.sleep(std::chrono::milliseconds(0));
            expect(marker.load() == 1,
                   "task sync yield should not switch to sibling task");
          }

          while (!release.load()) {
            task.yield_current();
          }
          expect(marker.load() == 1,
                 "sibling task should not run until sync block exits");
          return amber::runtime::Value::integer(11);
        });
      });

  expect(wait_for_condition([&marker]() { return marker.load() == 1; },
                            std::chrono::milliseconds(1000)),
         "sync guard task should enter sync block");

  const amber::runtime::RuntimeTaskHandle sibling = task.spawn([&marker]() {
    marker = 2;
    return amber::runtime::Value::integer(22);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  expect(marker.load() == 1,
         "queued sibling should wait while sync block is active");

  release = true;
  const amber::runtime::RuntimeTaskPublicResult guarded_result =
      guarded.wait(std::chrono::milliseconds(1000));
  expect(guarded_result.ok && guarded_result.ready,
         "sync guard task should finish");
  expect_integer(guarded_result.value, 11, "task sync return value");

  const amber::runtime::RuntimeTaskPublicResult sibling_result =
      sibling.wait(std::chrono::milliseconds(1000));
  expect(sibling_result.ok && sibling_result.ready,
         "sibling task should run after sync block exits");
  expect_integer(sibling_result.value, 22, "sibling task result");
  expect(marker.load() == 2,
         "sibling task should update marker after sync block exits");
}

void test_std010_task_failure_is_reported_by_handle() {
  amber::runtime::RuntimeTaskModule task(2);

  const amber::runtime::RuntimeTaskHandle handle = task.spawn([]() {
    throw amber::runtime::RuntimeTaskFailure("BoomError", "boom");
    return amber::runtime::Value::null();
  });

  const amber::runtime::RuntimeTaskPublicResult waited =
      handle.wait(std::chrono::milliseconds(1000));
  expect(!waited.ok && waited.failed && waited.error_name == "BoomError",
         "failed child wait should surface original error");
  expect_state(waited.state, amber::runtime::RuntimeTaskHandleState::Failed,
               "failed child wait should expose failed state");
  expect(handle.failed(), "failed handle should report failed state");

  const amber::runtime::RuntimeTaskFailureInfo failure = handle.failure();
  expect(failure.ready && failure.failed && failure.error_name == "BoomError",
         "failure() should expose failed state");
  expect_state(failure.state, amber::runtime::RuntimeTaskHandleState::Failed,
               "failure() should expose failed state");

  const amber::runtime::RuntimeTaskPublicResult result = handle.result();
  expect(result.ready && result.failed && result.error_name == "BoomError",
         "result() should expose original failed state");
  expect_state(result.state, amber::runtime::RuntimeTaskHandleState::Failed,
               "result() should expose failed state");
}

void test_std011_task_handle_state_result_failure_contract() {
  amber::runtime::RuntimeTaskHandle inactive;
  const amber::runtime::RuntimeTaskHandleSnapshot inactive_snapshot =
      inactive.snapshot();
  expect(!inactive_snapshot.active,
         "default task handle snapshot should be inactive");
  expect_state(inactive_snapshot.state,
               amber::runtime::RuntimeTaskHandleState::Inactive,
               "default task handle should expose inactive state");
  expect(inactive.result().error_name == "LifetimeError",
         "inactive result() should report LifetimeError");
  expect(inactive.failure().error_name == "LifetimeError",
         "inactive failure() should report LifetimeError");

  amber::runtime::RuntimeTaskModule task(2);
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};

  const amber::runtime::RuntimeTaskHandle handle =
      task.spawn([&entered, &release]() {
        entered = true;
        while (!release.load()) {
          std::this_thread::yield();
        }
        return amber::runtime::Value::integer(123);
      });

  expect(wait_for_condition([&entered]() { return entered.load(); },
                            std::chrono::milliseconds(1000)),
         "state contract task should enter running loop");

  const amber::runtime::RuntimeTaskHandleSnapshot running_snapshot =
      handle.snapshot();
  expect(running_snapshot.active, "running snapshot should be active");
  expect(!running_snapshot.ready, "running snapshot should not be ready");
  expect(running_snapshot.running,
         "running snapshot should expose running state");
  expect_state(handle.state(), amber::runtime::RuntimeTaskHandleState::Running,
               "state() should expose running state");

  const amber::runtime::RuntimeTaskPublicResult early_result = handle.result();
  expect(!early_result.ok && !early_result.ready &&
             early_result.error_name == "TaskNotDoneError",
         "result() should be non-blocking for unfinished tasks");
  expect_state(early_result.state,
               amber::runtime::RuntimeTaskHandleState::Running,
               "unfinished result() should expose running state");

  const amber::runtime::RuntimeTaskFailureInfo early_failure = handle.failure();
  expect(!early_failure.ready && early_failure.error_name == "TaskNotDoneError",
         "failure() should be non-blocking for unfinished tasks");
  expect_state(early_failure.state,
               amber::runtime::RuntimeTaskHandleState::Running,
               "unfinished failure() should expose running state");

  release = true;
  const amber::runtime::RuntimeTaskPublicResult joined =
      handle.wait(std::chrono::milliseconds(1000));
  expect(joined.ok && joined.ready, "released task should join successfully");
  expect_state(joined.state, amber::runtime::RuntimeTaskHandleState::Done,
               "joined task should expose done state");

  const amber::runtime::RuntimeTaskHandleSnapshot done_snapshot =
      handle.snapshot();
  expect(done_snapshot.ready && done_snapshot.succeeded,
         "done snapshot should expose success");
  expect_state(done_snapshot.state,
               amber::runtime::RuntimeTaskHandleState::Done,
               "done snapshot should expose done state");

  const amber::runtime::RuntimeTaskFailureInfo done_failure = handle.failure();
  expect(done_failure.ready && !done_failure.failed &&
             !done_failure.cancelled && done_failure.error_name.empty(),
         "failure() should return a ready empty failure for successful tasks");
  expect_state(done_failure.state, amber::runtime::RuntimeTaskHandleState::Done,
               "successful failure() should expose done state");
}

void test_std012_channel_buffered_fifo_close_and_isolation() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::RuntimeChannel channel(3);

  expect(channel.send(amber::runtime::Value::integer(1)).ok,
         "buffered channel should accept first value");
  expect(channel.send(amber::runtime::Value::integer(2)).ok,
         "buffered channel should accept second value");
  expect(channel.send(amber::runtime::Value::integer(3)).ok,
         "buffered channel should accept third value");

  const amber::runtime::RuntimeChannelStats filled_stats = channel.stats();
  expect(filled_stats.capacity == 3 && filled_stats.buffered_values == 3,
         "channel stats should expose capacity and buffered count");

  const amber::runtime::Value confined = heap.make_list_value({});
  const amber::runtime::RuntimeChannelResult rejected =
      channel.send(confined, std::chrono::milliseconds(0));
  expect(!rejected.ok && rejected.error_name == "IsolationError",
         "checked channel send should reject confined payload");

  const amber::runtime::Value transitively_confined =
      heap.make_list_value({confined}, true);
  const amber::runtime::RuntimeChannelResult nested_rejected =
      channel.send(transitively_confined, std::chrono::milliseconds(0));
  expect(!nested_rejected.ok && nested_rejected.error_name == "IsolationError",
         "checked channel send should reject transitively confined payload");

  expect(channel.close(), "channel close should mark open channel closed");
  expect(!channel.close(), "channel close should be idempotent");
  expect(channel.closed(), "closed? should report closed channel");

  const amber::runtime::RuntimeChannelResult send_after_close =
      channel.send(amber::runtime::Value::integer(4));
  expect(!send_after_close.ok && send_after_close.closed &&
             send_after_close.error_name == "ChannelClosedError",
         "send after close should report ChannelClosedError");

  expect_channel_integer(channel.recv(), 1,
                         "closed buffered channel first FIFO recv");
  expect_channel_integer(channel.recv(), 2,
                         "closed buffered channel second FIFO recv");
  expect_channel_integer(channel.recv(), 3,
                         "closed buffered channel third FIFO recv");

  const amber::runtime::RuntimeChannelResult closed_empty = channel.recv();
  expect(!closed_empty.ok && closed_empty.closed &&
             closed_empty.error_name == "ChannelClosedError",
         "closed empty channel recv should report ChannelClosedError");

  const amber::runtime::RuntimeChannelStats stats = channel.stats();
  expect(stats.sends == 3 && stats.receives == 3 && stats.closes == 1,
         "channel stats should count successful sends, receives, and close");
  expect(stats.isolation_rejections == 2,
         "channel stats should count isolation rejections");
}

void test_std012_channel_waiting_senders_fifo_and_send_timeout() {
  amber::runtime::RuntimeTaskModule task(4);
  amber::runtime::RuntimeChannel channel(0);
  std::atomic<int> released_senders{0};
  std::vector<amber::runtime::RuntimeTaskHandle> senders;

  for (int value = 1; value <= 3; ++value) {
    senders.push_back(task.spawn([&channel, &released_senders, value]() {
      const amber::runtime::RuntimeChannelResult sent =
          channel.send(amber::runtime::Value::integer(value),
                       std::chrono::milliseconds(1000));
      expect(sent.ok && sent.sent, "waiting sender should complete");
      released_senders.fetch_add(1);
      return amber::runtime::Value::integer(value);
    }));
    expect(wait_for_condition(
               [&channel, value]() {
                 return channel.stats().pending_senders ==
                        static_cast<std::uint64_t>(value);
               },
               std::chrono::milliseconds(1000)),
           "sender should enter FIFO wait queue");
  }

  expect(released_senders.load() == 0,
         "rendezvous senders should wait until receivers arrive");

  for (int expected = 1; expected <= 3; ++expected) {
    const amber::runtime::RuntimeChannelResult received =
        channel.recv(std::chrono::milliseconds(1000));
    expect_channel_integer(received, expected,
                           "rendezvous channel waiting-sender FIFO recv");
  }

  for (const amber::runtime::RuntimeTaskHandle &sender : senders) {
    const amber::runtime::RuntimeTaskPublicResult joined =
        sender.wait(std::chrono::milliseconds(1000));
    expect(joined.ok && joined.ready, "sender task should join");
  }
  expect(released_senders.load() == 3,
         "all waiting senders should be released by receives");

  const amber::runtime::RuntimeChannelResult timed_out = channel.send(
      amber::runtime::Value::integer(99), std::chrono::milliseconds(5));
  expect(!timed_out.ok && timed_out.timed_out &&
             timed_out.error_name == "TimeoutError",
         "unmatched rendezvous send should support timeout");

  const amber::runtime::RuntimeChannelStats stats = channel.stats();
  expect(stats.sends == 3 && stats.receives == 3 && stats.send_timeouts == 1,
         "channel stats should count waiting sends and send timeout");
}

void test_std012_channel_waiting_receivers_fifo_timeout_and_cancellation() {
  amber::runtime::RuntimeTaskModule task(4);
  amber::runtime::RuntimeChannel channel(0);
  std::mutex received_mutex;
  std::vector<std::int64_t> received_by_receiver(3, 0);
  std::vector<amber::runtime::RuntimeTaskHandle> receivers;

  for (int receiver_id = 1; receiver_id <= 3; ++receiver_id) {
    receivers.push_back(task.spawn([&channel, &received_mutex,
                                    &received_by_receiver, receiver_id]() {
      const amber::runtime::RuntimeChannelResult received =
          channel.recv(std::chrono::milliseconds(1000));
      expect(received.ok && received.received && received.value.is_integer(),
             "waiting receiver should receive value");
      {
        std::lock_guard<std::mutex> lock(received_mutex);
        received_by_receiver[static_cast<std::size_t>(receiver_id - 1)] =
            received.value.as_integer();
      }
      return amber::runtime::Value::integer(received.value.as_integer());
    }));
    expect(wait_for_condition(
               [&channel, receiver_id]() {
                 return channel.stats().pending_receivers ==
                        static_cast<std::uint64_t>(receiver_id);
               },
               std::chrono::milliseconds(1000)),
           "receiver should enter FIFO wait queue");
  }

  for (int value = 10; value <= 12; ++value) {
    const amber::runtime::RuntimeChannelResult sent = channel.send(
        amber::runtime::Value::integer(value), std::chrono::milliseconds(1000));
    expect(sent.ok && sent.sent, "send should release waiting receiver");
  }

  for (const amber::runtime::RuntimeTaskHandle &receiver : receivers) {
    const amber::runtime::RuntimeTaskPublicResult joined =
        receiver.wait(std::chrono::milliseconds(1000));
    expect(joined.ok && joined.ready, "receiver task should join");
  }

  {
    std::lock_guard<std::mutex> lock(received_mutex);
    expect(received_by_receiver[0] == 10 && received_by_receiver[1] == 11 &&
               received_by_receiver[2] == 12,
           "waiting receivers should be served FIFO");
  }

  amber::runtime::RuntimeChannel timeout_channel(0);
  const amber::runtime::RuntimeChannelResult timed_out =
      timeout_channel.recv(std::chrono::milliseconds(5));
  expect(!timed_out.ok && timed_out.timed_out &&
             timed_out.error_name == "TimeoutError",
         "empty open channel recv should support timeout");

  amber::runtime::RuntimeTaskModule cancel_task(2);
  amber::runtime::RuntimeChannel cancellation_channel(0);
  std::atomic<bool> cancellation_waiter_entered{false};
  const amber::runtime::RuntimeTaskHandle waiting_receiver = cancel_task.spawn(
      [&cancellation_channel, &cancellation_waiter_entered]() {
        cancellation_waiter_entered = true;
        const amber::runtime::RuntimeChannelResult received =
            cancellation_channel.recv(std::chrono::milliseconds(50));
        if (received.cancelled && received.error_name == "CancelledError") {
          return amber::runtime::Value::integer(1);
        }
        if (received.timed_out) {
          return amber::runtime::Value::integer(2);
        }
        return amber::runtime::Value::integer(3);
      });

  expect(wait_for_condition(
             [&cancellation_waiter_entered, &cancellation_channel]() {
               return cancellation_waiter_entered.load() &&
                      cancellation_channel.stats().pending_receivers == 1;
             },
             std::chrono::milliseconds(1000)),
         "cancellable receiver should enter wait queue");
  expect(waiting_receiver.cancel(),
         "cancelling receiver task should request cancellation");
  const amber::runtime::RuntimeTaskPublicResult cancelled =
      waiting_receiver.wait(std::chrono::milliseconds(1000));
  expect(cancelled.ok && cancelled.ready,
         "channel cancellation observer should finish task");
  expect_integer(cancelled.value, 1,
                 "channel recv should report CancelledError before timeout");
}

void test_std013_mutex_lock_unlock_owned_and_errors() {
  amber::runtime::RuntimeMutex mutex;

  const amber::runtime::RuntimeMutexResult locked = mutex.lock();
  expect(locked.ok && locked.locked,
         "mutex lock should acquire unlocked mutex");
  expect(mutex.locked(), "locked? should report held mutex");
  expect(mutex.owned(), "owned? should report current owner");

  std::string non_owner_error;
  std::thread other_thread([&mutex, &non_owner_error]() {
    const amber::runtime::RuntimeMutexResult result = mutex.unlock();
    non_owner_error = result.error_name;
  });
  other_thread.join();
  expect(non_owner_error == "OwnershipError",
         "mutex unlock by non-owner should report OwnershipError");
  expect(mutex.locked() && mutex.owned(),
         "failed non-owner unlock should keep mutex held by owner");

  const amber::runtime::RuntimeMutexResult reentrant =
      mutex.lock(std::chrono::milliseconds(0));
  expect(!reentrant.ok && reentrant.error_name == "DeadlockError",
         "same owner double lock should report DeadlockError");

  const amber::runtime::RuntimeMutexResult unlocked = mutex.unlock();
  expect(unlocked.ok && unlocked.unlocked, "mutex unlock should release owner");
  expect(!mutex.locked() && !mutex.owned(),
         "released mutex should not be locked or owned");

  const amber::runtime::RuntimeMutexResult unlocked_again = mutex.unlock();
  expect(!unlocked_again.ok && unlocked_again.error_name == "OwnershipError",
         "unlocking an unlocked mutex should report OwnershipError");
}

void test_std013_mutex_waiter_fifo() {
  amber::runtime::RuntimeTaskModule task(4);
  amber::runtime::RuntimeMutex mutex;
  std::mutex order_mutex;
  std::vector<std::int64_t> acquisition_order;
  std::vector<amber::runtime::RuntimeTaskHandle> waiters;

  expect(mutex.lock().ok, "main task should hold mutex before waiter setup");

  for (int value = 1; value <= 3; ++value) {
    waiters.push_back(task.spawn([&mutex, &order_mutex, &acquisition_order,
                                  value]() {
      const amber::runtime::RuntimeMutexResult locked =
          mutex.lock(std::chrono::milliseconds(1000));
      expect(locked.ok && locked.locked, "waiting locker should acquire mutex");
      expect(mutex.owned(), "waiting locker should become mutex owner");
      {
        std::lock_guard<std::mutex> lock(order_mutex);
        acquisition_order.push_back(value);
      }
      const amber::runtime::RuntimeMutexResult unlocked = mutex.unlock();
      expect(unlocked.ok && unlocked.unlocked,
             "waiting locker should release mutex");
      return amber::runtime::Value::integer(value);
    }));
    expect(wait_for_condition(
               [&mutex, value]() {
                 return mutex.stats().waiting_lockers ==
                        static_cast<std::uint64_t>(value);
               },
               std::chrono::milliseconds(1000)),
           "mutex waiter should enter FIFO queue");
  }

  expect(mutex.unlock().ok, "main task should release mutex to waiters");
  for (const amber::runtime::RuntimeTaskHandle &waiter : waiters) {
    const amber::runtime::RuntimeTaskPublicResult joined =
        waiter.wait(std::chrono::milliseconds(1000));
    expect(joined.ok && joined.ready, "mutex waiter task should join");
  }

  {
    std::lock_guard<std::mutex> lock(order_mutex);
    expect(acquisition_order.size() == 3 && acquisition_order[0] == 1 &&
               acquisition_order[1] == 2 && acquisition_order[2] == 3,
           "mutex waiters should acquire lock FIFO");
  }

  const amber::runtime::RuntimeMutexStats stats = mutex.stats();
  expect(stats.locks == 4 && stats.unlocks == 4 && stats.contentions == 3,
         "mutex stats should count owner lock and FIFO waiter locks");
}

void test_std013_mutex_synchronize_return_and_unwind() {
  amber::runtime::RuntimeMutex mutex;

  const amber::runtime::RuntimeMutexResult synchronized =
      mutex.synchronize([&mutex]() {
        expect(mutex.locked() && mutex.owned(),
               "synchronize block should run while mutex is owned");
        return amber::runtime::Value::integer(314);
      });
  expect(synchronized.ok && synchronized.locked && synchronized.unlocked,
         "synchronize should acquire and release mutex");
  expect_integer(synchronized.value, 314, "synchronize return value");
  expect(!mutex.locked(), "synchronize should leave mutex unlocked");

  bool caught = false;
  try {
    (void)mutex.synchronize([&mutex]() {
      expect(mutex.locked() && mutex.owned(),
             "throwing synchronize block should run while mutex is owned");
      throw std::runtime_error("boom");
      return amber::runtime::Value::null();
    });
  } catch (const std::runtime_error &) {
    caught = true;
  }

  expect(caught, "synchronize should propagate block exception");
  expect(!mutex.locked(), "synchronize should unlock during exception unwind");
  expect(mutex.lock().ok,
         "mutex should be reusable after synchronize exception unwind");
  expect(mutex.unlock().ok,
         "reused mutex should unlock after synchronize exception unwind");
}

void test_std013_mutex_owner_ids_are_unique_across_schedulers() {
  amber::runtime::RuntimeTaskModule first_scheduler(1);
  amber::runtime::RuntimeTaskModule second_scheduler(1);
  amber::runtime::RuntimeMutex mutex;
  std::atomic<bool> first_locked{false};
  std::atomic<bool> second_synchronized{false};

  const amber::runtime::RuntimeTaskHandle first =
      first_scheduler.spawn([&]() {
        const amber::runtime::RuntimeMutexResult synchronized =
            mutex.synchronize(
                [&]() {
                  first_locked.store(true, std::memory_order_release);
                  std::this_thread::sleep_for(std::chrono::milliseconds(30));
                  return amber::runtime::Value::null();
                },
                std::chrono::milliseconds(1000));
        expect(synchronized.ok,
               "first scheduler should release shared mutex");
        return amber::runtime::Value::null();
      });

  expect(wait_for_condition(
             [&]() { return first_locked.load(std::memory_order_acquire); },
             std::chrono::milliseconds(1000)),
         "first scheduler should acquire shared mutex");

  const amber::runtime::RuntimeTaskHandle second =
      second_scheduler.spawn([&]() {
        const amber::runtime::RuntimeMutexResult synchronized =
            mutex.synchronize(
                []() { return amber::runtime::Value::null(); },
                std::chrono::milliseconds(1000));
        second_synchronized.store(synchronized.ok,
                                  std::memory_order_release);
        return amber::runtime::Value::null();
      });

  expect(first.wait(std::chrono::milliseconds(1000)).ok,
         "first scheduler task should complete");
  expect(second.wait(std::chrono::milliseconds(1000)).ok,
         "second scheduler task should complete");
  expect(second_synchronized.load(std::memory_order_acquire),
         "mutex ownership must not collide across schedulers");
}

void test_std013_mutex_lock_wait_cancellation() {
  amber::runtime::RuntimeTaskModule task(2);
  amber::runtime::RuntimeMutex mutex;
  std::atomic<bool> waiter_entered{false};

  expect(mutex.lock().ok, "main task should hold mutex before cancellation");
  const amber::runtime::RuntimeTaskHandle waiter =
      task.spawn([&mutex, &waiter_entered]() {
        waiter_entered = true;
        const amber::runtime::RuntimeMutexResult locked =
            mutex.lock(std::chrono::milliseconds(1000));
        if (locked.cancelled && locked.error_name == "CancelledError") {
          return amber::runtime::Value::integer(1);
        }
        if (locked.timed_out) {
          return amber::runtime::Value::integer(2);
        }
        if (locked.ok) {
          (void)mutex.unlock();
          return amber::runtime::Value::integer(3);
        }
        return amber::runtime::Value::integer(4);
      });

  expect(wait_for_condition(
             [&waiter_entered, &mutex]() {
               return waiter_entered.load() &&
                      mutex.stats().waiting_lockers == 1;
             },
             std::chrono::milliseconds(1000)),
         "cancellable mutex locker should enter wait queue");
  expect(waiter.cancel(),
         "cancelling mutex locker task should request cancellation");

  const amber::runtime::RuntimeTaskPublicResult cancelled =
      waiter.wait(std::chrono::milliseconds(1000));
  expect(cancelled.ok && cancelled.ready,
         "mutex cancellation observer should finish task");
  expect_integer(cancelled.value, 1,
                 "mutex lock should report CancelledError before timeout");
  expect(mutex.stats().lock_cancellations == 1 &&
             mutex.stats().waiting_lockers == 0,
         "mutex stats should count cancellation and remove waiter");
  expect(mutex.unlock().ok,
         "main task should release mutex after cancellation test");
}

void test_std014_atomic_get_set_compare_and_set_and_guard() {
  amber::runtime::RuntimeAtomic atomic(0);
  expect(atomic.get() == 0, "atomic get should read initial integer");
  atomic.set(5);
  expect(atomic.get() == 5, "atomic set should publish integer value");
  expect(atomic.compare_and_set(5, 6),
         "atomic compare_and_set should update matching integer");
  expect(!atomic.compare_and_set(5, 7),
         "atomic compare_and_set should reject stale integer expected value");
  expect(atomic.get() == 6, "failed integer CAS should leave value unchanged");

  const amber::runtime::RuntimeAtomic::Result bool_set =
      atomic.set_value(amber::runtime::Value::boolean(true));
  expect(bool_set.ok && bool_set.updated && bool_set.value.as_bool(),
         "atomic set_value should accept bool payload");
  expect(atomic.get_value().as_bool(),
         "atomic get_value should return latest bool payload");

  amber::runtime::RuntimeHeap heap;
  const amber::runtime::Value frozen_tuple =
      heap.make_tuple_value({amber::runtime::Value::integer(7)});
  const amber::runtime::RuntimeAtomic::Result tuple_set =
      atomic.set_value(frozen_tuple);
  expect(tuple_set.ok && tuple_set.updated,
         "atomic set_value should accept shareable heap payload");

  const amber::runtime::Value equal_but_distinct_tuple =
      heap.make_tuple_value({amber::runtime::Value::integer(7)});
  const amber::runtime::RuntimeAtomic::Result identity_miss =
      atomic.compare_and_set_value(equal_but_distinct_tuple,
                                   amber::runtime::Value::integer(8));
  expect(identity_miss.ok && !identity_miss.matched,
         "atomic CAS should compare heap payloads by identity");
  expect(atomic.get_value().as_tuple() == frozen_tuple.as_tuple(),
         "failed identity CAS should leave heap value unchanged");

  const amber::runtime::RuntimeAtomic::Result identity_hit =
      atomic.compare_and_set_value(frozen_tuple,
                                   amber::runtime::Value::integer(9));
  expect(identity_hit.ok && identity_hit.matched && identity_hit.updated,
         "atomic CAS should update matching heap identity");
  expect(atomic.get() == 9, "matching heap CAS should publish replacement");

  const amber::runtime::Value confined = heap.make_list_value({});
  bool constructor_rejected = false;
  try {
    amber::runtime::RuntimeAtomic rejected_atomic(confined);
    (void)rejected_atomic;
  } catch (const amber::runtime::RuntimeTaskFailure &failure) {
    constructor_rejected = failure.error_name() == "AtomicCompatibilityError";
  }
  expect(constructor_rejected,
         "atomic constructor should reject incompatible initial payload");

  const amber::runtime::RuntimeAtomic::Result rejected_set =
      atomic.set_value(confined);
  expect(!rejected_set.ok &&
             rejected_set.error_name == "AtomicCompatibilityError",
         "atomic set_value should reject confined mutable payload");
  expect(atomic.get() == 9,
         "rejected atomic set_value should leave value unchanged");

  const amber::runtime::RuntimeAtomic::Result rejected_cas =
      atomic.compare_and_set_value(amber::runtime::Value::integer(9), confined);
  expect(!rejected_cas.ok &&
             rejected_cas.error_name == "AtomicCompatibilityError",
         "atomic CAS should reject incompatible replacement payload");
  expect(atomic.get() == 9, "rejected atomic CAS should leave value unchanged");
}

void test_std014_atomic_update_value_retry_and_guard() {
  amber::runtime::RuntimeAtomic atomic(1);
  const amber::runtime::RuntimeAtomic::Result incremented =
      atomic.update([](const amber::runtime::Value &current) {
        expect(current.is_integer(), "atomic update should read integer");
        return amber::runtime::Value::integer(current.as_integer() + 1);
      });
  expect(incremented.ok && incremented.matched && incremented.updated,
         "atomic update should publish replacement");
  expect_integer(incremented.value, 2, "atomic update return value");
  expect(incremented.attempts == 1,
         "uncontended atomic update should run block once");
  expect(atomic.get() == 2, "atomic update should store return value");

  amber::runtime::RuntimeAtomic retried(0);
  int calls = 0;
  const amber::runtime::RuntimeAtomic::Result retry_result =
      retried.update([&retried, &calls](const amber::runtime::Value &current) {
        ++calls;
        expect(current.is_integer(), "retrying atomic update should read int");
        if (calls == 1) {
          expect(retried.compare_and_set(current.as_integer(),
                                         current.as_integer() + 1),
                 "test CAS should create an update retry");
        }
        return amber::runtime::Value::integer(current.as_integer() + 1);
      });
  expect(retry_result.ok && retry_result.matched && retry_result.updated,
         "atomic update should eventually publish after retry");
  expect(retry_result.attempts == 2 && calls == 2,
         "atomic update should re-run block after failed CAS");
  expect_integer(retry_result.value, 2, "retrying atomic update return value");
  expect(retried.get() == 2, "retrying atomic update final value");

  amber::runtime::RuntimeHeap heap;
  const amber::runtime::Value confined = heap.make_list_value({});
  const amber::runtime::RuntimeAtomic::Result rejected = retried.update(
      [&confined](const amber::runtime::Value &) { return confined; });
  expect(!rejected.ok && rejected.attempts == 1 &&
             rejected.error_name == "AtomicCompatibilityError",
         "atomic update should reject incompatible replacement");
  expect(retried.get() == 2,
         "rejected atomic update should leave value unchanged");
}

void test_std014_atomic_cross_strand_counter() {
  amber::runtime::RuntimeTaskModule task(4);
  amber::runtime::RuntimeAtomic counter(0);
  std::vector<amber::runtime::RuntimeTaskHandle> workers;
  constexpr int kWorkers = 4;
  constexpr int kIterations = 250;

  for (int worker = 0; worker < kWorkers; ++worker) {
    workers.push_back(task.spawn([&counter]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const amber::runtime::RuntimeAtomic::Result incremented =
            counter.update([](const amber::runtime::Value &current) {
              expect(current.is_integer(),
                     "cross-strand atomic update should read integer");
              return amber::runtime::Value::integer(current.as_integer() + 1);
            });
        expect(incremented.ok && incremented.matched,
               "cross-strand atomic update should succeed");
      }
      return amber::runtime::Value::integer(1);
    }));
  }

  for (const amber::runtime::RuntimeTaskHandle &worker : workers) {
    const amber::runtime::RuntimeTaskPublicResult joined =
        worker.wait(std::chrono::milliseconds(3000));
    expect(joined.ok && joined.ready,
           "atomic counter worker should finish successfully");
  }

  expect(counter.get() == kWorkers * kIterations,
         "atomic update should preserve all cross-strand increments");
}

void test_std015_barrier_release_timeout_and_cancellation() {
  amber::runtime::RuntimeTaskModule task(4);
  amber::runtime::RuntimeBarrier barrier(3);
  std::atomic<int> arrived{0};
  std::vector<amber::runtime::RuntimeTaskHandle> waiters;

  for (int worker = 0; worker < 3; ++worker) {
    waiters.push_back(task.spawn([&barrier, &arrived]() {
      arrived.fetch_add(1);
      const amber::runtime::RuntimeBarrierResult passed =
          barrier.wait(std::chrono::milliseconds(1000));
      expect(passed.ok && passed.passed,
             "barrier worker should pass released generation");
      return amber::runtime::Value::integer(passed.last ? 1 : 0);
    }));
  }

  for (const amber::runtime::RuntimeTaskHandle &waiter : waiters) {
    const amber::runtime::RuntimeTaskPublicResult joined =
        waiter.wait(std::chrono::milliseconds(1000));
    expect(joined.ok && joined.ready, "barrier waiter should join");
  }
  expect(arrived.load() == 3, "all barrier workers should arrive");

  const amber::runtime::RuntimeBarrierStats released_stats = barrier.stats();
  expect(released_stats.arrivals == 3 && released_stats.passes == 1 &&
             released_stats.waiting == 0 && released_stats.generation == 1,
         "barrier stats should count released generation");

  amber::runtime::RuntimeBarrier timeout_barrier(2);
  const amber::runtime::RuntimeBarrierResult timed_out =
      timeout_barrier.wait(std::chrono::milliseconds(5));
  expect(!timed_out.ok && timed_out.timed_out &&
             timed_out.error_name == "TimeoutError",
         "barrier wait should support timeout");
  expect(timeout_barrier.stats().timeouts == 1 &&
             timeout_barrier.stats().waiting == 0,
         "barrier timeout should remove waiter");

  amber::runtime::RuntimeBarrier cancellation_barrier(2);
  amber::runtime::RuntimeTaskModule cancel_task(2);
  std::atomic<bool> waiter_entered{false};
  const amber::runtime::RuntimeTaskHandle cancellable =
      cancel_task.spawn([&cancellation_barrier, &waiter_entered]() {
        waiter_entered = true;
        const amber::runtime::RuntimeBarrierResult cancelled =
            cancellation_barrier.wait(std::chrono::milliseconds(1000));
        if (cancelled.cancelled && cancelled.error_name == "CancelledError") {
          return amber::runtime::Value::integer(1);
        }
        if (cancelled.timed_out) {
          return amber::runtime::Value::integer(2);
        }
        return amber::runtime::Value::integer(3);
      });

  expect(wait_for_condition(
             [&cancellation_barrier, &waiter_entered]() {
               return waiter_entered.load() &&
                      cancellation_barrier.stats().waiting == 1;
             },
             std::chrono::milliseconds(1000)),
         "cancellable barrier waiter should enter wait set");
  expect(cancellable.cancel(),
         "cancelling barrier waiter should request cancellation");
  const amber::runtime::RuntimeTaskPublicResult cancelled =
      cancellable.wait(std::chrono::milliseconds(1000));
  expect(cancelled.ok && cancelled.ready,
         "barrier cancellation observer should finish task");
  expect_integer(cancelled.value, 1,
                 "barrier wait should report CancelledError before timeout");
  expect(cancellation_barrier.stats().cancellations == 1 &&
             cancellation_barrier.stats().waiting == 0,
         "barrier stats should count cancellation and remove waiter");
}

void test_std015_flow_gather_and_scatter_map_ordered_results() {
  amber::runtime::RuntimeTaskModule task(3);
  std::vector<amber::runtime::RuntimeTaskHandle> handles;
  handles.push_back(task.spawn([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return amber::runtime::Value::integer(1);
  }));
  handles.push_back(
      task.spawn([]() { return amber::runtime::Value::integer(2); }));
  handles.push_back(task.spawn([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return amber::runtime::Value::integer(3);
  }));

  amber::runtime::RuntimeFlowModule flow(4);
  const amber::runtime::RuntimeFlowGatherResult gathered =
      flow.gather(std::move(handles));
  expect(gathered.ok && !gathered.failed && gathered.values.size() == 3,
         "flow gather should return ordered successful values");
  expect_integer(gathered.values[0], 1, "flow gather first value");
  expect_integer(gathered.values[1], 2, "flow gather second value");
  expect_integer(gathered.values[2], 3, "flow gather third value");

  std::vector<amber::runtime::Value> items = {
      amber::runtime::Value::integer(3), amber::runtime::Value::integer(1),
      amber::runtime::Value::integer(2)};
  const amber::runtime::RuntimeFlowGatherResult mapped = flow.scatter_map(
      items, [](const amber::runtime::Value &value, std::size_t index) {
        return amber::runtime::Value::integer(value.as_integer() * 10 +
                                              static_cast<std::int64_t>(index));
      });
  expect(mapped.ok && !mapped.failed && mapped.values.size() == 3,
         "flow scatter_map should complete successfully");
  expect_integer(mapped.values[0], 30, "flow scatter_map first value");
  expect_integer(mapped.values[1], 11, "flow scatter_map second value");
  expect_integer(mapped.values[2], 22, "flow scatter_map third value");

  const amber::runtime::RuntimeFlowStats stats = flow.stats();
  expect(stats.gathers >= 2 && stats.flows >= 1 && stats.completed_workers >= 6,
         "flow stats should count gather and scatter workers");
}

void test_std015_flow_reduce_broadcast_and_failure_collection() {
  amber::runtime::RuntimeFlowModule flow(4);
  std::vector<amber::runtime::Value> items = {
      amber::runtime::Value::integer(1), amber::runtime::Value::integer(2),
      amber::runtime::Value::integer(3), amber::runtime::Value::integer(4)};

  const amber::runtime::RuntimeFlowReduceResult reduced = flow.scatter_reduce(
      items, amber::runtime::Value::integer(0),
      [](const amber::runtime::Value &value, std::size_t) {
        return amber::runtime::Value::integer(value.as_integer() *
                                              value.as_integer());
      },
      [](const amber::runtime::Value &acc, const amber::runtime::Value &value) {
        return amber::runtime::Value::integer(acc.as_integer() +
                                              value.as_integer());
      });
  expect(reduced.ok, "flow scatter_reduce should complete successfully");
  expect_integer(reduced.value, 30, "flow scatter_reduce sum of squares");

  const amber::runtime::RuntimeFlowGatherResult broadcast = flow.broadcast(
      amber::runtime::Value::integer(5), 3,
      [](const amber::runtime::Value &value, std::size_t worker) {
        return amber::runtime::Value::integer(
            value.as_integer() + static_cast<std::int64_t>(worker));
      });
  expect(broadcast.ok && broadcast.values.size() == 3,
         "flow broadcast should gather all worker values");
  expect_integer(broadcast.values[0], 5, "flow broadcast first value");
  expect_integer(broadcast.values[1], 6, "flow broadcast second value");
  expect_integer(broadcast.values[2], 7, "flow broadcast third value");

  amber::runtime::RuntimeFlowOptions collect_failures;
  collect_failures.failure_policy =
      amber::runtime::RuntimeFlowFailurePolicy::Collect;
  const amber::runtime::RuntimeFlowGatherResult collected = flow.scatter_map(
      items,
      [](const amber::runtime::Value &value, std::size_t) {
        if (value.as_integer() == 2) {
          throw amber::runtime::RuntimeTaskFailure("FlowPartitionError",
                                                   "bad partition");
        }
        return amber::runtime::Value::integer(value.as_integer() * 10);
      },
      collect_failures);
  expect(collected.ok && collected.failed && collected.failures.size() == 1,
         "flow collect failure policy should return failures");
  expect(collected.failures[0].index == 1 &&
             collected.failures[0].error_name == "FlowPartitionError",
         "flow collect failure should preserve worker index and error");
  expect_integer(collected.values[0], 10, "flow collect failure first success");
  expect_integer(collected.values[2], 30, "flow collect failure later success");

  const amber::runtime::RuntimeFlowStats stats = flow.stats();
  expect(stats.reductions == 1 && stats.broadcasts == 1 &&
             stats.failed_workers >= 1,
         "flow stats should count reduce, broadcast, and failed workers");
}

void test_std015_flow_isolation_checked_and_unchecked() {
  amber::runtime::RuntimeFlowModule flow(2);
  amber::runtime::RuntimeHeap heap;
  const amber::runtime::Value confined = heap.make_list_value({});

  const amber::runtime::RuntimeFlowGatherResult rejected_partition =
      flow.scatter_map({confined},
                       [](const amber::runtime::Value &, std::size_t) {
                         return amber::runtime::Value::integer(0);
                       });
  expect(!rejected_partition.ok &&
             rejected_partition.error_name == "IsolationError",
         "checked flow should reject non-shareable partitions");

  amber::runtime::RuntimeFlowOptions unchecked;
  unchecked.isolation = amber::runtime::RuntimeFlowIsolationMode::Unchecked;
  const amber::runtime::RuntimeFlowGatherResult unchecked_result =
      flow.scatter_map(
          {confined},
          [](const amber::runtime::Value &value, std::size_t) {
            return amber::runtime::Value::integer(value.is_list() ? 1 : 0);
          },
          unchecked);
  expect(unchecked_result.ok && unchecked_result.values.size() == 1,
         "unchecked flow should accept confined partition");
  expect_integer(unchecked_result.values[0], 1,
                 "unchecked flow confined partition result");

  const amber::runtime::RuntimeFlowGatherResult rejected_result =
      flow.scatter_map({amber::runtime::Value::integer(1)},
                       [](const amber::runtime::Value &, std::size_t) {
                         return amber::runtime::make_list_value({});
                       });
  expect(!rejected_result.ok && rejected_result.failed &&
             rejected_result.error_name == "IsolationError",
         "checked flow should reject non-shareable worker results");
  expect(flow.stats().isolation_rejections >= 2,
         "flow stats should count partition and result isolation failures");
}

void test_std016_threaded_collection_iteration_and_transforms() {
  std::vector<amber::runtime::Value> items = {
      amber::runtime::Value::integer(1), amber::runtime::Value::integer(2),
      amber::runtime::Value::integer(3), amber::runtime::Value::integer(4)};
  amber::runtime::RuntimeThreadedCollection threaded(items, 2);

  const amber::runtime::RuntimeFlowGatherResult mapped =
      threaded.map([](const amber::runtime::Value &value, std::size_t index) {
        return amber::runtime::Value::integer(value.as_integer() * 10 +
                                              static_cast<std::int64_t>(index));
      });
  expect(mapped.ok && !mapped.failed,
         "threaded map should complete successfully");
  expect_integer_values(mapped.values, {10, 21, 32, 43},
                        "threaded map ordered results");

  const amber::runtime::RuntimeFlowGatherResult filter_mapped =
      threaded.filter_map([](const amber::runtime::Value &value, std::size_t) {
        if (value.as_integer() % 2 != 0) {
          return amber::runtime::Value::null();
        }
        return amber::runtime::Value::integer(value.as_integer() * 10);
      });
  expect(filter_mapped.ok && !filter_mapped.failed,
         "threaded filter_map should complete successfully");
  expect_integer_values(filter_mapped.values, {20, 40},
                        "threaded filter_map ordered results");

  const amber::runtime::RuntimeFlowGatherResult selected =
      threaded.select([](const amber::runtime::Value &value, std::size_t) {
        return value.as_integer() % 2 == 1;
      });
  expect(selected.ok && !selected.failed,
         "threaded select should complete successfully");
  expect_integer_values(selected.values, {1, 3},
                        "threaded select ordered results");

  const amber::runtime::RuntimeFlowGatherResult rejected =
      threaded.reject([](const amber::runtime::Value &value, std::size_t) {
        return value.as_integer() % 2 == 1;
      });
  expect(rejected.ok && !rejected.failed,
         "threaded reject should complete successfully");
  expect_integer_values(rejected.values, {2, 4},
                        "threaded reject ordered results");

  const amber::runtime::RuntimeFlowGatherResult flat_mapped =
      threaded.flat_map([](const amber::runtime::Value &value, std::size_t) {
        return std::vector<amber::runtime::Value>{
            value, amber::runtime::Value::integer(value.as_integer() + 10)};
      });
  expect(flat_mapped.ok && !flat_mapped.failed,
         "threaded flat_map should complete successfully");
  expect_integer_values(flat_mapped.values, {1, 11, 2, 12, 3, 13, 4, 14},
                        "threaded flat_map ordered results");

  amber::runtime::RuntimeAtomic sum(0);
  const amber::runtime::RuntimeFlowGatherResult each =
      threaded.each([&sum](const amber::runtime::Value &value, std::size_t) {
        const amber::runtime::RuntimeAtomic::Result updated =
            sum.update([&value](const amber::runtime::Value &current) {
              return amber::runtime::Value::integer(current.as_integer() +
                                                    value.as_integer());
            });
        expect(updated.ok && updated.updated,
               "threaded each atomic update should succeed");
      });
  expect(each.ok && !each.failed, "threaded each should complete successfully");
  expect(sum.get() == 10, "threaded each should visit every item once");

  const amber::runtime::RuntimeThreadedCollectionStats stats = threaded.stats();
  expect(stats.operations == 6 && stats.map_operations == 1 &&
             stats.filter_map_operations == 1 && stats.filter_operations == 2 &&
             stats.flat_map_operations == 1 && stats.each_operations == 1,
         "threaded collection stats should count iteration operations");
  expect(stats.flow.gathers >= 6 && stats.flow.completed_workers >= 12,
         "threaded collection stats should include flow stats");
}

void test_std016_threaded_collection_scatter_policies_bound_task_count() {
  std::vector<amber::runtime::Value> items;
  for (std::int64_t value = 1; value <= 8; ++value) {
    items.push_back(amber::runtime::Value::integer(value));
  }

  auto run_ids =
      [&items](amber::runtime::RuntimeFlowPartitionPolicy scatter_policy) {
        amber::runtime::RuntimeThreadedCollection threaded(
            items, 4, amber::runtime::RuntimeFlowOptions{}, scatter_policy);
        std::mutex mutex;
        std::unordered_set<std::uint64_t> task_ids;
        const amber::runtime::RuntimeFlowGatherResult mapped =
            threaded.map([&mutex, &task_ids](const amber::runtime::Value &value,
                                             std::size_t) {
              std::lock_guard<std::mutex> lock(mutex);
              task_ids.insert(amber::runtime::current_runtime_task_id());
              return value;
            });
        expect(mapped.ok && !mapped.failed && mapped.values.size() == 8,
               "threaded scatter policy map should complete");
        return std::pair<amber::runtime::RuntimeThreadedCollectionStats,
                         std::unordered_set<std::uint64_t>>{
            threaded.stats(), std::move(task_ids)};
      };

  const auto atomic_run =
      run_ids(amber::runtime::RuntimeFlowPartitionPolicy::Atomic);
  expect(atomic_run.first.flow.worker_tasks == 4 &&
             atomic_run.first.flow.completed_workers == 4,
         "atomic threaded scatter should spawn one task per worker");
  expect(!atomic_run.second.empty() && atomic_run.second.size() <= 4,
         "atomic threaded scatter should use at most worker-count task ids");

  const auto chunks_run =
      run_ids(amber::runtime::RuntimeFlowPartitionPolicy::Chunks);
  expect(chunks_run.first.flow.worker_tasks == 4 &&
             chunks_run.first.flow.completed_workers == 4,
         "chunked threaded scatter should spawn one task per worker");
  expect(chunks_run.second.size() == 4,
         "chunked threaded scatter should process four non-empty chunks");

  const auto items_run =
      run_ids(amber::runtime::RuntimeFlowPartitionPolicy::Items);
  expect(items_run.first.flow.worker_tasks == 8 &&
             items_run.first.flow.completed_workers == 8,
         "item threaded scatter should preserve per-item task mode");
  expect(items_run.second.size() == 8,
         "item threaded scatter should expose one task id per item");
}

void test_std016_threaded_collection_combination_and_permutation() {
  std::vector<amber::runtime::Value> items = {
      amber::runtime::Value::integer(1), amber::runtime::Value::integer(2),
      amber::runtime::Value::integer(3)};
  amber::runtime::RuntimeThreadedCollection threaded(items, 3);

  const amber::runtime::RuntimeFlowGatherResult combinations =
      threaded.combination(2);
  expect(combinations.ok && !combinations.failed,
         "threaded combination should complete successfully");
  expect_integer_list_values(combinations.values, {{1, 2}, {1, 3}, {2, 3}},
                             "threaded combination ordered results");

  const amber::runtime::RuntimeFlowGatherResult permutations =
      threaded.permutation(2);
  expect(permutations.ok && !permutations.failed,
         "threaded permutation should complete successfully");
  expect_integer_list_values(permutations.values,
                             {{1, 2}, {1, 3}, {2, 1}, {2, 3}, {3, 1}, {3, 2}},
                             "threaded permutation ordered results");

  const amber::runtime::RuntimeFlowGatherResult too_large =
      threaded.combination(4);
  expect(too_large.ok && too_large.values.empty(),
         "threaded combination should return empty result for too-large count");

  const amber::runtime::RuntimeThreadedCollectionStats stats = threaded.stats();
  expect(stats.combination_operations == 2 &&
             stats.permutation_operations == 1 && stats.generated_values == 9,
         "threaded collection stats should count generated rows");
}

void test_std016_threaded_collection_failure_and_isolation() {
  amber::runtime::RuntimeHeap heap;
  const amber::runtime::Value confined = heap.make_list_value({});

  amber::runtime::RuntimeThreadedCollection checked({confined}, 2);
  const amber::runtime::RuntimeFlowGatherResult rejected_partition =
      checked.map([](const amber::runtime::Value &, std::size_t) {
        return amber::runtime::Value::integer(0);
      });
  expect(!rejected_partition.ok &&
             rejected_partition.error_name == "IsolationError",
         "threaded collection should reject confined checked input");
  const amber::runtime::RuntimeFlowGatherResult rejected_generated =
      checked.permutation(1);
  expect(!rejected_generated.ok &&
             rejected_generated.error_name == "IsolationError",
         "threaded generated rows should reject confined checked source");

  amber::runtime::RuntimeFlowOptions unchecked_options;
  unchecked_options.isolation =
      amber::runtime::RuntimeFlowIsolationMode::Unchecked;
  amber::runtime::RuntimeThreadedCollection unchecked({confined}, 2,
                                                      unchecked_options);
  const amber::runtime::RuntimeFlowGatherResult unchecked_result =
      unchecked.map([](const amber::runtime::Value &value, std::size_t) {
        return amber::runtime::Value::integer(value.is_list() ? 1 : 0);
      });
  expect(unchecked_result.ok && unchecked_result.values.size() == 1,
         "threaded unchecked mode should accept confined input");
  expect_integer(unchecked_result.values[0], 1,
                 "threaded unchecked mode result");

  amber::runtime::RuntimeThreadedCollection result_guard(
      {amber::runtime::Value::integer(1)}, 2);
  const amber::runtime::RuntimeFlowGatherResult rejected_result =
      result_guard.map([&heap](const amber::runtime::Value &, std::size_t) {
        return heap.make_list_value({});
      });
  expect(!rejected_result.ok && rejected_result.failed &&
             rejected_result.error_name == "IsolationError",
         "threaded collection should reject confined checked result");

  amber::runtime::RuntimeFlowOptions collect_failures;
  collect_failures.failure_policy =
      amber::runtime::RuntimeFlowFailurePolicy::Collect;
  amber::runtime::RuntimeThreadedCollection failures(
      {amber::runtime::Value::integer(1), amber::runtime::Value::integer(2),
       amber::runtime::Value::integer(3)},
      2, collect_failures);
  const amber::runtime::RuntimeFlowGatherResult collected =
      failures.map([](const amber::runtime::Value &value, std::size_t) {
        if (value.as_integer() == 2) {
          throw amber::runtime::RuntimeTaskFailure("ThreadedError",
                                                   "bad threaded item");
        }
        return amber::runtime::Value::integer(value.as_integer() * 10);
      });
  expect(collected.ok && collected.failed && collected.failures.size() == 1,
         "threaded collection should support collect failure policy");
  expect(collected.failures[0].index == 1 &&
             collected.failures[0].error_name == "ThreadedError",
         "threaded collection failure should preserve item index and error");
  expect_integer(collected.values[0], 10,
                 "threaded collection collected first success");
  expect_integer(collected.values[2], 30,
                 "threaded collection collected later success");
}

void test_std017_source_level_task_sync_stack_compiles_and_runs() {
  const amber::runtime::ExecutionResult exec =
      execute_source_or_die("import task\n"
                            "from sync import Channel, Mutex, Atomic, Barrier\n"
                            "\n"
                            "ch = Channel.new(capacity: 2)\n"
                            "ch.send(10)\n"
                            "ch.send(20)\n"
                            "first = ch.recv()\n"
                            "second = ch.recv()\n"
                            "ch.close()\n"
                            "\n"
                            "m = Mutex.new()\n"
                            "guarded = m.synchronize: first + second\n"
                            "\n"
                            "a = Atomic.new(guarded)\n"
                            "a.update: _1 + 1\n"
                            "cas = a.compare_and_set(31, 40)\n"
                            "\n"
                            "barrier = Barrier.new(1)\n"
                            "passed = barrier.wait()\n"
                            "\n"
                            "h = task.spawn: a.get() + 1\n"
                            "\n"
                            "[h.wait(), cas, passed, ch.closed?()]\n");

  expect(exec.ok(), "source-level task/sync stack should execute");
  expect(exec.value.is_list(), "source-level task/sync result should be list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 4,
         "source-level task/sync result shape");
  expect_integer(values->items[0], 41, "source-level task.spawn/Atomic result");
  expect_bool(values->items[1], true,
              "source-level Atomic.compare_and_set result");
  expect_bool(values->items[2], true, "source-level Barrier.wait result");
  expect_bool(values->items[3], true, "source-level Channel.closed? result");
}

void test_std017_source_level_flow_and_threaded_collection_compile_and_run() {
  const amber::runtime::ExecutionResult exec = execute_source_or_die(
      "from task.flow import Flow, ThreadedCollection\n"
      "\n"
      "flowed = Flow.new().scatter_map([1, 2, 3]): _1 * 10 + _2\n"
      "threaded = [1, 2, 3].threaded(2).map: _1 + 5\n"
      "constructed = ThreadedCollection.new([1, 2, 3], 2, scatter: :items).map:"
      " _1 * 6\n"
      "filtered = [1, 2, 3, 4].threaded(2).filter_map |x|:\n"
      "  if x % 2 == 0:\n"
      "    x * 5\n"
      "  else:\n"
      "    false\n"
      "atomic = [1, 2, 3, 4].threaded(2, scatter: :atomic).map: _1 * 2\n"
      "chunks = [1, 2, 3, 4].threaded(2, scatter: :chunks).map: _1 * 3\n"
      "parallel = [1, 2, 3, 4].parallel(2, scatter: :chunks).map: _1 * 4\n"
      "pairs = [1, 2, 3].threaded(2).combination(2)\n"
      "[flowed, threaded, constructed, filtered, atomic, chunks, parallel, "
      "pairs]\n");

  expect(exec.ok(), "source-level flow/threaded stack should execute");
  expect(exec.value.is_list(), "source-level flow result should be list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 8,
         "source-level flow result shape");
  expect_integer_list_value(values->items[0], {10, 21, 32},
                            "source-level Flow.scatter_map");
  expect_integer_list_value(values->items[1], {6, 7, 8},
                            "source-level threaded map");
  expect_integer_list_value(values->items[2], {6, 12, 18},
                            "source-level ThreadedCollection.new map");
  expect_integer_list_value(values->items[3], {10, 20},
                            "source-level threaded filter_map");
  expect_integer_list_value(values->items[4], {2, 4, 6, 8},
                            "source-level threaded atomic scatter");
  expect_integer_list_value(values->items[5], {3, 6, 9, 12},
                            "source-level threaded chunk scatter");
  expect_integer_list_value(values->items[6], {4, 8, 12, 16},
                            "source-level parallel chunk scatter");
  expect_integer_list_values(values->items[7].as_list()->items,
                             {{1, 2}, {1, 3}, {2, 3}},
                             "source-level threaded combination");
}

void test_std018_task_local_basic_nested_exception_and_sleep() {
  const amber::runtime::ExecutionResult exec = execute_source_or_die(
      "import task\n"
      "local = task.local()\n"
      "before_bound = local.bound?()\n"
      "before_default = local.get(default: 90)\n"
      "assigned = local.set!(10)\n"
      "outer = local.with(20):\n"
      "  task.sleep(5)\n"
      "  inner = local.with(30): local.get()\n"
      "  [local.get(), inner]\n"
      "after_nested = local.get()\n"
      "rescued = try:\n"
      "  local.with(40):\n"
      "    1 / 0\n"
      "rescue:\n"
      "  local.get()\n"
      "cleared = local.clear!()\n"
      "[before_bound, before_default, assigned, outer, after_nested, rescued, "
      "cleared, local.bound?()]\n");

  expect(exec.ok(), "task-local basic/nested source should execute");
  expect(exec.value.is_list() && exec.value.as_list() != nullptr,
         "task-local basic result should be list");
  const std::vector<amber::runtime::Value> &items = exec.value.as_list()->items;
  expect(items.size() == 8, "task-local basic result shape");
  expect_bool(items[0], false, "task-local initially unbound");
  expect_integer(items[1], 90, "task-local default");
  expect_integer(items[2], 10, "task-local set return");
  expect_integer_list_value(items[3], {20, 30},
                            "task-local nested with values");
  expect_integer(items[4], 10, "task-local nested restoration");
  expect_integer(items[5], 10, "task-local exception restoration");
  expect_bool(items[6], true, "task-local clear return");
  expect_bool(items[7], false, "task-local clear removes binding");
}

void test_std018_task_local_spawn_inheritance_and_isolation() {
  const amber::runtime::ExecutionResult exec = execute_source_or_die(
      "import task\n"
      "private_local = task.local(inherit: false)\n"
      "request_local = task.local(inherit: true)\n"
      "private_local.set!(11)\n"
      "request_local.set!(22)\n"
      "child = task.spawn:\n"
      "  observed = [private_local.bound?(), request_local.get()]\n"
      "  request_local.set!(33)\n"
      "  [observed, request_local.get()]\n"
      "child_value = child.wait()\n"
      "[private_local.get(), request_local.get(), child_value]\n");

  expect(exec.ok(), "task-local inheritance source should execute");
  expect(exec.value.is_list() && exec.value.as_list() != nullptr,
         "task-local inheritance result should be list");
  const std::vector<amber::runtime::Value> &items = exec.value.as_list()->items;
  expect(items.size() == 3, "task-local inheritance result shape");
  expect_integer(items[0], 11, "non-inherited parent value remains");
  expect_integer(items[1], 22, "inherited parent value remains snapshot");
  expect(items[2].is_list() && items[2].as_list() != nullptr,
         "task-local child result shape");
  const std::vector<amber::runtime::Value> &child = items[2].as_list()->items;
  expect(child.size() == 2 && child[0].is_list() &&
             child[0].as_list() != nullptr,
         "task-local child observations shape");
  expect_bool(child[0].as_list()->items[0], false,
              "inherit false is absent in child");
  expect_integer(child[0].as_list()->items[1], 22,
                 "inherit true snapshots parent");
  expect_integer(child[1], 33, "child can replace its own binding");
}

void test_std018_task_local_distinct_tasks_and_parallel_stress() {
  amber::runtime::RuntimeTaskModule task(4);
  amber::runtime::RuntimeTaskLocal local(false);
  constexpr std::size_t count = 1024;
  constexpr std::size_t batch_size = 32;
  for (std::size_t base = 0; base < count; base += batch_size) {
    std::vector<amber::runtime::RuntimeTaskHandle> handles;
    handles.reserve(batch_size);
    for (std::size_t offset = 0; offset < batch_size; ++offset) {
      const std::size_t index = base + offset;
      handles.push_back(task.spawn([&task, &local, index]() {
        local.set(amber::runtime::Value::integer(
            static_cast<std::int64_t>(index)));
        task.yield_current();
        return local.get(amber::runtime::Value::integer(-1));
      }));
    }
    for (std::size_t offset = 0; offset < handles.size(); ++offset) {
      const amber::runtime::RuntimeTaskPublicResult result =
          handles[offset].wait(std::chrono::milliseconds(5000));
      expect(result.ok, "task-local parallel stress task should complete");
      expect_integer(result.value,
                     static_cast<std::int64_t>(base + offset),
                     "task-local parallel stress isolation");
    }
  }

  amber::runtime::RuntimeTaskModule single_worker(1);
  amber::runtime::RuntimeTaskLocal same_worker_local(false);
  const amber::runtime::RuntimeTaskHandle first =
      single_worker.spawn([&same_worker_local]() {
        same_worker_local.set(amber::runtime::Value::integer(101));
        return same_worker_local.get();
      });
  const amber::runtime::RuntimeTaskHandle second =
      single_worker.spawn([&same_worker_local]() {
        expect(!same_worker_local.bound(),
               "second task on same worker must start unbound");
        same_worker_local.set(amber::runtime::Value::integer(202));
        return same_worker_local.get();
      });
  expect_integer(first.wait().value, 101, "first same-worker task local");
  expect_integer(second.wait().value, 202, "second same-worker task local");
}

void test_std018_task_local_deterministic_worker_migration() {
  amber::runtime::RuntimeScheduler scheduler(
      amber::runtime::RuntimeSchedulerConfig{2, 700});
  amber::runtime::RuntimeTaskLocal local(false);
  std::atomic<int> phase{0};
  std::atomic<std::uint64_t> before_worker{0};
  std::atomic<std::uint64_t> after_worker{0};
  std::atomic<std::int64_t> observed{-1};

  const std::uint64_t strand_id = scheduler.spawn_strand(
      [&scheduler, &local, &phase, &before_worker, &after_worker, &observed]() {
        if (phase.load(std::memory_order_acquire) == 0) {
          local.set(amber::runtime::Value::integer(777));
          before_worker.store(amber::runtime::current_runtime_worker_id(),
                              std::memory_order_release);
          phase.store(1, std::memory_order_release);
          expect(scheduler.park_current(std::nullopt),
                 "migration probe should park its logical task");
          return;
        }
        after_worker.store(amber::runtime::current_runtime_worker_id(),
                           std::memory_order_release);
        observed.store(local.get().as_integer(), std::memory_order_release);
        phase.store(2, std::memory_order_release);
      });

  expect(wait_for_condition(
             [&scheduler, strand_id]() {
               const std::optional<amber::runtime::RuntimeStrandSnapshot>
                   snapshot = scheduler.strand_snapshot(strand_id);
               return snapshot.has_value() &&
                      snapshot->state ==
                          amber::runtime::RuntimeStrandState::Sleeping;
             },
             std::chrono::milliseconds(2000)),
         "migration probe should reach deterministic parked state");
  const std::uint64_t target_worker =
      before_worker.load(std::memory_order_acquire) == 700 ? 701 : 700;
  expect(scheduler.wake_strand_on_worker_for_test(strand_id, target_worker),
         "migration hook should target the other worker");
  const bool migration_idle =
      scheduler.wait_until_idle(std::chrono::milliseconds(2000));
  if (!migration_idle) {
    const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
    const std::optional<amber::runtime::RuntimeStrandSnapshot> snapshot =
        scheduler.strand_snapshot(strand_id);
    std::cerr << "migration debug: phase=" << phase.load()
              << " state="
              << (snapshot.has_value()
                      ? static_cast<int>(snapshot->state)
                      : -1)
              << " running/queue/sleeping=" << stats.worker_dequeues << "/"
              << stats.runnable_queue_depth << "/" << stats.sleeping_strands
              << " created/done/failed/cancelled=" << stats.strands_created
              << "/" << stats.strands_completed << "/"
              << stats.strands_failed << "/" << stats.tasks_cancelled << "\n";
  }
  expect(migration_idle,
         "migration probe should complete");
  expect(phase.load(std::memory_order_acquire) == 2,
         "migration probe should resume");
  expect(after_worker.load(std::memory_order_acquire) == target_worker &&
             after_worker.load(std::memory_order_acquire) !=
                 before_worker.load(std::memory_order_acquire),
         "migration probe must resume on a different native worker");
  expect(observed.load(std::memory_order_acquire) == 777,
         "task-local binding must survive forced worker migration");
}

void test_std018_task_local_lifecycle_cleanup_and_registry_reuse() {
  const std::uint64_t baseline_values =
      amber::runtime::runtime_task_local_retained_value_count();
  const std::uint64_t baseline_capacity =
      amber::runtime::runtime_task_local_registry_capacity();

  amber::runtime::RuntimeTaskLocal local(false);
  {
    const std::shared_ptr<amber::runtime::RuntimeTaskContext> context =
        amber::runtime::RuntimeTaskContext::create();
    amber::runtime::RuntimeTaskContextScope scope(context);
    auto first = std::make_shared<amber::runtime::RuntimeTaskLocal>();
    std::weak_ptr<amber::runtime::RuntimeTaskLocal> first_weak = first;
    local.set(amber::runtime::Value::task_local(first));
    first.reset();
    expect(!first_weak.expired(), "task-local set must retain value");

    auto second = std::make_shared<amber::runtime::RuntimeTaskLocal>();
    std::weak_ptr<amber::runtime::RuntimeTaskLocal> second_weak = second;
    local.set(amber::runtime::Value::task_local(second));
    second.reset();
    expect(first_weak.expired(), "task-local overwrite must release old value");
    expect(!second_weak.expired(), "task-local overwrite retains new value");
    expect(local.clear(), "task-local clear reports existing value");
    expect(second_weak.expired(), "task-local clear must release value");
  }

  amber::runtime::RuntimeTaskModule task(2);
  std::weak_ptr<amber::runtime::RuntimeTaskLocal> completed_weak;
  const amber::runtime::RuntimeTaskHandle completed =
      task.spawn([&local, &completed_weak]() {
        auto retained = std::make_shared<amber::runtime::RuntimeTaskLocal>();
        completed_weak = retained;
        local.set(amber::runtime::Value::task_local(std::move(retained)));
        return amber::runtime::Value::null();
      });
  expect(completed.wait(std::chrono::milliseconds(2000)).ok,
         "task-local lifecycle completion task should finish");
  expect(completed_weak.expired(),
         "normal task completion must release task-local values");

  std::atomic<bool> parked{false};
  std::weak_ptr<amber::runtime::RuntimeTaskLocal> cancelled_outer_weak;
  std::weak_ptr<amber::runtime::RuntimeTaskLocal> cancelled_scoped_weak;
  const amber::runtime::RuntimeTaskHandle cancelled =
      task.spawn([&task, &local, &parked, &cancelled_outer_weak,
                  &cancelled_scoped_weak]() {
        auto outer = std::make_shared<amber::runtime::RuntimeTaskLocal>();
        cancelled_outer_weak = outer;
        local.set(amber::runtime::Value::task_local(std::move(outer)));
        auto scoped = std::make_shared<amber::runtime::RuntimeTaskLocal>();
        cancelled_scoped_weak = scoped;
        (void)local.push_scope(
            amber::runtime::Value::task_local(std::move(scoped)));
        parked.store(true, std::memory_order_release);
        expect(task.scheduler().park_current(std::nullopt),
               "task-local cancellation probe should park");
        amber::runtime::runtime_mark_task_parked();
        return amber::runtime::Value::null();
      });
  expect(wait_for_condition([&parked]() { return parked.load(); },
                            std::chrono::milliseconds(2000)),
         "task-local cancellation probe should enter");
  expect(wait_for_condition(
             [&cancelled]() {
               return cancelled.state() ==
                      amber::runtime::RuntimeTaskHandleState::Sleeping;
             },
             std::chrono::milliseconds(2000)),
         "task-local cancellation probe should park");
  expect(cancelled.cancel(), "task-local cancellation should be requested");
  const amber::runtime::RuntimeTaskPublicResult cancelled_result =
      cancelled.wait(std::chrono::milliseconds(2000));
  expect(cancelled_result.cancelled,
         "task-local cancellation probe should report cancellation");
  expect(cancelled_outer_weak.expired(),
         "cancelled task must release shadowed task-local values");
  expect(cancelled_scoped_weak.expired(),
         "cancelled task must release active scoped task-local values");

  amber::runtime::RuntimeTaskLocal inherited_local(true);
  std::atomic<bool> inherited_parked{false};
  std::weak_ptr<amber::runtime::RuntimeTaskLocal> inherited_weak;
  amber::runtime::RuntimeTaskHandle inherited_child;
  {
    const std::shared_ptr<amber::runtime::RuntimeTaskContext> parent_context =
        amber::runtime::RuntimeTaskContext::create();
    amber::runtime::RuntimeTaskContextScope parent_scope(parent_context);
    auto retained = std::make_shared<amber::runtime::RuntimeTaskLocal>();
    inherited_weak = retained;
    inherited_local.set(
        amber::runtime::Value::task_local(std::move(retained)));
    inherited_child = task.spawn([&task, &inherited_local,
                                  &inherited_parked]() {
      expect(inherited_local.bound(),
             "inherit true child must retain the snapshotted binding");
      (void)inherited_local.get();
      inherited_parked.store(true, std::memory_order_release);
      expect(task.scheduler().park_current(std::nullopt),
             "inherited lifecycle child should park");
      amber::runtime::runtime_mark_task_parked();
      return amber::runtime::Value::null();
    });
    expect(inherited_local.clear(),
           "parent should clear its inherited lifecycle binding");
    expect(!inherited_weak.expired(),
           "child snapshot must independently retain inherited value");
  }
  expect(wait_for_condition([&inherited_parked]() {
           return inherited_parked.load(std::memory_order_acquire);
         }, std::chrono::milliseconds(2000)),
         "inherited lifecycle child should enter");
  expect(wait_for_condition(
             [&inherited_child]() {
               return inherited_child.state() ==
                      amber::runtime::RuntimeTaskHandleState::Sleeping;
             },
             std::chrono::milliseconds(2000)),
         "inherited lifecycle child should park");
  expect(inherited_child.cancel(),
         "inherited lifecycle child cancellation should be requested");
  expect(inherited_child.wait(std::chrono::milliseconds(2000)).cancelled,
         "inherited lifecycle child should cancel");
  expect(inherited_weak.expired(),
         "child cancellation must release inherited snapshot value");

  for (int wave = 0; wave < 250; ++wave) {
    std::vector<amber::runtime::RuntimeTaskHandle> short_tasks;
    short_tasks.reserve(4);
    for (int index = 0; index < 4; ++index) {
      short_tasks.push_back(task.spawn([&local, index]() {
        local.set(amber::runtime::Value::integer(index));
        return amber::runtime::Value::null();
      }));
    }
    for (const amber::runtime::RuntimeTaskHandle &handle : short_tasks) {
      expect(handle.wait(std::chrono::milliseconds(2000)).ok,
             "short task-local task should finish");
    }
  }
  expect(amber::runtime::runtime_task_local_retained_value_count() ==
             baseline_values,
         "short tasks must not leave retained task-local values");
  expect(amber::runtime::runtime_task_local_registry_capacity() <=
             baseline_capacity + 8,
         "short tasks must reuse bounded task-context registry slots");
}

} // namespace

// Layer B: a running strand can park itself, releasing its worker to run other
// strands, and resume later when woken. Eight strands park on an external wake
// with only two workers: if parking frees the worker, all eight reach the
// parked state; if it blocked the worker, `parks` would stall below eight and
// the wait would time out.
void test_layerb_scheduler_park_resume_frees_worker() {
  amber::runtime::RuntimeScheduler scheduler(2);
  const int strand_count = 8;
  std::atomic<int> entered{0};
  std::atomic<int> resumed{0};
  std::vector<std::uint64_t> ids;
  ids.reserve(strand_count);

  for (int i = 0; i < strand_count; ++i) {
    auto phase = std::make_shared<std::atomic<int>>(0);
    const std::uint64_t id =
        scheduler.spawn_strand([&scheduler, &entered, &resumed, phase]() {
          if (phase->fetch_add(1) == 0) {
            entered.fetch_add(1);
            // Request to park; worker_loop parks this strand when we return,
            // releasing the worker. Resume re-invokes this same function.
            scheduler.park_current(std::nullopt);
            return;
          }
          resumed.fetch_add(1);
        });
    ids.push_back(id);
  }

  expect(wait_for_condition(
             [&]() {
               return scheduler.stats().parks ==
                      static_cast<std::uint64_t>(strand_count);
             },
             std::chrono::milliseconds(2000)),
         "park: all strands should park with only two workers (worker freed)");
  expect(entered.load() == strand_count, "park: every strand ran phase 0");
  expect(resumed.load() == 0,
         "park: no strand resumes before an explicit wake");

  for (const std::uint64_t id : ids) {
    expect(scheduler.wake_strand(id),
           "park: waking a parked (Sleeping) strand should succeed");
  }

  expect(scheduler.wait_until_idle(std::chrono::milliseconds(2000)),
         "park: scheduler should drain after all strands resume");
  expect(resumed.load() == strand_count,
         "park: every parked strand should resume and complete");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.park_resumes >= static_cast<std::uint64_t>(strand_count),
         "park: park_resumes should reflect every resume dispatch");

  scheduler.shutdown();
}

// Layer B end-to-end: a task body that calls task.sleep parks cooperatively
// (releasing its worker) instead of blocking it, then resumes and produces its
// result. Four tasks each sleep once; the process-wide cooperative-park counter
// must advance by exactly four (proving the park path was taken regardless of
// how many workers the default pool has), and the results must still be
// correct.
void test_layerb_cooperative_sleep_parks_in_task() {
  const std::uint64_t parks_before =
      amber::runtime::runtime_cooperative_task_park_count();
  // A closure that references the `task` import directly (`task.sleep` in the
  // task body) compiles and verifies: the import alias resolves to a runtime
  // constant and is never captured as an upvalue (see emitter_tests
  // test_closure_module_import_reference_verifies for the BC1313 regression).
  const amber::runtime::ExecutionResult exec =
      execute_source_or_die("import task\n"
                            "\n"
                            "a = task.spawn: task.sleep(10)\n"
                            "b = task.spawn: task.sleep(10)\n"
                            "c = task.spawn: task.sleep(10)\n"
                            "d = task.spawn: task.sleep(10)\n"
                            "\n"
                            "[a.wait(), b.wait(), c.wait(), d.wait()]\n");
  const std::uint64_t parks_after =
      amber::runtime::runtime_cooperative_task_park_count();

  expect(exec.ok(), "cooperative sleep: source program should execute");
  expect(exec.value.is_list() && exec.value.as_list() != nullptr &&
             exec.value.as_list()->items.size() == 4,
         "cooperative sleep: all four tasks should resume and complete");
  expect(parks_after - parks_before == 4,
         "cooperative sleep: each of the four task.sleep calls must park");
}

// Layer B end-to-end (IO-yield): a task that issues a single-shot,
// infinite-timeout socket read cooperatively parks -- releasing its worker --
// until the loopback peer writes, then resumes and completes the read. This is
// the IO analogue of test_layerb_cooperative_sleep_parks_in_task: it drives the
// VM socket dispatch -> reactor wait -> park -> wake -> retry path entirely
// from Amber source. The reader CONNECTS its own client socket inside the task
// body (so the socket and the ByteBuffer are owned by the reader strand, not
// shared across strands) and reaches read! before the server writes, so the
// read genuinely blocks and parks. Keeping the socket strand-local keeps this
// test about the cooperative-park path alone; cross-strand handoff is now a
// sound, separately tested concern (see
// test_strand_confinement_handoff_to_task / _rejects_cross_strand below).
void test_layerb_cooperative_socket_read_parks_in_task() {
  const std::uint64_t parks_before =
      amber::runtime::runtime_cooperative_task_park_count();
  const amber::runtime::ExecutionResult exec = execute_source_or_die(
      "import task\n"
      "import net\n"
      "from io import ByteBuffer\n"
      "\n"
      "listener = net.tcp.listen(\"127.0.0.1\", 0)\n"
      "port = listener.local_endpoint().port()\n"
      "\n"
      "reader = task.spawn:\n"
      "  client = net.tcp.connect(\"127.0.0.1\", port)\n"
      "  buf = ByteBuffer.new(4)\n"
      "  client.read!(buf)\n"
      "  payload = buf.bytes().to_str()\n"
      "  client.close!()\n"
      "  payload\n"
      "\n"
      "server = listener.accept!()\n"
      "task.sleep(50)\n"
      "server.write_all!(\"pong\".bytes())\n"
      "out = reader.wait()\n"
      "server.close!()\n"
      "listener.close!()\n"
      // The program decides correctness itself (avoids resolving the runtime
      // string table from C++): true only if the bytes read equal the payload.
      "out == \"pong\"\n");
  const std::uint64_t parks_after =
      amber::runtime::runtime_cooperative_task_park_count();

  expect(exec.ok(),
         "cooperative socket read: source program should execute" +
             (exec.fault.has_value()
                  ? ": " + exec.fault->error_name + ": " +
                        exec.fault->message
                  : std::string{}));
  // Correctness: park -> resume -> retry must have actually completed the read,
  // returning the exact payload the peer wrote.
  expect_bool(
      exec.value, true,
      "cooperative socket read: reader must return the written payload");
  // Cooperative-park proof. Only a spawned task body is parkable, so the only
  // cooperative park in this run is the reader's blocked read: the main
  // strand's task.sleep (used purely to order the write after the read parks)
  // and reader.wait() both block their worker without parking. So the delta
  // counts the IO park alone -- a read that found data already buffered would
  // not park and the delta would be 0 (proving this asserts the cooperative
  // path, not merely that the read completed).
  expect(parks_after - parks_before >= 1,
         "cooperative socket read: the blocked read must park cooperatively "
         "rather than tying up a worker");
}

// Strand-confinement, end to end: a non-shareable buffer created on the main
// strand must NOT be usable from a spawned task strand without an explicit
// handoff. With the tagged owner id this is a deterministic IsolationError no
// matter which worker the task lands on (IO faults are terminal, so the task
// fails and worker.wait() re-raises it to the program). A regression would
// either let the cross-strand access through (program returns a value) or
// surface a different error. This is the cross-strand half of test (1) at the
// language level; the unit-level namespace soundness lives in io_tests.
void test_strand_confinement_rejects_cross_strand() {
  const amber::runtime::ExecutionResult exec = execute_source_or_die(
      "import task\n"
      "from io import ByteBuffer\n"
      "\n"
      "buf = ByteBuffer.new(4)\n"
      "buf.put_all!(\"hi\".bytes())\n" // usable on the owner (main) strand
      "\n"
      "worker = task.spawn:\n"
      "  buf.put!(120)\n" // cross-strand access -> rejected
      "  \"reached\"\n"
      "\n"
      "worker.wait()\n");
  expect(!exec.ok() && exec.fault.has_value(),
         "cross-strand confinement: task access must fail");
  expect(exec.fault->error_name == "IsolationError",
         "cross-strand confinement: failure must be a deterministic "
         "IsolationError");
}

// Accept-on-main, handle-in-task: the net.http/server handoff pattern. A buffer
// is created on the main strand, handed to a worker task that adopts it, and
// then used there. adopt! is the explicit handoff: it re-binds the owner to the
// task strand so the access is allowed by design. This is test (2): the
// accept/create-on-one-strand, use-in-another pattern, made deterministic.
void test_strand_confinement_handoff_to_task() {
  const amber::runtime::ExecutionResult exec = execute_source_or_die(
      "import task\n"
      "from io import ByteBuffer\n"
      "\n"
      "buf = ByteBuffer.new(4)\n" // created/owned on the main strand
      "\n"
      "worker = task.spawn:\n"
      "  buf.adopt!()\n" // explicit handoff to this strand
      "  buf.put_all!(\"pong\".bytes())\n"
      "  buf.flip!()\n"
      "  buf.bytes().to_str()\n"
      "\n"
      "worker.wait() == \"pong\"\n");
  expect(exec.ok(), "handoff to task: source program should execute");
  expect_bool(exec.value, true,
              "handoff to task: an adopted buffer must be usable on the task "
              "strand");
}

// Accept-on-main, read-in-task over a real socket: the concrete net direction.
// The server side is accepted on the main strand, handed to a worker task that
// adopts the stream (and owns a task-local buffer), and reads the payload the
// client already wrote. Proves the handoff works for a confined TcpStream, not
// only a ByteBuffer.
void test_strand_confinement_socket_handoff_read_in_task() {
  const amber::runtime::ExecutionResult exec = execute_source_or_die(
      "import task\n"
      "import net\n"
      "from io import ByteBuffer\n"
      "\n"
      "listener = net.tcp.listen(\"127.0.0.1\", 0)\n"
      "port = listener.local_endpoint().port()\n"
      "client = net.tcp.connect(\"127.0.0.1\", port)\n"
      "client.write_all!(\"ping\".bytes())\n"
      "server = listener.accept!()\n" // accepted on the MAIN strand
      "\n"
      "worker = task.spawn:\n"
      "  server.adopt!()\n" // hand the socket to this task
      "  buf = ByteBuffer.new(4)\n"
      "  server.read!(buf)\n"
      "  payload = buf.bytes().to_str()\n"
      "  server.close!()\n"
      "  payload\n"
      "\n"
      "out = worker.wait()\n"
      "client.close!()\n"
      "listener.close!()\n"
      "out == \"ping\"\n");
  expect(exec.ok(), "socket handoff: source program should execute");
  expect_bool(exec.value, true,
              "socket handoff: a task that adopts an accepted socket must read "
              "the payload");
}

// Layer B: the cooperative-IO mechanism (the foundation the VM's socket
// dispatch will use). A strand registers reactor readiness interest on a socket
// and parks (releasing its worker); the reactor's readiness completion wakes
// it. The peer is written *before* the strand parks, so the completion races
// the park transition -- exercising the park/wake race fix. A lost wake would
// hang the strand and time out wait_until_idle.
void test_layerb_io_park_via_reactor() {
  amber::runtime::RuntimeScheduler scheduler(2);
  amber::runtime::RuntimeReactor &reactor =
      amber::runtime::RuntimeReactor::instance();

  int fds[2];
  expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
         "io park: socketpair should succeed");
  const char byte = 'x';
  expect(::write(fds[1], &byte, 1) == 1, "io park: peer write should succeed");
  const int read_fd = fds[0];

  std::atomic<int> phase{0};
  std::atomic<bool> resumed{false};
  scheduler.spawn_strand([&scheduler, &reactor, &phase, &resumed, read_fd]() {
    if (phase.fetch_add(1) == 0) {
      const std::uint64_t self_id = amber::runtime::current_runtime_strand_id();
      // Park first, then register interest: a completion that fires before we
      // finish parking sets park_wake_pending rather than being lost.
      scheduler.park_current(std::nullopt);
      reactor.wait_async(read_fd, amber::runtime::ReactorInterest::Read,
                         std::nullopt, nullptr,
                         [&scheduler, self_id](amber::runtime::ReactorOutcome) {
                           scheduler.wake_strand(self_id);
                         });
      return;
    }
    resumed = true;
  });

  expect(scheduler.wait_until_idle(std::chrono::milliseconds(2000)),
         "io park: scheduler should drain (no lost wake)");
  expect(resumed.load(),
         "io park: strand should resume after reactor readiness");
  scheduler.shutdown();
  ::close(fds[0]);
  ::close(fds[1]);
}

int main() {
  test_layerb_scheduler_park_resume_frees_worker();
  test_layerb_io_park_via_reactor();
  test_layerb_cooperative_sleep_parks_in_task();
  test_layerb_cooperative_socket_read_parks_in_task();
  test_strand_confinement_rejects_cross_strand();
  test_strand_confinement_handoff_to_task();
  test_strand_confinement_socket_handoff_read_in_task();
  test_std010_task_async_spawn_and_wait_return_values();
  test_std010_task_wait_timeout_does_not_cancel_child();
  test_std010_task_yield_sleep_and_cancel_surface();
  test_std010_task_sync_block_suppresses_cooperative_yield();
  test_std010_task_failure_is_reported_by_handle();
  test_std011_task_handle_state_result_failure_contract();
  test_std012_channel_buffered_fifo_close_and_isolation();
  test_std012_channel_waiting_senders_fifo_and_send_timeout();
  test_std012_channel_waiting_receivers_fifo_timeout_and_cancellation();
  test_std013_mutex_lock_unlock_owned_and_errors();
  test_std013_mutex_waiter_fifo();
  test_std013_mutex_synchronize_return_and_unwind();
  test_std013_mutex_owner_ids_are_unique_across_schedulers();
  test_std013_mutex_lock_wait_cancellation();
  test_std014_atomic_get_set_compare_and_set_and_guard();
  test_std014_atomic_update_value_retry_and_guard();
  test_std014_atomic_cross_strand_counter();
  test_std015_barrier_release_timeout_and_cancellation();
  test_std015_flow_gather_and_scatter_map_ordered_results();
  test_std015_flow_reduce_broadcast_and_failure_collection();
  test_std015_flow_isolation_checked_and_unchecked();
  test_std016_threaded_collection_iteration_and_transforms();
  test_std016_threaded_collection_scatter_policies_bound_task_count();
  test_std016_threaded_collection_combination_and_permutation();
  test_std016_threaded_collection_failure_and_isolation();
  test_std017_source_level_task_sync_stack_compiles_and_runs();
  test_std017_source_level_flow_and_threaded_collection_compile_and_run();
  test_std018_task_local_basic_nested_exception_and_sleep();
  test_std018_task_local_spawn_inheritance_and_isolation();
  test_std018_task_local_distinct_tasks_and_parallel_stress();
  test_std018_task_local_deterministic_worker_migration();
  test_std018_task_local_lifecycle_cleanup_and_registry_reuse();
  std::cout << "stdlib_task_tests: ok\n";
  return 0;
}
