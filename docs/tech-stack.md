# C++ TUI Agent with Separate Core Process — Practical Tech Stack

Researched stack choices for a learning-oriented C++ agent: a `core` daemon process (owns HTTP/SSE, JSON, session state) and a `cli` TUI process talking over local IPC. All versions below were read from live sources (see Sources).

## 1. HTTP client for OpenAI-compatible chat completions with SSE

| Lib | Latest seen | Model | Streaming SSE support |
|---|---|---|---|
| **libcurl** | curl 8.22.0 (2026-09-02) | C, blocking + `curl_multi` | Raw: `CURLOPT_WRITEFUNCTION` gets body chunks; you parse SSE yourself |
| **cpr** | 1.15.0 (`project(cpr VERSION 1.15.0)`) | C++17, wraps libcurl | **First-class**: `SetServerSentEventCallback` parses events for you |
| **Boost.Beast** | Boost 1.92.0 | C++11, part of Boost | Raw async socket; `http::response_parser` + `async_read_some`; SSE parse is yours |
| **cpp-httplib** | v0.60.1 (2026-10-08) | header-only C++11 | **Built-in**: `stream::Get()`, `open_stream()`, and a full `SSEClient` |

**libcurl** is the lowest-level, most portable option and the one cpr is built on. You set `CURLOPT_WRITEFUNCTION`; libcurl calls it with body bytes as they arrive. Critically, **a write callback return value is just a continue/abort signal — there is no per-chunk notification semantics beyond that**, which is exactly the point raised on the curl-library list. So you must buffer bytes yourself and split on the blank-line event terminator. Pros: ubiquitous, HTTP/2, proxy and TLS config, tiny link cost. Cons: C API, manual handle/cleanup management, and you own SSE framing.

**cpr** is the ergonomic sweet spot. Its SSE callback hands you a parsed struct:

```cpp
struct ServerSentEvent {
    std::optional<std::string> id;
    std::string event;   // default "message"
    std::string data;
    std::optional<size_t> retry;
};
```
You register with `session.SetServerSentEventCallback(cpr::ServerSentEventCallback{lambda, userdata})` and return `true` to continue or `false` to stop. This maps 1:1 onto the OpenAI `data: {...}` / `data: [DONE]` stream — you don't write a parser at all. Note the callback is invoked on the request thread, so it blocks unless you run `session.Get()` off the UI thread. cpr requires C++17 (it explicitly errors if the parent project sets a lower standard).

**Boost.Beast** gives you a real async state machine over Asio: `http::async_read_some` on a `response_parser`, then `flat_buffer` chunks. This is the "correct" end-state for a non-blocking core, and it composes with the same `io_context` you'd use for IPC, so you get one event loop for both HTTP and the local socket. The cost is the steepest learning curve of the four and the heaviest dependency (Boost). There are also known reports of occasional `async_read` stalls, so budget debugging time.

**cpp-httplib** is remarkably complete for this use case and is header-only (drop in `httplib.h`). Its streaming README documents a layered API: low-level `open_stream()` → `StreamHandle::read(buf,len)`; iterator `stream::Get()` → `while (result.next()) { result.data(), result.size(); }`; and a high-level `sse::SSEClient` with `on_message`, `on_event("...", ...)`, `on_open`, `on_error`, auto-reconnect, `Last-Event-ID` resumption, and `start_async()`. Caveats it states outright: **no keep-alive on streaming calls** (each `stream::Get()` uses a dedicated connection closed at end of body), **single iteration only**, and **the returned `Result` is not thread-safe** (call from one thread). For an agent, the `SSEClient` + `start_async()` + `stop()` combination is the fastest path to a working streamer.

**Recommendation for a beginner:** start with **cpp-httplib's SSEClient** (zero build friction, real SSE semantics, async mode built in) or **cpr** if you want a conventional packaged dependency. Graduate to **Beast** only when you want the HTTP client and IPC to share one `io_context`.

### Parsing SSE incrementally
The WHATWG spec (HTML Standard §9.2, "Parsing an event stream") is the authority. The practical algorithm, which you need regardless of library if you go the libcurl/Beast route:

