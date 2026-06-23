#include "runtime/context.h"
#include "runtime/io.h"
#include "runtime/reactor.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using amber::runtime::ReactorInterest;
using amber::runtime::ReactorOutcome;
using amber::runtime::RuntimeByteBuffer;
using amber::runtime::RuntimeBytes;
using amber::runtime::RuntimeEndpoint;
using amber::runtime::RuntimeFile;
using amber::runtime::RuntimeFileMode;
using amber::runtime::RuntimeFileOpenOptions;
using amber::runtime::RuntimePath;
using amber::runtime::RuntimePipe;
using amber::runtime::RuntimeReactor;
using amber::runtime::RuntimeTaskScope;
using amber::runtime::RuntimeTcpListener;
using amber::runtime::RuntimeTcpStream;
using amber::runtime::RuntimeUdpSocket;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "io test failed: " << message << "\n";
    std::exit(1);
  }
}

std::string temp_path(const std::string &suffix) {
  return "/tmp/amber_io_" + std::to_string(::getpid()) + "_" + suffix;
}

void test_name_enums_and_path() {
  expect(amber::runtime::runtime_file_mode_from_name("read") ==
             RuntimeFileMode::Read,
         "read mode should normalize");
  expect(amber::runtime::runtime_file_mode_from_name("read_write") ==
             RuntimeFileMode::ReadWrite,
         "read_write mode should normalize");
  expect(!amber::runtime::runtime_file_mode_from_name("READ").has_value(),
         "NameEnum normalization must be case-sensitive");
  expect(!amber::runtime::runtime_file_mode_from_name("r").has_value(),
         "NameEnum normalization must reject aliases");
  RuntimeEndpoint parsed;
  expect(RuntimeEndpoint::parse("localhost:80", &parsed).ok &&
             parsed.host == "localhost" && parsed.port == 80,
         "endpoint parse mismatch");
  expect(RuntimeEndpoint::parse("localhost:80junk", &parsed).error_name ==
             "ArgumentError",
         "endpoint parse must reject trailing port characters");

  RuntimePath path("/var");
  RuntimePath joined = path.join("log").join("amber.log");
  expect(joined.string() == "/var/log/amber.log", "path join mismatch");
  expect(joined.basename() == "amber.log", "path basename mismatch");
  expect(joined.extname() == ".log", "path extname mismatch");
  expect(joined.parent().string() == "/var/log", "path parent mismatch");
  expect(joined.absolute(), "path should be absolute");
  expect(RuntimePath("a/../b/./c").normalize().string() == "b/c",
         "path normalize mismatch");
}

void test_byte_buffer() {
  auto buffer = std::make_shared<RuntimeByteBuffer>(4);
  expect(buffer->count() == 0 && buffer->capacity() == 4 &&
             buffer->remaining() == 4 && buffer->position() == 0 &&
             buffer->limit() == 4,
         "new byte buffer shape mismatch");
  const std::uint8_t bytes[] = {'a', 'b', 'c'};
  expect(buffer->append(bytes, 3) == 3, "byte buffer append mismatch");
  expect(buffer->bytes() == "abc", "byte buffer bytes mismatch");
  expect(buffer->flip().ok && buffer->position() == 0 && buffer->limit() == 3 &&
             buffer->remaining() == 3,
         "byte buffer flip mismatch");
  expect(buffer->get().byte == 'a', "byte buffer get mismatch");
  auto slice = buffer->read_slice(1);
  expect(slice->count() == 1 && slice->bytes()->string() == "b" &&
             !slice->shareable(),
         "mutable byte slice mismatch");
  expect(slice->copy_bytes()->shareable(),
         "copied slice bytes should be shareable");
  buffer.reset();
  expect(slice->owner() != nullptr && slice->bytes()->string() == "b",
         "byte slice should keep its mutable owner alive");
  buffer = slice->owner();
  expect(buffer->compact().ok && buffer->position() == 1 &&
             buffer->remaining() == 3,
         "byte buffer compact mismatch");
  expect(buffer->put('d').ok && buffer->flip().ok && buffer->bytes() == "cd",
         "byte buffer compact/put roundtrip mismatch");
  expect(buffer->freeze_bytes()->string() == "cd",
         "byte buffer freeze bytes mismatch");
  expect(buffer->clear().ok, "byte buffer clear failed");
  expect(buffer->count() == 0 && buffer->remaining() == 4,
         "byte buffer clear mismatch");
  expect(buffer->put(256).error_name == "ArgumentError",
         "byte buffer must reject invalid byte");

  RuntimeBytes immutable(std::string("\x00\x7f\xff", 3));
  expect(immutable.count() == 3 && immutable.at(-1).byte == 255,
         "Bytes indexing mismatch");
  expect(immutable.hex() == "007fff", "Bytes hex mismatch");
}

