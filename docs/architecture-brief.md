# Architecture of Modern LLM Coding Agents & Agent Harnesses

## 1. Process Model: Single-Process vs. Core/Daemon + Thin Client

**The short answer: the industry is converging on a core/daemon + thin-client split. Codex CLI is the most explicit implementation; Gemini CLI shipped it as a package boundary from day one; Claude Code is a monolithic Node CLI that emulates the split internally.**

### Codex CLI (Rust) — explicit daemon + client, and it is the strongest evidence
The `codex-rs` Cargo workspace is a ~200-crate workspace whose member list is itself an architectural statement. The relevant crates:
- `app-server` — the long-lived JSON-RPC server owning threads/turns
- `app-server-daemon` — process/lifecycle manager for the app-server ("backs the machine-readable `codex app-server` lifecycle commands used by remote clients such as the desktop and mobile apps"; supports Linux/macOS/Windows; `start|restart|update|stop|enable-remote-control`)
- `app-server-client` — "Shared in-process app-server client facade for CLI surfaces... used by surfaces like TUI and exec"
- `app-server-protocol` / `app-server-transport` / `app-server-test-client`
- `uds` — cross-platform async Unix-domain-socket helpers; `stdio-to-uds` — adapter bridging UDS ↔ stdio so an MCP server on `/tmp/mcp.sock` can be attached as a stdio MCP server
- `tui`, `cli`, `exec-server`, `responses-api-proxy`

Two decisive details:
1. **The client facade is transport-agnostic.** `app-server-client` exposes both `InProcessAppServerClient` and `RemoteAppServerClient`. The TUI can run the core in-process (embedded) *or* attach to a remote/local daemon over a socket. The same JSON-RPC envelope is used on both paths — the crate docs state that even on the in-process path, "successful responses still travel back through the same JSON-RPC result envelope used by socket/stdio transports."
2. **Graceful fallback is designed in.** Per `app-server-daemon/README.md`: "If an implicitly discovered daemon cannot initialize the connection, the TUI starts an embedded server instead." Explicit `--remote` endpoints are authoritative and report failures. Attachment is also skipped when `CODEX_EXEC_SERVER_URL` is set, to preserve executor selection.
3. **Security posture of the socket.** `uds` restricts the socket directory to the user (0700 on Unix, protected user-only DACL on Windows), detects stale rendezvous paths, and on Windows offers `ensure_non_elevated_peer()` — requiring the peer to belong to the current user with **neither process elevated** before any application data is sent.

Hard operational constraints documented for the daemon:
- Windows AF_UNIX path limit is **108 bytes including terminator**; a junction alias that resolves longer falls back to the embedded server.
- **"Shared clients use the environment inherited when the daemon started"** — opening a new terminal does not clear the running daemon's env; **per-client environment isolation is not provided**. This is the single sharpest downside of daemonization, stated by the vendor.
- Daemon updates restart the process, so "active or queued work may be interrupted."

### Gemini CLI (TypeScript) — package-boundary split
Two npm packages: `@google/gemini-cli-core` and `@google/gemini-cli` (the `gemini` bin). The CLI depends on `@google/gemini-cli-core` via `file:../core`. Core owns the agent loop (`GeminiClient`), tool registry, context/compression, sandbox manager, MCP client, and `node-pty`. The CLI owns Ink/React UI and depends on `@agentclientprotocol/sdk` — i.e. Gemini CLI implements **ACP**, so it can be driven as a subprocess agent by any ACP client (Zed, JetBrains). Note this is a *library* split within one process by default, not a daemon; the ACP surface is what enables external thin clients.

### Claude Code — monolithic Node CLI
Anthropic's repo is docs/plugins-only; the product is a distributed Node binary (npm `@anthropic-ai/claude-code`, now normally installed via `install.sh`/`install.ps1`/Homebrew/WinGet). There is a single `claude` process. The "harness vs UI" split exists **as an internal module boundary**, not a process boundary — the same process renders the TUI, runs the loop, executes tools, and owns MCP connections. Multi-frontend access is achieved by *adjacent* surfaces (VS Code/JetBrains extensions, desktop app, web, Slack, Chrome extension, Remote Control, Agent SDK) rather than by a shared daemon.