1. Keep a `std::string buffer`. Append every chunk from the write callback / `read()` verbatim. **Never assume a chunk ends on an event boundary** — TCP and TLS fragment arbitrarily, and one read frequently contains half an event or three whole ones.
2. Repeatedly find the next complete event terminator. Note the spec allows `\r\n`, `\n`, and (historically) `\r` as line endings, so normalize before splitting.
3. Within a block, process `field: value` lines. Per spec: a leading space after the colon is stripped; a line starting with `:` is a comment (this is what OpenAI-style keep-alive pings use) and must be ignored; `data` lines **concatenate** with a newline between them rather than replacing; `event` sets the type; `id` sets the resume cursor; `retry` sets backoff ms.
4. Dispatch on the blank line that ends the block, then erase the consumed prefix from the buffer — do not `clear()` the whole buffer or you will drop a partial next event.

For OpenAI-compatible streams the body is a series of `data: {"choices":[{"delta":{"content":"..."}}]}` lines with a terminal `data: [DONE]`. Handle the `delta` object's optional `role`, `content`, and `tool_calls` fields; tool-call arguments arrive as **fragments that must be concatenated by `index` before the whole JSON is parseable**. That last point is the single most common bug in C++ agent clients.

## 2. JSON libraries

| Lib | Latest seen | Parse style | Fit for tool calling |
|---|---|---|---|
| **nlohmann/json** | v3.12.0 (2025-04-11) | DOM, header-only, exceptions | Best ergonomics; `json::parse` + implicit conversions |
| **RapidJSON** | — | DOM + SAX, in-situ | Fast, more verbose; SAX for huge payloads |
| **simdjson** | v5.0.4 (2026-10-12) | On-Demand / DOM, SIMD | Fastest, but strict alignment + lifetime rules |

**nlohmann/json** is the right default. Its value semantics make building a chat-completions request trivial and, importantly, make **round-tripping a tool-call `arguments` string into an object and back** a one-liner — you will do this constantly. It also gives you easy serialization of an entire transcript to disk, which is what your transcript-replay test harness needs. Cost: compile times and memory/CPU on large payloads.