void test_file_contract() {
  const RuntimePath path(temp_path("file.txt"));
  RuntimeFileOpenOptions write_options;
  write_options.create = true;
  write_options.truncate = true;
  auto opened = RuntimeFile::open(path, RuntimeFileMode::Write, write_options);
  expect(opened.ok && opened.file != nullptr, "file write open failed");
  expect(opened.file->write_all("\nalpha\nbeta").ok, "file write_all failed");
  expect(opened.file->flush().ok, "file flush failed");
  expect(opened.file->close().ok && opened.file->close().ok,
         "file close must be idempotent");
  expect(opened.file->write("x").error_name == "AlreadyClosedError",
         "write after close should fail");

  auto reader = RuntimeFile::open(path, RuntimeFileMode::Read);
  expect(reader.ok, "file read open failed");
  RuntimeByteBuffer empty(0);
  expect(reader.file->read(empty).error_name == "ArgumentError",
         "empty read buffer should fail");

  auto first = reader.file->read_line();
  expect(first.ok && first.bytes.empty() && !first.eof,
         "empty line must not be EOF");
  auto second = reader.file->read_line();
  expect(second.ok && second.bytes == "alpha", "line read mismatch");
  auto exact = reader.file->read_exact(4);
  expect(exact.ok && exact.bytes == "beta", "read_exact mismatch");
  RuntimeByteBuffer eof_buffer(1);
  auto eof = reader.file->read(eof_buffer);
  expect(eof.ok && eof.eof && eof.count == 0,
         "buffer read must expose EOF as zero");
  expect(reader.file->close().ok, "reader close failed");

  auto short_reader = RuntimeFile::open(path, RuntimeFileMode::Read);
  expect(short_reader.ok, "short reader open failed");
  expect(short_reader.file->read_exact(128).error_name == "EOFError",
         "short read_exact should raise EOFError");
  short_reader.file->close();

  RuntimeFileOpenOptions invalid;
  invalid.truncate = true;
  expect(RuntimeFile::open(path, RuntimeFileMode::Read, invalid).error_name ==
             "ArgumentError",
         "read+truncate should be rejected");
  invalid = {};
  invalid.exclusive = true;
  expect(RuntimeFile::open(path, RuntimeFileMode::Write, invalid).error_name ==
             "ArgumentError",
         "exclusive without create should be rejected");
  ::unlink(path.string().c_str());
}