### OpenHands — multi-process *by design*, but the split is server↔sandbox
OpenHands is not a TUI at all. Its documented backend architecture has: Frontend (SPA) → Backend (EventStream, Storage, Runtime Interface, LLM) → Runtime (Docker/Local/Remote) → **Action Execution Server** → Bash Session / Jupyter Plugin / BrowserEnv. The runtime client speaks REST to the action-execution server inside the sandbox. In Enterprise form this becomes an app server + Runtime API + one sandbox pod **per conversation** + Postgres/Redis/object store, orchestrated by Kubernetes. So OpenHands is the "control plane / data plane" version of the split: the agent loop lives *inside* the sandbox, the orchestration lives in the server.

### Aider — strictly single-process
`aider/main.py` wires a `Coder` (in-process), `InputOutput` (prompt_toolkit), `GitRepo`, `RepoMap`, `ChatSummary`, and optionally a Streamlit browser UI launched as a subprocess. There is no core/daemon. The loop and the UI are the same Python process.

### Cursor — closed-source, but the harness is a named, versioned artifact
Cursor's engineering blog treats "the harness" as the product surface being iterated independently of models — "we spend weeks customizing our harness to a model's strengths and quirks." That framing implies harness/model separation internal to their stack; the process topology is not published.

### The "harness vs UI" split, concretely
The clean decomposition that all of these approximate:
- **Harness (core)**: message history, agent loop, model transport, tool registry + dispatch, permission policy, compaction, session/transcript store, MCP clients, subagent spawning. Knows nothing about rendering.
- **UI (frontend)**: input, streaming render, diff display, approval prompts, scrollback. Knows nothing about tool semantics.
- **The contract between them**: a typed event/request stream (JSON-RPC in Codex, ACP in Gemini/Zed, internal callbacks in Claude Code/Aider).

Codex's `core/src/lib.rs` codifies the discipline at the compiler level: `#![deny(clippy::print_stdout, clippy::print_stderr)]` — "Prevent accidental direct writes to stdout/stderr in library code. All user-visible output must go through the appropriate abstraction (e.g., the TUI or the tracing stack)." `tui/src/lib.rs` repeats the same lint. **This is the mechanical enforcement of the harness/UI boundary** and is worth copying: a core library that physically cannot print, plus a client that physically cannot call the model directly.

### Concrete benefits / drawbacks of daemon + client

| Benefit | Evidence / mechanism |
|---|---|
| Session persistence across UI restarts | Codex `thread/list`, `thread/read`, `thread/resume`, `thread/fork` exist as RPCs precisely because the server outlives the client |
| Multiple frontends, one session | Codex daemon explicitly supports "remote clients such as the desktop and mobile apps" plus TUI and `exec`; ACP gives Gemini an IDE frontend for free |
| Hot reload / update without killing work | `codex app-server daemon update` replaces the binary; **but restarts the daemon** (see drawbacks) |
| Crash isolation | A TUI segfault doesn't lose the transcript; the daemon keeps the thread |
| Uniform in-process and out-of-process paths | Codex reuses the identical JSON-RPC envelope in-process, so there is no second code path to keep correct |
| Security boundary | UDS + user-only DACL/0700 + non-elevated-peer check is a real capability boundary that an in-process design cannot express |
| IPC overhead | Named as a cost, but small: local UDS/NP round-trip is microseconds vs. multi-second model latency. Non-issue in practice |
| Debugging complexity | Real and significant: you now debug two processes, attach to an async worker queue, and reason about ordering. Codex's client crate interposes a worker task and warns about bounded command channels vs. an **unbounded local consumer queue** "so unread notifications cannot prevent request responses" — that is exactly the class of bug the split introduces |
| State/env duplication | The documented "shared clients use the environment inherited when the daemon started" — no per-client env isolation |
| Lifecycle/zombie management | Requires stale-socket detection, restart/update flows, grace periods (`shutdownGraceSeconds`, default 60), shutdown admission errors (`-32600 {reason:"serverShuttingDown"}`) |
| Platform limits | Windows 108-byte AF_UNIX cap forces fallback paths |

**Verdict for a C++ implementation:** start in-process (one binary, clean library boundary enforced by build rules), but define the core API *as an event/request protocol* from day one and keep it transport-neutral. Add the daemon later; it is a transport swap, not a rewrite — that is exactly how Codex's `InProcessAppServerClient` / `RemoteAppServerClient` pair works.

## 2. The Canonical Agent Loop

