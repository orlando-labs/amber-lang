#include "runtime/context.h"
#include "runtime/io.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using amber::runtime::RuntimeByteBuffer;
using amber::runtime::RuntimeBytes;
using amber::runtime::RuntimeEndpoint;
using amber::runtime::RuntimeFile;
using amber::runtime::RuntimeFileMode;
using amber::runtime::RuntimeFileOpenOptions;
using amber::runtime::RuntimePath;
using amber::runtime::RuntimePipe;
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

int main() {
  test_name_enums_and_path();
  test_byte_buffer();
  test_file_contract();
  test_pipe_contract();
  test_checked_and_unchecked_sharing();
  test_tcp_roundtrip_and_shutdown();
  test_udp_datagram_boundary();
  std::cout << "io_tests: ok\n";
  return 0;
}