**RapidJSON** sits in the middle: materially faster, still a familiar DOM (`Document`, `GetObject`, `HasMember`), with a SAX interface if you ever need to stream-parse enormous responses. You pay in boilerplate and in lifetime discipline (values reference the document's allocator).

**simdjson** is the performance ceiling — its On-Demand API validates and parses using SIMD, and it requires a `padded_string` (it reads past the end of input, so a plain `std::string` is unsafe). Its docs are explicit that documents are **iterators you can only walk once**, and that rewind/re-parse is needed to revisit. For an agent that mostly parses a few KB of SSE per turn, this is complexity you don't need.

**Recommendation:** **nlohmann/json** for the whole project. If profiling later shows JSON is a hotspot (it won't be; the network dominates), swap only the hot path to simdjson. Keep a thin `json_io.hpp` wrapper so the swap stays a one-file change.

## 3. TUI libraries

| Lib | Latest seen | Style | Chat UI + streaming + keys |
|---|---|---|---|
| **FTXUI** | v7.0.3 (2026-08-06) | Functional/declarative, C++17, MIT | **Best**: `App::Loop` + `PostEvent` cross-thread redraw |
| **ncurses** | — | Imperative C, terminfo | Works, but you build widgets yourself |
| **notcurses** | v3.0.17 | Imperative C, rich graphics | Powerful, steep; explicitly *not* an X/Open curses replacement |
| **termbox2** | 772 stars, single-header C | Minimal cell buffer | Simplest primitive loop; no widgets |
| **ImTui** | v1.0.4 | Dear ImGui ported to ncurses | Familiar if you know ImGui; needs libncurses |

**FTXUI is the clear beginner winner for a chat UI.** It is header-friendly, CMake-packaged, cross-platform (Linux/macOS/Windows/WASM), and has **no dependencies beyond the standard library**. Architecturally it splits into a pure functional DOM layer (`text`, `vbox`, `hbox`, `border`, `gauge`) and a retained Component layer — you can ship a static render first and add interactivity later. Its `ScreenInteractive` has been renamed to `App` (there's a `using ScreenInteractive = App;` alias for compatibility), and the pieces you need for streaming are all public on `App`:

- `void Loop(Component)` — run the main loop
- `void PostEvent(Event)` / `void Post(Task)` — **enqueue work from another thread**, which is exactly how your SSE reader thread pushes "new token arrived, redraw"
- `static void PostEventOrExecute(Closure)` — safe even if the loop isn't running yet
- `void RequestAnimationFrame()`, `void Exit()`, `Closure ExitLoopClosure()`
- `void PrintAbove(Element)` — print into terminal scrollback above the app; useful for a non-alternate-screen chat log

Key handling is uniform: `component |= CatchEvent([&](Event e){ ... return false; })`, with `e.input()` giving raw codepoints and `e.DebugString()` for logging. That's a genuinely small amount of code to get a working input box.

**ncurses** is the incumbent and is installed nearly everywhere, but you manage a raw screen, colors, resize (`SIGWINCH`), and scrolling by hand, and its terminfo dependency is a real cross-platform annoyance. **notcurses** is far more capable (Unicode, multimedia, threads) but its own README stresses it is *not* a source-compatible curses implementation nor a drop-in replacement, and its C API is large — a poor first project. **termbox2** is tiny and pleasant: `tb_init()`, `tb_set_cell`/`tb_printf`, `tb_present()`, and a blocking event loop with `tb_peek_event(&ev, timeout_ms)` / `tb_poll_event(&ev)`. Its `tb_peek_event` timeout makes a poll-based streaming loop trivial, and it needs nothing beyond libc. But it gives you no text wrapping, no scrolling region, no widget tree — you'd write the chat view yourself. **ImTui** (v1.0.4) is a thin ncurses backend for Dear ImGui, so if you already know ImGui's immediate-mode API it feels instantly familiar, and you get ImGui's text input and word wrap. Downside: it's a much smaller community, requires libncurses, and Windows support is described as only partial.

**Recommendation:** **FTXUI**, full stop, for a first chat TUI. **termbox2** is the good fallback if you want to understand the terminal from the ground up. Avoid notcurses and ImTui for a first project.

## 4. Networking / IPC between `cli` and `core`

**Boost.Asio vs standalone Asio.** Standalone Asio is the upstream project; Boost.Asio is the same library shipped inside Boost. Asio's own site lists **1.38.2** as latest stable and notes it "is also included in Boost 1.92.0 beta" — i.e. the two are version-locked and API-identical modulo the namespace (`asio::` vs `boost::asio::`) and header prefix. Standalone Asio can be used header-only with C++11+ and **no other Boost dependency**, which drops a multi-hundred-MB build for a learning project. Choose **standalone Asio** if you only need I/O; choose **Boost.Asio** if you already depend on Boost (e.g. Beast) and want one coherent dependency.

**Transport choice:**

- **Unix domain sockets** — filesystem path or Linux abstract namespace, own filesystem permissions, lower overhead than loopback TCP, stream semantics you already know. POSIX-only; **no native Windows support** (Windows 10+ has AF_UNIX, but it's poorly supported and unportable in practice).
- **Windows named pipes** — the native Windows analogue, with real access control and message-mode support. Completely different API (`CreateNamedPipe`/`ConnectNamedPipe`/`ReadFile`), no POSIX equivalent.
- **TCP loopback (127.0.0.1)** — the **only** option that is genuinely identical code on every platform. Cost: another process on the host could connect, so you need a token/handshake; also a port-number collision and firewall-prompt risk.
- **stdio pipes** — child `core` spawned by `cli` with redirected stdin/stdout. Dead simple, OS handles lifecycle, dies with the parent. But it couples process lifetimes, on Windows you cannot do non-blocking async pipe I/O with `io_context` as cleanly, and mixing logs into stdout corrupts the protocol.

**Cross-platform concerns to plan for:** newline/encoding (force binary mode on Windows), process spawning differences (separate `core` binary vs `fork`), path length limits, and the fact that `SIGPIPE`/`EPIPE` handling differs from Windows' error codes.

**Recommended minimal abstraction for a learning project:**

```cpp
// ipc.hpp — the ONLY transport-specific file
class Channel {
public:
    virtual ~Channel() = default;
    virtual asio::awaitable<void> send(std::string_view frame) = 0;
    virtual asio::awaitable<std::string> recv_frame() = 0;
};
std::unique_ptr<Channel> connect_local(const std::string& endpoint); // client
std::unique_ptr<Channel> listen_local(const std::string& endpoint);  // server
```

Implement it **once with TCP loopback on `127.0.0.1`** for your first milestone. It is the least code, it is byte-identical across Linux/macOS/Windows, and you can attach external tooling to debug it. Then add a Unix-domain-socket backend behind the same `Channel` interface on POSIX, and a named-pipe backend on Windows. The key design decision that makes this work: **length-prefix every message** (4-byte little-endian length + payload) so framing is transport-independent, and keep the payload as newline-delimited JSON so it's human-readable in a log. Never let two different transports leak into your business logic.

## 5. Build system

**CMake + vcpkg** is the lowest-friction combination here: vcpkg integrates through a single toolchain file, its release **2026.07.29** reports **2858 ports** (2779 tested on x64-windows), and every library in this stack (ftxui, nlohmann-json, curl, cpr, boost) is a stock port. **Conan 2.33.0** is the more powerful alternative — it has generators for CMake/Meson and a first-class `conanfile.py` for packaging your own components, which matters if `core` later becomes a published library — but vcpkg's manifest mode is genuinely simpler for a single repo. ConanCenter carries roughly 1700 recipes vs vcpkg's 2858.

vcpkg manifest (`vcpkg.json`):
```json
{
  "name": "agent-cli",
  "version-semver": "0.1.0",
  "dependencies": [
    "ftxui", "nlohmann-json", "cpr", "asio",
    { "name": "gtest", "host": true }
  ],
  "builtin-baseline": "<commit-sha from your vcpkg checkout>"
}
```

**Minimal two-binary CMakeLists:**

```cmake
cmake_minimum_required(VERSION 3.25)
project(agent_cli CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ftxui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)

# --- shared static library: everything both processes agree on ---
add_library(agent_common STATIC
  src/common/protocol.cpp     # framed NDJSON messages
  src/common/sse.cpp          # SSE incremental parser
  src/common/chat_types.cpp   # Message, ToolCall, Delta
)
target_include_directories(agent_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(agent_common PUBLIC nlohmann_json::nlohmann_json)

# --- core: owns HTTP/SSE + session state ---
add_executable(agent_core src/core/main.cpp src/core/llm_client.cpp src/core/ipc_server.cpp)
target_link_libraries(agent_core PRIVATE agent_common cpr::cpr)

# --- cli: owns the TUI, talks to core over a local channel ---
add_executable(agent_cli src/cli/main.cpp src/cli/tui.cpp src/cli/ipc_client.cpp)
target_link_libraries(agent_cli PRIVATE agent_common ftxui::component)

# --- tests ---
enable_testing()
find_package(GTest CONFIG REQUIRED)
add_executable(unit_tests tests/sse_parser_test.cpp tests/protocol_test.cpp)
target_link_libraries(unit_tests PRIVATE agent_common GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests)
```

Configure with `cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` (or put that in `CMakePresets.json` as `toolchainFile`). Note the dependency direction: **`agent_common` must never link cpr or ftxui** — HTTP belongs to `core`, rendering to `cli`, and the shared lib holds only the wire protocol and pure types. That single rule is what keeps the two-process split honest.

## 6. SSE streaming without blocking the TUI

The core problem: a blocking HTTP read on the UI thread freezes the interface, and a naive "parse on the reader thread, mutate UI state directly" data race corrupts the render. The clean structure is **one reader thread in `core`, one UI thread in `cli`, connected by a queue** — and it works precisely because `core` is a separate process.

In `core`:

1. A dedicated thread runs the blocking HTTP call (cpr's `Get()`, or `cpp-httplib`'s `sse.start()`). It parses SSE incrementally with the buffer-and-split algorithm from §1.
2. For each parsed event it emits a framed NDJSON message to the IPC channel — `{"type":"delta","text":"..."}`, `{"type":"tool_call_delta",...}`, `{"type":"done"}`, `{"type":"error",...}`.
3. If you used Beast/Asio instead, this is *not* a thread at all: you co_await the socket read on the same `io_context` that serves IPC, and the event loop naturally interleaves. That's the elegant version, and it's why Beast is the eventual upgrade path.

In `cli`:

4. The FTXUI `App::Loop` owns the terminal and the input.
5. A second thread reads IPC frames and, for each one, calls `screen.PostEvent(Event::Custom)` (or `screen.Post(task)`). **`PostEvent` is the thread-safe hand-off** — it enqueues onto the loop, which wakes, re-renders, and drains. Your component's `Render()` reads the accumulated text; because mutation happens on the loop thread inside the posted task, there is no race.
6. `Event::Custom` is a "something changed, redraw" signal; if you need to carry a payload, post a `Task` (a `std::function<void()>`) instead — that's what `App::Post` is for.
7. For smooth token-by-token output, don't redraw per token. Coalesce: accumulate deltas and call `RequestAnimationFrame()` at a fixed cadence, or batch deltas on a ~16–33 ms timer. This keeps CPU sane when the model streams fast.
8. Terminate cleanly: post a task that calls `screen.Exit()` on done/error, and join the IPC thread before destroying the channel. `PostEventOrExecute` exists so the shutdown signal works even if the loop already stopped.

The invariant to internalize: **blocking I/O lives in `core` on its own thread (or as a coroutine on the io_context); the TUI thread never blocks on the network, and is only ever mutated via posted tasks.**

## 7. Testing & observability

**A fake LLM server is the highest-value test asset.** Write a small `httplib::Server` that serves `POST /v1/chat/completions` and returns a scripted SSE body. Make it deterministic and controllable:

- **Canned transcripts**: serve fixture streams from `tests/fixtures/*.sse` so tests assert exact output.
- **Fault injection**: emit a chunk split mid-JSON-event, mid-UTF-8 codepoint, and mid-`data:` line. This is *the* test that catches the incremental-parser bug class from §1, and it's nearly impossible to trigger against a real API on demand.
- **Slow streams**: sleep 50 ms between events to exercise the coalescing logic and prove the TUI stays responsive.
- **Tool-call fragmentation**: a fixture where `tool_calls[0].function.arguments` arrives in three pieces. Assert the reassembled JSON parses.
- **Error paths**: 401, 429 with `Retry-After`, 500, and a mid-stream connection drop (to test SSE reconnect + `Last-Event-ID`).

For HTTP-level stubbing without writing a server, **WireMock 3.13.2** can match on request body and return a chunked/streamed response, though a hand-rolled httplib server is less friction for SSE specifically.

**Transcript replay.** Log every turn to a JSON Lines file: the request body (including the full message array and tool schemas), plus every raw SSE event with a timestamp. Because you're on nlohmann/json, this is a few lines. Then:

- A `replay` mode in `core` that reads a recorded transcript and re-emits the *same* SSE events into the IPC channel — this tests the entire `core`→`cli` pipeline, including your UI's streaming and rendering, with **zero network**, deterministically, in CI.
- A golden test that runs replay and diffs the resulting assembled assistant message + tool calls against a checked-in expectation. Bugs in delta concatenation and tool-call index merging show up immediately.
- Because the transcript format is plain NDJSON, it doubles as a debugging artifact you can read in a terminal — critical when the failure is "the UI showed a garbled token".

**Observability.** Use **spdlog**/**fmt** in `core` and log to a file or stderr, *never* stdout — stdout is a protocol channel if you ever use stdio pipes. Correlate with a per-request ID in every log line and every IPC frame. Add a `core --dump-transcript` flag rather than a live debugger. In the TUI, render a hidden `Ctrl-D` diagnostics pane showing the last N IPC frames and their timestamps; it makes "is it stuck in the model or stuck in my UI?" a two-second question. Note that `CMAKE_EXPORT_COMPILE_COMMANDS ON` (already in the sketch) gives you clangd/IDE support for free.

Finally, keep unit tests pure: the SSE parser takes `std::string_view` chunks and yields events with no I/O at all, so it's trivially unit-testable and is where most of your real bugs will live.

---

## Sources

- curl 8.22.0 release — https://api.github.com/repos/curl/curl/releases/latest
- libcurl SSE / write-callback semantics (curl-library mailing list) — https://curl.se/mail/lib-2019-02/0069.html
- cpr version 1.15.0 and C++17 requirement — https://raw.githubusercontent.com/libcpr/cpr/master/CMakeLists.txt
- cpr Server-Sent Events documentation — https://raw.githubusercontent.com/libcpr/docs/6be2090529a616f8543f9c32cd99c8197c8f081e/sse.md
- cpp-httplib v0.60.1 release — https://api.github.com/repos/yhirose/cpp-httplib/releases/latest
- cpp-httplib streaming API — https://raw.githubusercontent.com/yhirose/cpp-httplib/09d00c099ca828aa31bb1bfbadff2d9f1fd8be98/README-stream.md
- cpp-httplib SSEClient API — https://raw.githubusercontent.com/yhirose/cpp-httplib/09d00c099ca828aa31bb1bfbadff2d9f1fd8be98/README-sse.md
- WHATWG HTML Standard, Server-sent events (9.2) — https://html.spec.whatwg.org/multipage/server-sent-events.html
- Boost 1.92.0 tag — https://api.github.com/repos/boostorg/boost/tags?per_page=5
- Boost.Asio async model overview — https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/overview/core/async.html
- nlohmann/json v3.12.0 release — https://api.github.com/repos/nlohmann/json/releases/latest
- nlohmann/json changelog — https://raw.githubusercontent.com/nlohmann/json/develop/ChangeLog.md
- simdjson v5.0.4 release — https://api.github.com/repos/simdjson/simdjson/releases/latest
- simdjson basics / padded_string / On-Demand lifetime rules — https://raw.githubusercontent.com/ov-l/simdjson/master/doc/basics.md
- FTXUI v7.0.3 release — https://api.github.com/repos/ArthurSonzogni/FTXUI/releases/latest
- FTXUI README (no dependencies, cross-platform) — https://raw.githubusercontent.com/ArthurSonzogni/FTXUI/main/README.md
- FTXUI App/ScreenInteractive API (Loop, Post, PostEvent, RequestAnimationFrame, Exit, PrintAbove) — https://raw.githubusercontent.com/ArthurSonzogni/FTXUI/main/include/ftxui/component/app.hpp
- FTXUI ScreenInteractive alias to App — https://raw.githubusercontent.com/ArthurSonzogni/FTXUI/main/include/ftxui/component/screen_interactive.hpp
- FTXUI key-handling example (CatchEvent / Event::input) — https://raw.githubusercontent.com/ArthurSonzogni/FTXUI/main/examples/component/print_key_press.cpp
- FTXUI library research notes (version, architecture) — https://raw.githubusercontent.com/PetarKirov/sparkles/refs/heads/main/docs/research/tui-libraries/ftxui.md
- notcurses v3.0.17 tag — https://api.github.com/repos/dankamongmen/notcurses/tags?per_page=3
- notcurses README (not a source-compatible X/Open Curses implementation) — https://raw.githubusercontent.com/dankamongmen/notcurses/master/README.md
- termbox2 repository — https://github.com/termbox/termbox2
- termbox2 README (tb_init/tb_present/tb_peek_event, single-header, libc-only) — https://raw.githubusercontent.com/termbox/termbox2/master/README.md
- ImTui v1.0.4 README (Dear ImGui on ncurses, partial Windows support) — https://raw.githubusercontent.com/ggerganov/imtui/master/README.md
- Asio 1.38.2 latest stable, standalone vs Boost — https://think-async.com/Asio/
- Asio 1.38.2 tag — https://api.github.com/repos/chriskohlhoff/asio/tags?per_page=5
- vcpkg 2026.07.29 release (2858 ports) — https://api.github.com/repos/microsoft/vcpkg/releases/latest
- Conan 2.33.0 release — https://api.github.com/repos/conan-io/conan/releases/latest
- vcpkg vs Conan comparison and manifest/CMake integration — https://raw.githubusercontent.com/Dr-Sergey/learn_cpp/08279578ce23b1cbf2d3394357620600db618eca/31_Project_Architecture/Use_package_managers_Conan_vcpkg_in_project_architecture.md
- WireMock 3.13.2 release — https://api.github.com/repos/wiremock/wiremock/releases/latest