**Origin.** ReAct (Yao et al., arXiv:2210.03629) established the interleaving of reasoning traces and task-specific actions: "reasoning traces help the model induce, track, and update action plans as well as handle exceptions, while actions allow it to interface with external sources." On ALFWorld/WebShop it beat imitation and RL baselines by 34% and 10% absolute success rate with 1–2 in-context examples.

**Modern form = ReAct + native tool calling.** Instead of parsing a text `Action:` line, the loop uses the provider's structured tool-call API and appends a typed tool-result message.

**Roles.** Three, structurally:
- `system` — instructions, tool schemas, environment info. Written once, cached.
- `user` — human turns *and* tool results (in most providers tool results are `user`-role or a dedicated `tool` role).
- `assistant` — text plus zero or more tool-call requests.

**The loop, precisely:**
1. Assemble context: system prompt + tool schemas + pruned history + new user turn.
2. Stream a model response.
3. If the response contains tool calls: execute them (sequentially or in parallel), serialize each result back into the transcript, goto 2.
4. Else: the turn ends; yield control to the human.

**Streaming.** Gemini CLI's `GeminiEventType` enum is the cleanest public taxonomy of loop events: `Content`, `Thought`, `ToolCallRequest`, `ToolCallResponse`, `ToolCallConfirmation`, `UserCancelled`, `Error`, `ChatCompressed`, `MaxSessionTurns`, `Finished`, `LoopDetected`, `Citation`, `Retry`, `ContextWindowWillOverflow`, `InvalidStream`, `ModelInfo`, `AgentExecutionStopped`, `AgentExecutionBlocked`. Note the loop-control events: an explicit turn cap (`MAX_TURNS = 100` in `client.ts`), a **loop detector**, and separate "stopped" vs "blocked" hook outcomes.

**Feeding tool results back.** Codex's `format_exec_output_for_model` shows the discipline: results are formatted as `Exit code: N` / `Wall time: X seconds` / `Total output lines: N` / `Output:` + truncated body. Truncation is centralized (`TruncationPolicy`, `codex-utils-output-truncation`) and applied **before** serialization for model consumption. Timeouts are surfaced as an explicit prepended message, not a silent empty result.

## 3. Core Components of a Harness

**Context management.** Anthropic's "Effective context engineering for AI agents" (Sep 2025) is the canonical framing: context is "a finite resource with diminishing marginal returns," and **context rot** — degraded recall as tokens grow — "emerges across all models." Claude Code publishes a token-level budget for its own startup context (illustrative, from the interactive context-window page): system prompt ~4,200 tokens; auto-memory (`MEMORY.md`, first 200 lines or 25KB) ~680; environment info ~280; **MCP tool names deferred** ~120 (schemas load on demand via tool search); skill one-line descriptions ~450; user `~/.claude/CLAUDE.md` ~320; project `CLAUDE.md` ~1,800. Explicit guidance: "Keep it under 200 lines." Cursor's blog documents the same arc — early static context (folder layout, semantic code snippets, compressed attached files) is "mostly long gone," replaced by dynamic agent-fetched context as models improved.

**Compaction / summarization.** Two distinct strategies, both present:
- *Summarize*: Gemini CLI's `ChatCompressionService` with `DEFAULT_COMPRESSION_TOKEN_THRESHOLD = 0.5` (compress at 50% of the model's token limit), `RECENT_TURNS_PROTECTED = 3` turns kept at full fidelity, a `RETRIEVAL_TOOL_NAMES_EXEMPT_FROM_COLLAPSE` set (read_file, read_many_files, grep, rip_grep, glob, read_mcp_resource, get_internal_docs) that must not be collapsed, and `COLLAPSED_FUNCTION_RESPONSE_MAX_BYTES = 2048` for older turns.
- *Reset*: Codex's `compact_token_budget.rs` — "Token-budget compaction **skips model/server summarization and installs a fresh context window** instead. It is still modeled as compaction so compact hooks and `ContextCompaction` turn items observe the same lifecycle." Triggers: `Manual` and `Auto`, wrapped by **pre-compact and post-compact hooks** that can abort the turn (`CodexErr::TurnAborted`).

**Token budget.** `tokenLimit`, `calculateRequestTokenCount`, `estimateTokenCountSync` (Gemini); `compact_token_budget`, `rollout_budget`/`RolloutBudgetReminder` (Codex). Note Claude Code's `noSurviveCompact` flag on skill descriptions: the listing is **not** re-injected after `/compact` — only skills actually invoked are preserved. Post-compaction re-injection is a distinct design decision with real cost.