void test_pipe_contract() {
  std::shared_ptr<amber::runtime::RuntimePipeReader> detached_reader;
  std::shared_ptr<amber::runtime::RuntimePipeWriter> detached_writer;
  {
    auto detached = RuntimePipe::create(2);
    detached_reader = detached.reader;
    detached_writer = detached.writer;
  }
  auto recovered_pipe = detached_reader->pipe();
  expect(recovered_pipe != nullptr &&
             recovered_pipe->reader() == detached_reader &&
             detached_writer->pipe() == recovered_pipe,
         "pipe endpoints should retain a recoverable pipe object");

  auto pipe = RuntimePipe::create(3);
  expect(pipe.ok && pipe.pipe != nullptr && pipe.pipe->capacity() == 3,
         "bounded pipe create failed");
  expect(pipe.writer->try_write("abcd").count == 3,
         "bounded pipe should allow partial write");
  expect(pipe.writer->try_write("x").would_block,
         "full pipe try_write should would-block");
  RuntimeByteBuffer first(2);
  expect(pipe.reader->read(first).count == 2 && first.bytes() == "ab",
         "bounded pipe read mismatch");
  expect(pipe.writer->write_all("de").ok, "pipe write_all failed");
  RuntimeByteBuffer rest(3);
  expect(pipe.reader->read(rest).count == 3 && rest.bytes() == "cde",
         "pipe buffered ordering mismatch");
  expect(pipe.reader->try_read(rest).error_name == "ArgumentError",
         "full target buffer should be rejected");
  rest.clear();
  expect(pipe.reader->try_read(rest).would_block,
         "empty open pipe should would-block");
  expect(pipe.writer->close().ok, "pipe writer close failed");
  expect(pipe.reader->read(rest).eof, "writer close should expose EOF");
  expect(pipe.reader->close().ok && pipe.reader->close().ok,
         "pipe close should be idempotent");

  auto broken = RuntimePipe::create(1);
  broken.reader->close();
  expect(broken.writer->write("x").error_name == "BrokenPipeError",
         "write after reader close should break pipe");

  auto timed = RuntimePipe::create(1);
  RuntimeByteBuffer wait_buffer(1);
  auto timeout = timed.reader->read(wait_buffer, 20ms);
  expect(timeout.timed_out && timeout.error_name == "TimeoutError",
         "empty pipe read should time out");

  std::atomic<bool> cancel{true};
  {
    RuntimeTaskScope task_scope(7, &cancel);
    auto cancelled = timed.reader->read(wait_buffer, 1s);
    expect(cancelled.cancelled && cancelled.error_name == "CancelledError",
           "pipe read should observe task cancellation");
  }

  auto rendezvous = RuntimePipe::create(0);
  std::atomic<bool> write_done{false};
  std::thread writer([&] {
    auto result = rendezvous.writer->write("z", 1s);
    write_done.store(result.ok);
  });
  std::this_thread::sleep_for(20ms);
  expect(!write_done.load(), "rendezvous writer should wait for reader");
  RuntimeByteBuffer handoff(1);
  expect(rendezvous.reader->read(handoff, 1s).ok && handoff.bytes() == "z",
         "rendezvous handoff failed");
  writer.join();
  expect(write_done.load(), "rendezvous writer did not complete");
}

void test_checked_and_unchecked_sharing() {
  auto checked = RuntimePipe::create(1);
  std::atomic<bool> allowed{false};
  std::thread accepted([&] { allowed.store(checked.writer->write("x").ok); });
  accepted.join();
  expect(allowed.load(), "checked pipe endpoints should be shareable");
  checked.reader->close();
  expect(checked.writer->write("y").error_name == "BrokenPipeError",
         "shared pipe must preserve close checks");

  auto buffer = std::make_shared<RuntimeByteBuffer>(1);
  std::string buffer_error;
  std::thread rejected([&] { buffer_error = buffer->put('x').error_name; });
  rejected.join();
  expect(buffer_error == "IsolationError",
         "cross-thread ByteBuffer access must fail");

  const RuntimePath path(temp_path("isolation.txt"));
  RuntimeFileOpenOptions options;
  options.create = true;
  options.truncate = true;
  auto file = RuntimeFile::open(path, RuntimeFileMode::Write, options);
  expect(file.ok, "isolation file open failed");
  std::string file_error;
  std::thread file_rejected(
      [&] { file_error = file.file->write("x").error_name; });
  file_rejected.join();
  expect(file_error == "IsolationError",
         "checked file handle should be confined");
  file.file->allow_unchecked_sharing();
  std::atomic<bool> file_allowed{false};
  std::thread file_accepted(
      [&] { file_allowed.store(file.file->write("x").ok); });
  file_accepted.join();
  expect(file_allowed.load(), "unchecked file sharing should be allowed");
  file.file->close();
  ::unlink(path.string().c_str());
}

