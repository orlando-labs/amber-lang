#!/usr/bin/env python3
"""Full-native net.http QUERY smoke test against a loopback server."""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import threading
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"native HTTP QUERY test failed: {message}")


def receive_request(connection: socket.socket) -> bytes:
    request = bytearray()
    content_length: int | None = None
    while True:
        chunk = connection.recv(4096)
        if not chunk:
            break
        request.extend(chunk)
        marker = request.find(b"\r\n\r\n")
        if marker < 0:
            continue
        if content_length is None:
            for line in request[:marker].split(b"\r\n")[1:]:
                name, separator, value = line.partition(b":")
                if separator and name.strip().lower() == b"content-length":
                    content_length = int(value.strip())
                    break
            if content_length is None:
                content_length = 0
        if len(request) >= marker + 4 + content_length:
            break
    return bytes(request)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    amberc = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build/amberc"
    work = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "build/native-http-query"
    if not amberc.is_absolute():
        amberc = (root / amberc).resolve()
    if not work.is_absolute():
        work = (root / work).resolve()
    work.mkdir(parents=True, exist_ok=True)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(2)
    listener.settimeout(120)
    port = listener.getsockname()[1]
    received: list[bytes] = []
    server_error: list[BaseException] = []

    def serve() -> None:
        try:
            for index in range(3):
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(10)
                    received.append(receive_request(connection))
                    body = f"native-query-ok-{index + 1}".encode()
                    status = 207 + index
                    connection.sendall(
                        f"HTTP/1.1 {status} Multi-Status\r\n".encode()
                        + f"Content-Length: {len(body)}\r\n".encode()
                        + b"Content-Type: text/plain\r\nConnection: close\r\n\r\n"
                        + body
                    )
        except BaseException as error:  # surfaced on the main test thread
            server_error.append(error)

    server = threading.Thread(target=serve, daemon=True)
    server.start()

    source = work / "query.am"
    source.write_text(
        "package native.http_query\n\n"
        "import task\n"
        "from net.http import Client, RequestBody, Server, ServerRequest, ServerResponse\n\n"
        "export main\n\n"
        "def main():\n"
        "  client = Client()\n"
        "  server_types = [Server, ServerRequest, ServerResponse]\n"
        "  types_ok = server_types.count() == 3\n"
        f"  first = client.query(\"http://127.0.0.1:{port}/search?active=1\",\n"
        "    headers: {\"content-type\": \"application/sql\", \"x-native-map\": \"preserved\"},\n"
        "    body: \"SELECT 1\")\n"
        "  first_ok = first.status() == 207 and first.body_text() == \"native-query-ok-1\"\n"
        f"  second = client.query(\"http://127.0.0.1:{port}/search?active=1\",\n"
        "    headers: {\"content-type\": \"application/sql\"},\n"
        "    body: \"SELECT 2\") |response|:\n"
        "    if response.status() == 208:\n"
        "      response.body_text()\n"
        "    else:\n"
        "      \"unexpected-scoped-status\"\n"
        "  payload = RequestBody.stream(length: 18) |writer|:\n"
        "    writer.write_all!(\"native-stream-body\".bytes)\n"
        f"  third = client.post(\"http://127.0.0.1:{port}/upload\", body: payload)\n"
        "  third_ok = third.status() == 209 and third.body_text() == \"native-query-ok-3\"\n"
        "  internal_server = Server(host: \"127.0.0.1\", port: 0, workers: 1)\n"
        "  internal_port = internal_server.port()\n"
        "  runner = task.spawn:\n"
        "    internal_server.serve(max_requests: 1) |request|:\n"
        "      ServerResponse.stream(\n"
        "        headers: {\"x-native-one\": \"one\", \"x-native-two\": \"two\"},\n"
        "        trailers: [\"x-native-done\"]) |writer|:\n"
        "          writer.write(\"native-stream-response\")\n"
        "          writer.close()\n"
        "          writer.trailer(\"x-native-done\", \"complete\")\n"
        "          writer.finish()\n"
        "  internal = client.get(\"http://127.0.0.1:#{internal_port}/stream\")\n"
        "  internal_body = internal.body_text()\n"
        "  internal_trailer = internal.body().trailers().first(\"x-native-done\")\n"
        "  runner.wait()\n"
        "  internal_ok = internal.headers().first(\"transfer-encoding\") == \"chunked\" and internal.headers().first(\"x-native-one\") == \"one\" and internal.headers().first(\"x-native-two\") == \"two\" and internal_body == \"native-stream-response\" and internal_trailer == \"complete\"\n"
        "  if types_ok and first_ok and second == \"native-query-ok-2\" and third_ok and internal_ok:\n"
        "    \"native-query-ok\"\n"
        "  else:\n"
        "    \"unexpected-query-result\"\n"
    )
    executable = work / "query"
    build = subprocess.run(
        [
            str(amberc),
            "build",
            str(source),
            "--target",
            "native",
            "--entry",
            "main-only",
            "--require-full-native",
            "--grant",
            "net.connect",
            "--grant",
            "net.listen",
            "-o",
            str(executable),
            "--out-dir",
            str(work),
        ],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=240,
    )
    if build.returncode != 0:
        listener.close()
        fail(build.stderr or build.stdout)
    result = json.loads(build.stdout)
    bytecode_fallback = result.get(
        "native_bytecode_fallback", result.get("bytecode_fallback")
    )
    if bytecode_fallback is not False:
        listener.close()
        fail(f"build retained bytecode fallback: {result}")
    full_coverage = result.get(
        "native_full_coverage", result.get("native_graph_full_coverage")
    )
    if full_coverage is not True:
        listener.close()
        fail(f"build did not report full native coverage: {result}")

    run = subprocess.run(
        [str(executable)], capture_output=True, text=True, timeout=15, cwd=root
    )
    server.join(timeout=15)
    listener.close()
    if server.is_alive():
        fail("loopback server did not finish")
    if server_error:
        fail(f"loopback server error: {server_error[0]}")
    if run.returncode != 0:
        fail(f"native executable exited {run.returncode}: {run.stderr}")
    if run.stdout != '"native-query-ok"\n':
        fail(f"unexpected stdout: {run.stdout!r}")
    if len(received) != 3:
        fail("server did not receive exactly three requests")

    for index, request in enumerate(received):
        head, separator, body = request.partition(b"\r\n\r\n")
        if not separator:
            fail("request head was incomplete")
        lines = head.split(b"\r\n")
        expected_line = (
            b"QUERY /search?active=1 HTTP/1.1"
            if index < 2
            else b"POST /upload HTTP/1.1"
        )
        if lines[0] != expected_line:
            fail(f"wrong request line: {lines[0]!r}")
        headers = {}
        for line in lines[1:]:
            name, colon, value = line.partition(b":")
            if colon:
                headers[name.strip().lower()] = value.strip().lower()
        if index < 2 and headers.get(b"content-type") != b"application/sql":
            fail(f"wrong Content-Type: {headers.get(b'content-type')!r}")
        if index == 0 and headers.get(b"x-native-map") != b"preserved":
            fail(f"native Map bridge dropped a string key: {headers!r}")
        expected_body = (
            f"SELECT {index + 1}".encode()
            if index < 2
            else b"native-stream-body"
        )
        if body != expected_body:
            fail(f"wrong request body: {body!r}")

    print("native HTTP QUERY test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