**Permission / approval.** MCP's own spec mandates it: "there **SHOULD** always be a human in the loop with the ability to deny tool invocations," with UI confirming which tools are exposed and prompting before operations. Codex implements this as a layered policy: `exec_policy.rs` with a `Policy`/`PolicyParser`/`Decision`/`MatchOptions` engine, `AskForApproval` modes (including `Granular.sandbox_approval` and `Granular.rules`), rule files (`CODEX_HOME/rules/*.rules`, `default.rules`), an explicit list of **banned command prefixes** that cannot be allow-listed (`bash -c`, `sh -lc`, `python -c`, `powershell -EncodedCommand`, `node -e`, `sudo`, `rm`, `cmd /c`, ...), and approval **amendments** that persist a new allow-prefix rule. Error strings are literal and diagnostic: "approval required by policy, but AskForApproval is set to Never." Claude Code exposes the complementary design: the sandbox is **on by default off**, and when on, OS-enforced limits let it "run sandboxed commands without asking you to approve each one" — i.e. **sandboxing is a way to reduce approval prompts**.

**Tool registry.** Gemini's `ToolRegistry` builds `@google/genai` `FunctionDeclaration`s, wraps MCP-discovered tools (`DiscoveredMCPTool`, `DISCOVERED_TOOL_PREFIX`), handles legacy aliases, routes per-invocation approvals through a `MessageBus`, and exposes `shouldConfirmExecute()` alongside `execute()`. Codex's `tools/mod.rs` has `registry`, `router` (`ToolRouter`), `orchestrator`, `parallel`, `sandboxing`, `approvals`, `lifecycle`, plus **namespaced tool names** (`ToolName { namespace, name }`) with a `flat_tool_name()` bridge only for legacy boundaries. Codex also models tool *modes*: `Direct` vs `CodeMode` (tools invoked from a code-execution host) with an in-process fallback.

**Subagents.** Claude Code ships "Create custom subagents," "Agent view," "Run agent teams," "Cross-session messaging," "Dynamic workflows," and "Isolate sessions with worktrees" — i.e. subagents are isolated *sessions*, optionally in separate git worktrees, with a defined messaging channel. Codex has `core/src/agent/` with `AgentControl`, `SpawnRequest`, `SendRequest`, `AgentExecutionGuard`, `MessageDeliveryMode`, `SpawnAgentForkMode`, plus `agent-message-board` and `agent-graph-store` crates.

**Skills.** The Agent Skills open standard (`agentskills.io`): a `SKILL.md` file that "loads only when it's used, so long reference material costs almost nothing until you need it." Claude Code extends it with invocation control (`disable-model-invocation: true` keeps a skill entirely out of context until `/name` is typed), subagent execution, and dynamic context injection; custom commands (`.claude/commands/*.md`) have been merged into skills. Codex has a `skills` crate and `ext/skills`.

**MCP.** Client-host-server. The host "creates and manages multiple client instances," one client per server with a **1:1 stateful session**; servers "should not be able to read the whole conversation, nor 'see into' other servers." Capability negotiation on `initialize` (`roots`, `sampling`, `elicitation` client-side; `prompts`, `resources`, `tools`, `logging`, `completions` server-side), then an `initialized` notification. Tools are "**model-controlled**"; `tools/list` (paginated) and `tools/call`, with `notifications/tools/list_changed`. Tool annotations "**MUST** be considered untrusted unless they come from trusted servers." Claude Code's concrete scaling trick: MCP tool **names** are cheap (~120 tokens) and full **schemas are deferred**, loaded on demand via tool search (`ENABLE_TOOL_SEARCH=auto` loads upfront only if they fit within 10% of the context window). Codex's `stdio-to-uds` lets a long-lived UDS-backed MCP server be attached as if it were stdio.

**Sessions / transcripts.** Codex: `rollout`, `rollout-trace`, `history`, `thread-store`, `state`, `thread-manager-sample` crates; RPCs `thread/list` (with filters: source, provider, archive, project, section, cwd; `excludedThreadIds` capped at 100; cursor pagination capped at 100), `thread/read`, `thread/resume`, `thread/fork`, and experimental `readState` (`{firstUnread, revision}`) with optimistic-concurrency updates and `-32600 {reason:"readStateConflict"}`. Gemini: "Conversation checkpointing to save and resume complex sessions" + `ChatRecordingService`/`ResumedSessionData`.