// Soundness of the strand-confinement owner identity across namespaces, and the
// explicit adopt handoff. These run entirely on one OS thread, toggling the
// worker/strand TLS via the scopes, so the result is fixed regardless of how a
// scheduler would have interleaved real workers.
void test_strand_confinement_owner_namespaces() {
  using amber::runtime::RuntimeStrandScope;
  using amber::runtime::RuntimeWorkerScope;

  // Regression for the cross-namespace owner-id collision. A buffer confined to
  // *worker* 7 must not be treated as owned by *strand* 7. The previous check
  // compared bare ids minted by independent counters that both start at 1, so
  // worker 7 == strand 7 and the access was wrongly allowed; the tagged owner
  // id keeps the namespaces apart, making this a deterministic IsolationError
  // no matter how the raw counters happen to line up.
  std::shared_ptr<RuntimeByteBuffer> worker_owned;
  {
    RuntimeWorkerScope worker(7);
    worker_owned = std::make_shared<RuntimeByteBuffer>(4);
  }
  {
    RuntimeStrandScope strand(7);
    expect(worker_owned->put('x').error_name == "IsolationError",
           "worker-owned buffer must not be reachable from the same-numbered "
           "strand id");
  }

  // Confinement and explicit handoff between two strands.
  {
    RuntimeStrandScope strand(9);
    auto buffer = std::make_shared<RuntimeByteBuffer>(4);
    expect(buffer->put('a').ok, "buffer must be usable from its owner strand");
    {
      RuntimeStrandScope other(10);
      expect(buffer->put('b').error_name == "IsolationError",
             "buffer must be confined to its creating strand");
      // adopt! re-owns the buffer to the current strand even though it could
      // not be accessed a moment ago -- adoption is the operation that grants
      // ownership, so it cannot itself require ownership.
      buffer->adopt_to_current_owner();
      expect(buffer->put('b').ok,
             "adopt_to_current_owner must re-own the buffer to this strand");
    }
    // The handoff moved ownership: strand 9 has lost access.
    expect(buffer->put('c').error_name == "IsolationError",
           "after handoff the original strand must lose access");
  }

  // The same identity/handoff applies to confined IO resources (files/sockets),
  // which share check_access()'s owner comparison.
  const RuntimePath confine_path(temp_path("confine.txt"));
  RuntimeFileOpenOptions options;
  options.create = true;
  options.truncate = true;
  std::shared_ptr<RuntimeFile> confined_file;
  {
    RuntimeWorkerScope worker(3);
    auto opened = RuntimeFile::open(confine_path, RuntimeFileMode::Write,
                                    options);
    expect(opened.ok, "confinement file open failed");
    confined_file = opened.file;
  }
  {
    RuntimeStrandScope strand(3);
    expect(confined_file->write("x").error_name == "IsolationError",
           "worker-owned file must not be reachable from same-numbered strand");
    confined_file->adopt_to_current_owner();
    expect(confined_file->write("x").ok,
           "adopt_to_current_owner must re-own the file to this strand");
  }
  confined_file->close();
  ::unlink(confine_path.string().c_str());
}

void test_tcp_roundtrip_and_shutdown() {
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "TCP listen failed: " + listening.error_name + " " +
                           listening.message);
  const RuntimeEndpoint endpoint = listening.listener->local_endpoint();
  expect(endpoint.port != 0, "TCP listener did not get a port");
  listening.listener->allow_unchecked_sharing();

  std::string server_error;
  std::thread server([&] {
    auto accepted = listening.listener->accept(2s);
    if (!accepted.ok) {
      server_error = accepted.error_name;
      return;
    }
    RuntimeByteBuffer input(4);
    auto read = accepted.stream->read(input, 2s);
    if (!read.ok || input.bytes() != "ping") {
      server_error = read.error_name.empty() ? "payload" : read.error_name;
      return;
    }
    auto written = accepted.stream->write_all("pong", 2s);
    if (!written.ok) {
      server_error = written.error_name;
    }
    accepted.stream->close();
  });

  auto connected = RuntimeTcpStream::connect(endpoint, 2s);
  expect(connected.ok, "TCP connect failed");
  expect(connected.stream->set_nodelay(true).ok, "TCP nodelay option failed");
  expect(connected.stream->write_all("ping", 2s).ok, "TCP client write failed");
  RuntimeByteBuffer response(4);
  expect(connected.stream->read(response, 2s).ok && response.bytes() == "pong",
         "TCP roundtrip mismatch");
  expect(
      connected.stream->shutdown(amber::runtime::RuntimeShutdownSide::Write).ok,
      "TCP shutdown write failed");
  connected.stream->close();
  server.join();
  expect(server_error.empty(), "TCP server failed: " + server_error);
  listening.listener->close();
}