**Sandboxing.** Claude Code: OS-enforced boundary around shell commands only — writes default to the working dir + per-user temp; reads default to *most of the machine including `~/.ssh` and `~/.aws/credentials`* unless `filesystem.denyRead`/`credentials` restrict them; network has **no direct route out**, going through a local proxy checking `network.allowedDomains` (starts empty). Built on the open-source `@anthropic-ai/sandbox-runtime`. **Crucially documented: "Built-in file and web tools, MCP servers, and hooks run outside it."** And on native Windows it is unsupported — you must run inside WSL2. Codex: `sandboxing`, `linux-sandbox`, `windows-sandbox-rs`, `windows-sandbox-service`, `bwrap`, `mxc-sandbox`, `process-hardening`, plus a `network-proxy` with per-domain and per-UDS-socket permissions.

**Hooks.** Claude Code has a full hook lifecycle with locations/frontmatter resolution; Gemini has `BeforeAgent`/`AfterAgent` hooks returning `AgentExecutionStopped` vs `AgentExecutionBlocked`, and `PreCompressTrigger`; Codex has a `hooks` crate plus `PreCompactHookOutcome`/`PostCompactHookOutcome`. Hooks are the extension seam that avoids forking the harness.

## 4. IPC Options for a C++ Implementation

**JSON-RPC 2.0** is the universal choice here — MCP, ACP, LSP, and Codex's app-server all use it. It is explicitly transport-agnostic: "the concepts can be used within the same process, over sockets, over http, or in many various message passing environments." Three message kinds: Request (`id` + `method` + `params`), Response (`result` XOR `error`), Notification (no `id`). Error codes: `-32700` ParseError, `-32600` InvalidRequest, `-32601` MethodNotFound, `-32602` InvalidParams, `-32603` InternalError; `-32000..-32099` reserved for implementation-defined server errors. LSP extends this (`-32002` ServerNotInitialized). Codex uses these codes concretely: `-32602` for bad `excludedThreadIds`, `-32603` when the local state DB can't serve a DB-only query, `-32600` for shutdown admission and read-state conflicts.

**Framing: two real options, both specified.**

*(a) NDJSON — newline-delimited JSON.* Chosen by **MCP stdio** and **ACP stdio**. MCP: "Messages are delimited by newlines, and **MUST NOT** contain embedded newlines. The server **MUST NOT** write anything to its `stdout` that is not a valid MCP message. The client **MUST NOT** write anything to the server's `stdin` that is not a valid MCP message." Logging goes to stderr. ACP repeats this verbatim. JSON-RPC messages MUST be UTF-8.

*(b) Content-Length headers (LSP style).* Header part + content part, "comparable to HTTP," separated by `\r\n`. Header fields are `Name: value`, ASCII-encoded, HTTP header semantics (RFC 7230 §3.2). **`Content-Length` is required** — "The length of the content part in bytes." `Content-Type` defaults to `application/vscode-jsonrpc; charset=utf-8`. "Two `\r\n` sequences always immediately precede the content part of a message." Content is JSON-RPC, UTF-8 (legacy constant `utf8` should be treated as `utf-8`).

**Recommendation for C++:** use **Content-Length framing**. Rationale: (1) it is byte-exact and binary-safe, so a JSON string containing an escaped newline can never desynchronize the stream; (2) you can skip/route a message body without parsing JSON; (3) LSP tooling and mental models transfer; (4) NDJSON's "MUST NOT contain embedded newlines" is a *serializer obligation* that a naive `json.dumps`-style emitter can violate. Cost: ~20 lines of header parsing and a need to handle the reverse-proxy/HTTP-ish confusion. If interop with MCP/ACP clients matters, implement both behind one `IFraming` interface — Codex does exactly this by adding `stdio-to-uds` as an *adapter* rather than changing the protocol.