void test_udp_datagram_boundary() {
  auto receiver = RuntimeUdpSocket::bind({"127.0.0.1", 0});
  auto sender = RuntimeUdpSocket::bind({"127.0.0.1", 0});
  expect(receiver.ok && sender.ok, "UDP bind failed");
  expect(receiver.socket->try_recv_from().would_block,
         "empty UDP socket should would-block");
  const RuntimeEndpoint destination = receiver.socket->local_endpoint();
  expect(sender.socket->send_to("first", destination, 1s).ok,
         "first UDP send failed");
  expect(sender.socket->send_to("second", destination, 1s).ok,
         "second UDP send failed");
  auto first = receiver.socket->recv_from(65507, false, 1s);
  auto second = receiver.socket->recv_from(65507, false, 1s);
  expect(first.ok && first.bytes == "first", "first UDP datagram mismatch");
  expect(second.ok && second.bytes == "second", "second UDP datagram mismatch");

  expect(sender.socket->send_to("oversized", destination, 1s).ok,
         "oversized UDP send failed");
  auto too_large = receiver.socket->recv_from(4, false, 1s);
  expect(too_large.error_name == "DatagramTooLargeError",
         "UDP oversized datagram should fail");
  expect(sender.socket->send_to("truncate", destination, 1s).ok,
         "truncate UDP send failed");
  auto truncated = receiver.socket->recv_from(4, true, 1s);
  expect(truncated.ok && truncated.bytes == "trun",
         "UDP truncate contract mismatch");
  receiver.socket->close();
  sender.socket->close();
}

} // namespace

void make_nonblocking_socketpair(int fds[2]) {
  expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
         "reactor: socketpair failed");
  for (int i = 0; i < 2; ++i) {
    const int flags = ::fcntl(fds[i], F_GETFL, 0);
    ::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
  }
}