**Transport choice:**
- **Unix domain socket** (Linux/macOS): pathname-addressed, `SOCK_STREAM` for reliable ordered bytes, or `SOCK_SEQPACKET`/datagram variants for message boundaries. **Enforce directory permissions 0700** and prefer an abstract-namespace or per-user runtime dir. Codex's `prepare_private_socket_directory` is the reference implementation.
- **Windows named pipes**: `\\.\pipe\<name>`, message-mode via `PIPE_TYPE_MESSAGE`, but you still want explicit length framing; ACL the pipe to the owning user SID and, like Codex, **verify the peer is non-elevated and same-user before sending application data**. Implement UDS too — Windows 10 1803+ supports AF_UNIX, though the 108-byte path limit is a real constraint (Codex documents it and falls back to embedded).
- **stdio**: simplest, zero infrastructure, works with `AppContainer`/`sandbox-exec` constraints, but couples lifetimes — the child dies with the parent, and you cannot attach a second frontend. Use for MCP servers and subagents; not for a durable session daemon.
- **Overlapped/async I/O**: on Windows use I/O completion ports; on POSIX use `epoll`/`kqueue` (or `io_uring`). Do **not** block the model-streaming thread on socket writes — Codex's lesson (bounded command channel, unbounded consumer queue) is that backpressure on the wrong channel deadlocks request/response delivery.

**Additional protocol details worth copying from Codex's app-server:** request `id` as int|string; explicit `initialize` handshake with `clientInfo` and capability flags before any other request (MCP: client "SHOULD NOT send requests other than pings before the server has responded to `initialize`"); server-initiated requests for approvals (`ExecCommandApproval`, `ApplyPatchApproval`) with the client resolving/rejecting them; explicit cancellation notifications; per-request timeouts with reset-on-progress and a hard maximum. Version negotiation: client sends latest supported; if the server doesn't support it, it returns another version it supports; **if the client can't support that, it SHOULD disconnect.**

## 5. Ordered Learning Path for Agent Engineering

The argument for this order is that each rung removes exactly one source of confusion, and you should never debug two unknowns at once (Anthropic: "find the simplest solution possible, and only increase complexity when needed").

1. **Raw model API + streaming.** No agent. Learn messages/roles, streaming deltas, stop reasons, token accounting. *You cannot debug a harness if you don't know what the wire looks like.*
2. **One hand-written tool.** A single function, its JSON Schema, and the round-trip: model emits a call → you execute → you append the result in the correct role → you call again. Stop when you can explain why `isError: true` beats raising an exception.
3. **The loop.** Wrap step 2 in `while`. Add a max-turns cap and a loop detector. This is ReAct in ~50 lines and is the actual heart of every tool listed above.
4. **Context budget + compaction.** Only now does the problem exist. Implement token estimation, a threshold, and one compaction strategy; add "protect the last N turns" and "never collapse retrieval results." Read Anthropic's context engineering post here — it will land because you've felt the pain.
5. **Permissions and sandboxing.** Add an approval gate per tool call, then a policy engine, then (finally) an OS-enforced sandbox so you can *stop* prompting. Learn the difference between prompt suppression and a real boundary: Claude Code's own docs are explicit that file tools, MCP servers, and hooks run outside the sandbox.
6. **Persistence.** Transcripts on disk, resume, fork, list. This is where you feel the case for a core/daemon split.
7. **Tool registry + parallel dispatch + truncation.** Many tools, namespaced, executed concurrently, with centralized output truncation. Note the ordering: parallel dispatch is trivial once you have one tool and hard if you started with twenty.
8. **MCP.** Now that your registry exists, make it pluggable. Implement stdio first (NDJSON), then Streamable HTTP. Learn capability negotiation and why a server must not see the whole conversation.
9. **Hooks.** Add lifecycle seams (pre/post tool, pre/post compact, before/after agent) that can block or inject context without forking your harness.
10. **Skills.** Progressive-disclosure prompt packs: one-line descriptions in context, full body loaded on use, not re-injected after compaction.
11. **Subagents.** Isolated sessions with their own context windows, a spawn/send/message protocol, and worktree isolation. This is the last thing to build because it multiplies every bug you have.
12. **Split the process.** Freeze your core API as an event/request protocol; run it in-process and over a UDS/named pipe behind the same interface; add a daemon lifecycle. Reference design: Codex's `InProcessAppServerClient` / `RemoteAppServerClient` over one JSON-RPC envelope, with an embedded fallback when the daemon can't be reached.
13. **Evaluate.** Cursor's framing is the right close: harness quality needs offline benchmarks *plus* online A/B, and metrics like latency, token efficiency, tool-call count, cache-hit rate, and code **Keep Rate**.

**The one-line thesis:** build the loop in one process and one file; make every extension point (tools, permissions, hooks, skills, subagents) a seam in that single loop; and only then make the seam a socket.

## Sources