void test_reactor() {
  RuntimeReactor &reactor = RuntimeReactor::instance();

  // Readiness: a waiter returns Ready once the peer writes.
  {
    int fds[2];
    make_nonblocking_socketpair(fds);
    std::atomic<ReactorOutcome> result{ReactorOutcome::Error};
    std::thread waiter([&]() {
      result =
          reactor.wait(fds[0], ReactorInterest::Read, std::nullopt, nullptr);
    });
    std::this_thread::sleep_for(20ms);
    const char byte = 'x';
    expect(::write(fds[1], &byte, 1) == 1, "reactor: peer write failed");
    waiter.join();
    expect(result.load() == ReactorOutcome::Ready,
           "reactor: expected Ready on readiness");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // Timeout: nothing is written, the deadline elapses.
  {
    int fds[2];
    make_nonblocking_socketpair(fds);
    const auto start = std::chrono::steady_clock::now();
    const ReactorOutcome out =
        reactor.wait(fds[0], ReactorInterest::Read, start + 50ms, nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(out == ReactorOutcome::TimedOut, "reactor: expected TimedOut");
    expect(elapsed >= 40ms, "reactor: timeout returned too early");
    expect(elapsed < 1000ms, "reactor: timeout returned too late");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // Cancellation: a flipped cancel flag plus a kick wakes the waiter promptly.
  {
    int fds[2];
    make_nonblocking_socketpair(fds);
    std::atomic<bool> cancel{false};
    std::atomic<ReactorOutcome> result{ReactorOutcome::Error};
    std::thread waiter([&]() {
      result =
          reactor.wait(fds[0], ReactorInterest::Read, std::nullopt, &cancel);
    });
    std::this_thread::sleep_for(20ms);
    cancel = true;
    reactor.kick();
    waiter.join();
    expect(result.load() == ReactorOutcome::Cancelled,
           "reactor: expected Cancelled");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // Closed: notify_closed releases a waiter blocked on the fd.
  {
    int fds[2];
    make_nonblocking_socketpair(fds);
    std::atomic<ReactorOutcome> result{ReactorOutcome::Error};
    std::thread waiter([&]() {
      result =
          reactor.wait(fds[0], ReactorInterest::Read, std::nullopt, nullptr);
    });
    std::this_thread::sleep_for(20ms);
    reactor.notify_closed(fds[0]);
    waiter.join();
    expect(result.load() == ReactorOutcome::Closed,
           "reactor: expected Closed on notify_closed");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // Two strands waiting on one fd (the duplex / shared-socket case) both wake.
  {
    int fds[2];
    make_nonblocking_socketpair(fds);
    std::atomic<ReactorOutcome> r1{ReactorOutcome::Error};
    std::atomic<ReactorOutcome> r2{ReactorOutcome::Error};
    std::thread w1([&]() {
      r1 = reactor.wait(fds[0], ReactorInterest::Read, std::nullopt, nullptr);
    });
    std::thread w2([&]() {
      r2 = reactor.wait(fds[0], ReactorInterest::Read, std::nullopt, nullptr);
    });
    std::this_thread::sleep_for(20ms);
    const char byte = 'y';
    expect(::write(fds[1], &byte, 1) == 1, "reactor: duplex peer write failed");
    w1.join();
    w2.join();
    expect(r1.load() == ReactorOutcome::Ready &&
               r2.load() == ReactorOutcome::Ready,
           "reactor: both waiters on one fd should wake Ready");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // Async registration (Layer B mechanism): the completion fires with Ready
  // once the peer writes, driven from the reactor thread rather than blocking
  // the caller.
  {
    int fds[2];
    make_nonblocking_socketpair(fds);
    std::atomic<bool> fired{false};
    std::atomic<ReactorOutcome> got{ReactorOutcome::Error};
    reactor.wait_async(fds[0], ReactorInterest::Read, std::nullopt, nullptr,
                       [&](ReactorOutcome o) {
                         got = o;
                         fired = true;
                       });
    std::this_thread::sleep_for(20ms);
    const char byte = 'z';
    expect(::write(fds[1], &byte, 1) == 1, "reactor: async peer write failed");
    for (int i = 0; i < 200 && !fired.load(); ++i) {
      std::this_thread::sleep_for(5ms);
    }
    expect(fired.load() && got.load() == ReactorOutcome::Ready,
           "reactor: wait_async should complete Ready on readiness");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // Async registration resolves with TimedOut when the deadline elapses.
  {
    int fds[2];
    make_nonblocking_socketpair(fds);
    std::atomic<bool> fired{false};
    std::atomic<ReactorOutcome> got{ReactorOutcome::Error};
    reactor.wait_async(fds[0], ReactorInterest::Read,
                       std::chrono::steady_clock::now() + 50ms, nullptr,
                       [&](ReactorOutcome o) {
                         got = o;
                         fired = true;
                       });
    for (int i = 0; i < 200 && !fired.load(); ++i) {
      std::this_thread::sleep_for(5ms);
    }
    expect(fired.load() && got.load() == ReactorOutcome::TimedOut,
           "reactor: wait_async should complete TimedOut on deadline");
    ::close(fds[0]);
    ::close(fds[1]);
  }
}

// Layer B IO yield (io.cpp half): with cooperative parking enabled, a blocking
// read on a socket with no data available must NOT block -- wait_fd records the
// fd in the thread-local park channel and returns park, which the VM turns into
// a strand park + reactor wait_async. With parking disabled the read blocks as
// usual. (The full VM-driven socket e2e is gated on the Amber-source net/io
// frontend, which still fails to compile -- BC1313 / class-path-ref.)
void test_io_park_emission() {
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "io park: listen failed");
  const RuntimeEndpoint endpoint = listening.listener->local_endpoint();
  auto connected = RuntimeTcpStream::connect(endpoint, 2s);
  expect(connected.ok, "io park: connect failed");
  auto accepted = listening.listener->accept(2s);
  expect(accepted.ok, "io park: accept failed");

  amber::runtime::tls_runtime_io_park_enabled = true;
  amber::runtime::tls_runtime_io_park_requested = false;
  RuntimeByteBuffer buf(4);
  auto parked = accepted.stream->read(buf, 2s);
  amber::runtime::tls_runtime_io_park_enabled = false;
  expect(parked.park && !parked.ok,
         "io park: blocking read on no-data socket should park, not block");
  expect(amber::runtime::tls_runtime_io_park_requested,
         "io park: park request flag should be set");
  expect(amber::runtime::tls_runtime_io_park_request.fd >= 0,
         "io park: park request should record the fd");
  expect(!amber::runtime::tls_runtime_io_park_request.want_write,
         "io park: a read should record read interest");

  // Parking disabled: the same read now blocks until data arrives, then reads.
  expect(connected.stream->write_all("ping", 2s).ok, "io park: client write");
  RuntimeByteBuffer buf2(4);
  auto got = accepted.stream->read(buf2, 2s);
  expect(got.ok && buf2.bytes() == "ping",
         "io park: read should work normally when parking is disabled");

  connected.stream->close();
  accepted.stream->close();
  listening.listener->close();
}

int main() {
  test_name_enums_and_path();
  test_byte_buffer();
  test_file_contract();
  test_pipe_contract();
  test_checked_and_unchecked_sharing();
  test_strand_confinement_owner_namespaces();
  test_tcp_roundtrip_and_shutdown();
  test_udp_datagram_boundary();
  test_reactor();
  test_io_park_emission();
  std::cout << "io_tests: ok\n";
  return 0;
}