Primary specifications
- JSON-RPC 2.0 Specification — https://www.jsonrpc.org/specification
- MCP — Transports (2025-06-18) — https://modelcontextprotocol.io/specification/2025-06-18/basic/transports.md
- MCP — Lifecycle (2025-06-18) — https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle.md
- MCP — Architecture — https://modelcontextprotocol.io/specification/2025-06-18/architecture.md
- MCP — Tools — https://modelcontextprotocol.io/specification/2025-06-18/server/tools.md
- Language Server Protocol Specification 3.17 (Base Protocol / Content-Length framing) — https://raw.githubusercontent.com/microsoft/language-server-protocol/gh-pages/_specifications/lsp/3.17/specification.md
- Agent Client Protocol — Overview — https://agentclientprotocol.com/protocol/overview.md
- Agent Client Protocol — Transports — https://agentclientprotocol.com/protocol/transports.md

Codex CLI (source of record)
- Workspace crate list — https://raw.githubusercontent.com/openai/codex/main/codex-rs/Cargo.toml
- `codex-uds` cross-platform UDS API — https://raw.githubusercontent.com/openai/codex/main/codex-rs/uds/src/lib.rs
- `codex-stdio-to-uds` adapter — https://raw.githubusercontent.com/openai/codex/main/codex-rs/stdio-to-uds/README.md
- `codex-app-server-daemon` README — https://raw.githubusercontent.com/openai/codex/main/codex-rs/app-server-daemon/README.md
- `codex-app-server` README — https://raw.githubusercontent.com/openai/codex/main/codex-rs/app-server/README.md
- `codex-app-server-client` (in-process + remote facade) — https://raw.githubusercontent.com/openai/codex/main/codex-rs/app-server-client/src/lib.rs
- `codex-app-server-protocol` exports — https://raw.githubusercontent.com/openai/codex/main/codex-rs/app-server-protocol/src/lib.rs
- `codex-core` root (no-stdout lint) — https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/lib.rs
- `codex-tui` root — https://raw.githubusercontent.com/openai/codex/main/codex-rs/tui/src/lib.rs
- Context manager — https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/context_manager/mod.rs
- Token-budget compaction — https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/compact_token_budget.rs
- Exec policy / approvals — https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/exec_policy.rs
- Tool registry/router — https://raw.githubusercontent.com/openai/codex/main/codex-rs/core/src/tools/mod.rs
- Codex CLI README — https://raw.githubusercontent.com/openai/codex/main/README.md

Gemini CLI
- `@google/gemini-cli-core` package.json — https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/package.json
- `@google/gemini-cli` package.json (depends on core + ACP SDK) — https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/cli/package.json
- `GeminiClient` agent loop — https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/src/core/client.ts
- Event taxonomy / turn types — https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/src/core/turn.ts
- `ChatCompressionService` — https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/src/context/chatCompressionService.ts
- `ToolRegistry` — https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/src/tools/tool-registry.ts
- Gemini CLI README — https://raw.githubusercontent.com/google-gemini/gemini-cli/main/README.md

Claude Code (Anthropic)
- How Claude Code works — https://code.claude.com/docs/en/how-claude-code-works
- Explore the context window (token budget breakdown) — https://code.claude.com/docs/en/context-window.md
- Sandboxing the Bash tool — https://code.claude.com/docs/en/sandboxing.md
- Skills — https://code.claude.com/docs/en/skills.md
- Hooks reference — https://code.claude.com/docs/en/hooks
- Subagents — https://code.claude.com/docs/en/sub-agents
- MCP / tool search — https://code.claude.com/docs/en/mcp
- Overview — https://code.claude.com/docs/en/overview
- anthropics/sandbox-runtime — https://github.com/anthropics/sandbox-runtime
- Effective context engineering for AI agents (Sep 29, 2025) — https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents
- Building effective agents (Dec 19, 2024) — https://www.anthropic.com/engineering/building-effective-agents

Others
- Aider `main.py` — https://raw.githubusercontent.com/Aider-AI/aider/main/aider/main.py
- Aider `base_coder.py` — https://raw.githubusercontent.com/Aider-AI/aider/main/aider/coders/base_coder.py
- OpenHands Backend Architecture — https://docs.openhands.dev/openhands/usage/architecture/backend.md
- OpenHands Enterprise Architecture — https://docs.openhands.dev/enterprise/architecture.md
- Cursor — Continually improving our agent harness — https://cursor.com/blog/continually-improving-agent-harness
- ReAct: Synergizing Reasoning and Acting in Language Models — https://arxiv.org/abs/2210.03629
- Agent Skills open standard — https://agentskills.io
