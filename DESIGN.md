# wsl2-ssh-agent: low-resource re-engineering

Everything here was measured on one machine (see [Environment](#environment)).
Raw outputs are in `bench/results/`. Numbers that could not be measured are
marked **unverified**.

## 1. The original implementation

```
ssh ─unix socket─ wsl2-ssh-agent (Go) ─stdin/stdout via WSL interop─ powershell.exe ─named pipe─ ssh-agent.exe
```

* **Go daemon** (`main.go`, `config.go`, `daemon.go`, `server.go`,
  `repeater.go`, about 700 lines; one dependency: `golang.org/x/sys`). It
  daemonizes by re-executing itself with a pipe on fd 3 and listens on the UNIX
  socket. One goroutine per client puts requests on a single channel. A single
  "server" goroutine handles them strictly in order against one PowerShell
  process, with a 10 s reply timeout and up to 3 PowerShell restarts.
* **PowerShell relay** (`repeater.ps1`, embedded). It starts once at daemon
  start-up and stays resident for the daemon's whole lifetime. For **every
  request** it opens a new `NamedPipeClientStream` to `openssh-ssh-agent`,
  forwards one message and reads one reply. It answers `SSH_AGENTC_EXTENSION`
  itself when the Windows agent is older than 8.9.
* There is also a Linux-side process for the Windows child: WSL interop's
  `/init` relays the child's stdio over hvsocket.

### Where the resources go (measured, `bench/results/orig.txt`)

| Component | Idle | Notes |
|---|---|---|
| Go daemon | RSS 3.8 MB, PSS 3.8 MB, 8 threads | statically linked Go runtime |
| `/init` relay for powershell.exe | RSS 2.4 MB, PSS 0.3 MB | shared pages, mostly |
| **powershell.exe (Windows)** | **WS 94 MB, private WS 40 MB, commit 74 MB, 27 threads** | **about 90% of the total footprint** |
| PowerShell CPU | 656 ms in the 30 s right after a burst of requests; 16 ms per 600 s at steady idle (§7.2) | .NET runtime work after activity (GC/JIT tail) |

Bottlenecks, measured rather than assumed:

1. **The resident PowerShell process** dominates memory, and it's the only
   component using CPU at idle. That's small at steady state, but reaches
   about 2% of a core for tens of seconds after activity.
2. **Request latency, about 8–12 ms per request**, comes from the interpreted
   relay. It reconnects the pipe and writes log lines for every request. Go is
   not the cost here: native relays do the same work in 0.14 ms.
3. **Global serialization.** All clients share one PowerShell, so 8
   concurrent clients see p50 70–80 ms per request.
4. **Start-up.** PowerShell takes 0.4–1.2 s (warm/cold) before the first reply.
5. Binary size: 4.1 MB (2.6 MB stripped), all Go runtime and libraries.

What was *not* a bottleneck: Go's request handling and the memory copies.
The Go daemon's per-request work is a few allocations (`readMessage` builds a
new slice and then appends header and body), which is immeasurable next to
the interop hop.

## 2. Constraints that shape the design

* The Windows agent is reachable **only** through a named pipe. WSL2 can't
  open Windows named pipes, and AF_UNIX interop works only on WSL1. Some
  Windows-side process is therefore unavoidable while a request is in flight.
  The real question is whether that process stays **resident** or exists only
  **per connection**.
* Starting a Windows process through WSL interop has a fixed cost. Measured
  with 30 launches each:

  | Windows image (launched from WSL) | Launch + exit |
  |---|---|
  | 2.5 KB no-CRT C exe, on the Linux FS (tmpfs) | 40–42 ms |
  | same, on the Windows FS (`%LOCALAPPDATA%`) | 36–37 ms |
  | 1.2 MB Go exe, on the Linux FS | **264–559 ms** (Windows reads it over 9P) |
  | 1.2 MB Go exe, on the Windows FS | 39–57 ms |
  | `powershell.exe -NoProfile -Command exit` | 203 ms (warm) |

  So a per-connection helper must be **tiny**, or it must be installed on
  the Windows FS. A Go helper is ruled out unless it's installed on Windows.

## 3. Alternatives evaluated

All the prototypes below were built and run through the same workload
(`bench/run.sh`). The "Linux daemon" prototypes use the per-connection design
from §4, with the C helper.

### 3.1 Linux side (same design, different language)

| Daemon | Binary | Idle RSS | Idle **PSS** | Threads | After load | Idle wakeups (30 s) |
|---|---|---|---|---|---|---|
| Original Go (+PS design) | 4.1 MB (2.6 MB stripped) | 3.8 MB | 3.8 MB | 8 | 4.8 MB | 0 |
| Go, same design, `-s -w` | 2.2 MB | 2.8 MB | 2.8 MB | 5 | **7.9 MB, 16 threads** | 0 |
| Rust (std, `opt-level=z`, LTO, abort, strip) | 320 KB | 2.1 MB | 0.35 MB | 1 | 0.34 MB | 0 |
| **C, glibc dynamic (selected)** | **30 KB** (incl. 6 KB helper) | 1.8 MB | **0.18 MB** | 1 | 0.18 MB | **0** |
| C, glibc static | 958 KB | 0.9 MB | 0.92 MB | 1 | 0.92 MB | 0 |
| Zig | — | — | — | — | — | **not measured** (see below) |

* PSS (proportional set size) is the fair metric. RSS counts shared libc pages
  that every process on the system already maps.
* **Static linking is worse.** It replaces about 0.1 MB of private pages with
  0.9 MB that are unshared. It's 32 times bigger, too.
* The Go version grows under load: goroutine stacks and the heap do not shrink.
* Rust is a reasonable second choice: about twice the PSS of C and ten times
  the binary size, with the same behavior. C wins on both of the top two
  priorities, and the whole daemon is 420 lines that use only libc.
* **Zig: unverified.** Running a freshly downloaded Zig toolchain was refused
  by this environment's permission policy. I'd expect it to land between C and
  Rust (a static Zig binary has no shared libc to amortize), but that was not
  measured.

### 3.2 Windows side

| Helper | Size | Resident footprint (one connection) | Conn. setup p50 | Notes |
|---|---|---|---|---|
| PowerShell (original) | — (system) | WS 94 MB, private WS 40 MB, commit 74 MB | n/a (resident) | 8–12 ms per request |
| Go (`GOOS=windows`, `-s -w`) | 1.5 MB | not measured | **148 ms** | loaded over 9P |
| Rust (std, msvc, rust-lld) | 112 KB | not measured | 58 ms | |
| **C, MSVC, no CRT (selected)** | **6 KB** | **WS 3.3 MB, private WS 276 KB, commit 712 KB, 1 thread**; Linux `/init` 312 KB PSS | **50 ms** | |

The prototype Go and Rust helpers failed some concurrent connects (61–64 of
160). They don't have the pipe-instance retry logic described in §5, which
is unrelated to the language.

### 3.3 Architectures

| Architecture | Idle memory (Linux PSS + Windows private) | Per request | New connection | Concurrency | Complexity |
|---|---|---|---|---|---|
| A. Original: resident PowerShell, serialized | 4.1 MB + 40 MB (74 MB commit) | 8–12 ms | ~9 ms | serialized | medium |
| B. Resident native helper, serialized (PS replaced) | ~0.2 + 0.3 + 0.3 MB ≈ **0.8 MB** (≈1.2 MB commit) | ~0.2–0.7 ms | ~0.5 ms | serialized, or needs multiplexing | medium (needs a framing/mux protocol) |
| C. Warm standby: one pre-spawned helper, daemon proxies | ≈ **0.8 MB** | ~0.2–0.7 ms + 1 proxy hop | ~1 ms | full | high (epoll proxy, buffers) |
| **D. Per-connection helper, socket handed over as stdio (selected)** | **0.18 MB + 0** | **0.14 ms list / 0.7 ms sign** | **~50 ms** | **full, no locking** | **lowest** |

Notes on the other options from the brief:

* **Optimizing the existing Go version.** Replacing the PowerShell relay is
  what matters (~95% of memory). With that done, Go still costs 2.8–7.9 MB
  PSS versus 0.18 MB for C, with no latency benefit.
* **Persistent helpers in general (B, C).** These cost a measured ~0.6 MB of
  private memory (about 1 MB commit) plus a resident Windows process. In
  return they save ~50 ms per connection.
* **Hyper-V sockets / AF_VSOCK.** These would still need a resident Windows
  listener (and registering a GUID needs admin), so they're no better than B.
* **Zero-copy.** In design D the daemon never touches payload bytes; the
  accepted socket *is* the helper's stdio. The only copies left are the
  unavoidable interop and pipe hops.

## 4. Selected design and why

**D: a C accept loop in WSL plus a 6 KB no-CRT C helper per connection.**

Against the stated priorities:

1. **Idle memory.** Lowest of everything measured: 0.18 MB PSS, and nothing at
   all on Windows. That's about 25× smaller than the original Go daemon
   alone, and about 250× smaller counting PowerShell's private working set.
2. **Deployed size.** 30 KB in total, helper embedded, against 4.1 MB.
3. **CPU.** The daemon is single-threaded and sleeps in `ppoll()` with no
   timeout. It had 0 CPU ticks and 0 context switches over the idle windows
   (§7.2). There's no Windows process to burn CPU at idle.
4. **Latency.** Requests are 15–70× faster than the original (0.14 ms list,
   0.7 ms sign). But **every new connection costs ~40–50 ms** of interop
   process start-up, so a single login is **slower**: real `ssh` login p50 is
   130 ms against 103 ms (+27 ms). Under concurrency the new design is much
   better, because nothing is serialized.
5. **Simplicity.** No protocol between the daemon and the helper, no
   multiplexing, no restart logic, no timeouts. Agent restarts and
   disconnections are handled by construction, because each connection is
   independent.

**The trade-off, stated plainly:** design B/C would remove the +27 ms per
login for about 0.6 MB of resident private memory (≈1 MB commit), one
permanent Windows process, and noticeably more code. Under the stated
priority order (RAM first, latency fourth), plus the instruction not to keep
a persistent helper unless it pays for itself, D is the default. The
warm-standby variant (C) is available as the opt-in `-prespawn` flag; §9 has
its implementation and measurements.

## 5. Implementation notes

`src/wsl2-ssh-agent.c` (Linux, about 420 lines):

* **Event-driven:** a blocking `ppoll()` on a non-blocking listen socket, with
  SIGTERM/SIGINT/SIGHUP/SIGCHLD blocked except *inside* `ppoll`. So there's
  no shutdown race, and no polling or timers.
* `posix_spawn()` (vfork-style in glibc) of the helper with
  `stdin = stdout = accepted socket` and `stderr = log or /dev/null`; then the
  daemon closes its copy. Children are reaped from SIGCHLD and counted. At 64
  concurrent connections, the daemon stops accepting until one finishes, which
  is back-pressure rather than a fork bomb. On EMFILE it waits the same way.
* **Access control:** the socket is 0600 (umask 0177), and every connection
  is also checked with `SO_PEERCRED` (same uid or root).
* **Embedded helper:** `.incbin` puts it into `.rodata`. It's written
  atomically (`O_EXCL` temp file, `fsync`, `rename`) to
  `$XDG_RUNTIME_DIR/wsl2-ssh-agent/` or `~/.cache/wsl2-ssh-agent/`. The
  directory must be owned by the user and not writable by group or others.
  The file is reused only if its owner, mode 0700, size and content all match.
* CLI compatible with the Go version: the same flags in `-x`, `--x`, `-x=v`
  and `-x v` forms; the same shell output formats; the same "already running"
  detection and stale-socket clean-up. `-stop` refuses to signal a server
  owned by a different uid and waits on a pidfd, which is event-driven.
* **Daemonization:** bind in the parent (so errors reach the terminal);
  `listen()` in the forked child, because the kernel records the
  `SO_PEERCRED` pid at `listen()` time and `-stop` depends on it. The parent
  prints the `SSH_AUTH_SOCK=…` line only after the child signals that it
  accepts connections.

`windows/helper.c` (Windows, ~190 lines, kernel32 only, no CRT):

* Synchronous framed relay: read a length-prefixed message from stdin,
  reject length 0 or > 256 KiB (OpenSSH's `AGENT_MAX_LEN`) by closing,
  write it to the pipe, read one framed reply, write it to stdout, repeat.
  Pipelined requests work. EOF on either side ends the process.
* `CreateFileW` uses `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`. A
  process that squats the pipe name can identify the client, but can never
  impersonate it. This was verified to work with Windows OpenSSH 9.5
  (listing and signing).
* **Pipe-instance races** (found by the concurrency benchmark).
  `ssh-agent.exe` creates its next pipe instance only after accepting a
  client, so concurrent connects see both `ERROR_PIPE_BUSY` and transient
  `ERROR_FILE_NOT_FOUND`. BUSY is handled by `WaitNamedPipe`, which is
  event-driven. NOT_FOUND gets a bounded backoff (1, 2, …, 256 ms, about
  0.5 s in total) before the helper concludes that the agent isn't running.
  The overall limit is 10 s. Result: 0 failures in 832 connections at 32-way
  and 64-way concurrency (there were 3–20 failures before the fix).
* **Old-agent compatibility** (Windows OpenSSH < 8.9). On the first
  `SSH_AGENTC_EXTENSION` of a connection, the helper looks up `ssh-agent.exe`
  on `PATH` and reads its file version (version.dll is loaded only then). If
  the agent is old or missing, the helper replies `SSH_AGENT_FAILURE` itself.
  Two deliberate changes from the original: the version test is
  `major < 8 || (major == 8 && minor < 9)` (the original's
  `major <= 8 && minor < 9` misclassifies 7.9 and earlier `x.9` releases), and
  the reply is FAILURE (as the protocol requires for an unsupported extension)
  rather than SUCCESS. **Unverified against a real < 8.9 agent**: none was
  available.

Private key material never passes through the daemon, and nothing is
persisted. The helper holds one message at a time in a static buffer, and
signing happens inside `ssh-agent.exe`.

## 6. Build configuration (measured; `make` uses the "shipped" row)

| Linux build | File size |
|---|---|
| `-O2` | 36,592 B |
| `-O2 -s` | 31,536 B |
| `-Os -s`, `+ --gc-sections`, `+ -fno-unwind-tables`, `+ -flto` | 31,536–31,544 B (text 20.4 → 20.0 KB) |
| **shipped: `-Os`, gc-sections, no unwind tables, PIE, full RELRO, stack protector, FORTIFY, strip** | **30,920 B** |
| shipped `+ -z noseparate-code` | 26,744 B (*rejected*: makes `.rodata`, including the embedded helper, executable) |
| static glibc | 981,184 B (*rejected*: bigger and higher PSS) |

* File size is mostly ELF segment alignment, so LTO and gc-sections change
  text by about 2% and the file not at all. The hardening flags cost nothing
  measurable, so they stay.
* Alternative allocators: not applicable. The daemon makes no heap
  allocations after start-up.
* Helper: MSVC `/O1 /Os /GS- /Gs999999`, `/NODEFAULTLIB /ENTRY:start`,
  `/OPT:REF /OPT:ICF /MERGE:.rdata=.text`, giving 6,144 B. The equivalent std
  Rust helper is 112 KB, and a Go one is 1.5 MB. `/GS-` is acceptable because
  the helper has no stack arrays; its single buffer is static, and every read
  is bounds-checked against `MAX_MSG`.
* mingw-w64 build (`make WINCC=x86_64-w64-mingw32-gcc`, used by CI):
  **unverified locally.** mingw-w64 isn't installed here and installing it
  needs sudo. MSVC through interop (`windows/msvc.sh`) is what was tested.

## 7. Benchmarks

### Environment

Intel i7-13620H; Windows 11 (NT 10.0.26200) with Windows OpenSSH 9.5.6.4 and
one ED25519 key; WSL 3.0.3.0 with kernel 6.18.54.1; Arch Linux, glibc 2.44,
gcc 16.2.1, Go 1.27.3, rustc 1.99.1, MSVC 14.40; OpenSSH client 10.6p1. The
machine was in normal desktop use, so absolute numbers carry noise of roughly
±10%.

### Method

* `bench/run.sh NAME IMAGE 'CMD'` starts the agent in the foreground and
  measures the time to its first `ssh-add -l` reply. After 5 s it samples
  memory: Linux RSS/PSS/threads for the whole process tree from
  `/proc/*/smaps_rollup`, and Windows working set, private working set,
  commit and threads through `bench/winmem.ps1` (CIM and `Get-Process`;
  pre-existing processes with the same image name are excluded). Then it runs
  the workloads, samples again, and idles for 30 s while measuring CPU ticks
  and context switches.
* `bench/agentbench.c` produces latency percentiles. `list` is
  `REQUEST_IDENTITIES`, `sign` is `SIGN_REQUEST` with the first key, and
  `conn` opens a new connection for every request. All accept a concurrency
  argument.
* `bench/sshauth.sh` times real `ssh … true` logins, 100 per run, against a
  throwaway user-level `sshd` on 127.0.0.1 that accepts only the agent's key.
* `bench/idle.sh` runs both agents side by side for 600 s, sampling only at
  the start and the end.

### 7.1 Before and after

| Metric | Original (Go + PowerShell) | Optimized (C + per-connection helper) | Change |
|---|---|---|---|
| Idle RAM, Linux (PSS) | 4,148 KB (daemon 3,836 + relay 312) | **183 KB** | **−96%** |
| Idle RAM, Windows | WS 94,492 KB / private WS 40,184 KB / commit 74,480 KB | **0** (no resident process) | **−100%** |
| Peak RAM, Linux daemon (VmHWM, after workloads) | 4,816 KB | 1,824 KB RSS (179 KB PSS) | −62% (RSS) |
| Peak RAM, Windows | PS peak WS 119,816 KB | per active connection: WS 3.3 MB, private WS 276 KB, commit 712 KB (+312 KB PSS for the Linux relay) | — |
| Total helper RAM at idle | 40 MB private WS + 0.3 MB relay | 0 | −100% |
| Deployed size | 4,129,114 B (2,601,120 stripped) | **30,920 B** (includes the 6,144 B helper) | **−99.3%** |
| Idle CPU, Linux (30 s / 600 s) | 0 ticks | 0 ticks, 0 context switches | = |
| Idle CPU, Windows | 656 ms in the 30 s after the workload; 16 ms per 600 s at steady idle | 0 (no process) | −100% |
| Startup to first reply | 421–440 ms (1,176 ms cold) | 54–66 ms | **−85%** |
| `list` on one connection, p50 / p95 / p99 | 10.4 / 12.4 / 13.8 ms | **0.14 / 0.21 / 0.29 ms** | **~70× faster** |
| `sign` on one connection, p50 / p95 / p99 | 12.3 / 15.0 / 15.5 ms | **0.74 / 0.90 / 1.29 ms** | **~16× faster** |
| New connection + list, p50 / p95 | 8.5 / 9.9 ms | 51.0 / 57.0 ms | **+42 ms (slower)** |
| 8 clients, a new connection per request, p50 / p95 | 70.4 / 75.4 ms | 61.0 / 90.8 ms | similar |
| 8 concurrent signing clients, p50 / p95 | 80.2 / 86.6 ms | **0.93 / 1.21 ms** | **~85× faster** |
| `ssh-add -l` (process wall time) | 13 ms | 58 ms | +45 ms |
| `ssh-keygen -Y sign` through the agent | 24 ms | 57 ms | +33 ms |
| **Real `ssh` login, p50 / p95 / p99** | **103.1 / 108.4 / 110.5 ms** | **130.1 / 138.0 / 146.0 ms** | **+27 ms (+26%)** |
| Runtime dependencies | Go runtime (static), `golang.org/x/sys`, **powershell.exe**, WSL interop | glibc, WSL interop | PowerShell removed |
| Threads | 8–9 (Linux) + 27 (Windows) | 1 (+1 per active connection on each side) | |

The SSH login rows are two runs of 100 logins each, averaged
(`bench/results/ssh-login.txt`). Everything else is from
`bench/results/{orig,c}.txt`.

### 7.2 Long idle (600 s)

Both agents ran side by side, with one request each, then 600 s untouched
(`bench/results/idle-600s.txt`):

| | Original | Optimized |
|---|---|---|
| Linux CPU ticks / context switches over 600 s | 0 / 0 (Go daemon and `/init` relay) | **0 / 0** |
| Windows CPU over 600 s | 16 ms (powershell.exe) | no process |
| Linux daemon at end: RSS / PSS / **anon** | 1,264 / 1,256 / **1,168 KB** | 1,800 / 160 / **120 KB** |
| `/init` relay | 312 KB PSS | none |
| Windows at end | WS 93.8 MB, private WS 39.8 MB, commit 73.6 MB | 0 |

Over the 600 s, the original Go daemon's RSS fell from 3.8 MB to 1.3 MB. The
kernel reclaimed its file-backed pages because this machine had about 120 MB
free; that memory comes back on the next request. Anonymous memory (1.17 MB
vs 0.12 MB) is the steadier comparison. Neither Linux daemon woke up at all;
Go's sysmon parks when idle.

### 7.3 Functional and regression results

* `tests/daemon.sh`: **23/23 passed.** Linux only, with `cat` standing in for
  the helper; this is what CI runs.
* `tests/run.sh`: **37/37 passed** against the real Windows agent. It covers:
  real `ssh-add` and `ssh-keygen -Y sign` with signature verification;
  pipelined requests; unknown message types and extensions (Windows OpenSSH
  9.5 closes the connection, and the original behaves the same way);
  oversized, zero-length and truncated frames and 300 KB of garbage (closed,
  no hang, daemon unaffected); 500 requests and 200 signatures on one
  connection; 16×10 concurrent connections; 8×50 concurrent signers; 300
  sequential connections with no leftover helpers, no fd leak, no RSS growth
  and no zombies; `-stop`; stale socket after SIGKILL; SIGTERM clean-up;
  reinstalling a corrupted helper; and an unavailable agent pipe (`ssh-add`
  fails cleanly and the daemon survives).
* Stress: 0 failures in 832 connections at 32-way and 64-way concurrency.

**Unverified (needs elevation or would kill the test session):**
* **Windows agent restart** (`Restart-Service ssh-agent` needs an elevated
  shell). By construction every connection opens a fresh pipe, so the next
  connection after a restart should simply work. The validation command is in
  `tests/run.sh`.
* **`wsl --shutdown` and restart.** The daemon dies with the VM. On the next
  start, the stale socket is detected (`ECONNREFUSED`) and replaced, which is
  covered by the SIGKILL test.
* **Old (< 8.9) Windows agent** compatibility path.
* **arm64**, and the **mingw-w64 build**.

## 8. Remaining limitations and possible future work

* **Per-connection start-up cost (~40–50 ms)** in the default mode is the one
  measurable regression: +12 to +27 ms on a real login, +45 ms for
  `ssh-add -l`. `-prespawn` (§9) removes it, for one resident helper.
* `-prespawn` holds a single standby. A burst of new connections arriving
  faster than one per ~40 ms still waits for the next standby to start;
  back-to-back connections measured p50 18.6 ms instead of 48.9 ms, not
  ~1 ms. A pool of N standbys would fix that for N × the idle cost.
* The `-prespawn` relay copies through a 16 KB user-space buffer (one
  `read` + `write` per chunk, about +35 µs per request). `splice()` through a
  pipe would avoid the copy, but agent messages are small and the
  difference wasn't measurable against the interop hop.
* Installing the helper on the Windows FS would save another ~4 ms per
  connection (36 vs 40 ms launch). But it needs the user's Windows profile
  path, which takes a `cmd.exe` launch to discover, plus a Windows-side
  install step.
* The old-agent version check runs once per connection that sends an
  extension message (about 1 ms). It could be cached in the daemon, at the
  cost of a daemon↔helper protocol.
* The interop launch cost depends on the WSL version. Faster interop in
  future WSL releases would directly shrink the one remaining regression.

## 9. `-prespawn`: warm standby (opt-in)

### Design

* At start-up, and after every hand-off, the daemon creates a socketpair and
  starts the helper on one end (`spawn_helper`, the same `posix_spawn` path as
  the default mode). The Windows process is created by WSL interop while
  nothing is waiting for it.
* When a client connects, the daemon `fork()`s a relay process, closes its
  copies of the client socket and the standby end, and immediately starts the
  next standby. The relay `poll()`s both sockets and copies bytes, forwarding
  a client half-close to the helper as `shutdown(SHUT_WR)`, and exits when the
  helper side closes. The accept loop never touches payload and never blocks
  on a slow client; each connection's failure stays inside its own relay.
* The helper now opens the agent's pipe when the **first message** arrives
  (both modes). A standby may wait for hours, and it must not be holding a
  pipe connection that went stale because `ssh-agent.exe` restarted in the
  meantime.
* **Launch handshake.** WSL interop occasionally fails to start a Windows
  process at all: `/init` logs `UtilAcceptVsock:281: accept4 failed 110`
  (ETIMEDOUT) to the helper's stderr and the socketpair just reports EOF.
  In the default mode that ends only the one connection the helper was
  started for. A standby, though, is started *before* its client, so before
  this fix the client that got a never-started standby was dropped
  (`ssh-add` saw EOF). This was the intermittent `PRESPAWN=1 tests/run.sh`
  failure ("stale socket", once "300 sequential connections"); the daemon
  log of the failing run held that `/init` line.
  So standbys are started as `helper.exe --ready PIPENAME` and write one
  `0xff` byte as soon as the Windows process runs, before touching the pipe.
  The daemon reads that byte; until it arrives the standby only *might*
  exist. A client that gets a standby which is not ready yet is handled by
  the relay: it holds the client's bytes (up to 16 KB, then it stops
  reading, so nothing is lost), and if the helper exits, times out (10 s) or
  sends anything but `0xff`, it starts a fresh `--ready` helper and replays
  them, up to 3 launches. Replay is safe because nothing can have reached
  the agent before the ready byte. After the ready byte nothing is replayed:
  a helper that dies mid-request ends that connection, as in the default
  mode. The relay names itself `wsa-relay` (`prctl`), so it is not mistaken
  for the daemon in `ps`/`top`.
* An idle, ready helper never writes again, so a readable or hung-up standby
  socket means the helper exited. If it died within 2 s of starting, the
  restart is delayed (100 ms, doubling, capped at 60 s); this is the only
  timer in the daemon, and only while a restart is pending. Meanwhile, and on
  `fork()` failure, connections fall back to the default per-connection path.
  The standby exits on its own when the daemon exits (EOF on its stdin).
* The connection limit counts processes, so it is 2 × 64 + 1 with
  `-prespawn` (relay + helper per connection, plus the standby).

### Results

Same binary for both modes, same machine and session (`bench/results/c.txt`,
`c-prespawn.txt`, `ssh-login-prespawn.txt`). The SSH logins ran
interleaved, default/prespawn × 3 runs × 100 logins, plus one run of the
original for reference.

| Metric | Default | `-prespawn` | Original (reference) |
|---|---|---|---|
| **Real `ssh` login p50** (mean of 3 runs) | 119.3 ms | **96.0 ms** | 107.1 ms |
| Real `ssh` login p95 | 127.3 ms | **101.4 ms** | 112.2 ms |
| Real `ssh` login p99 | 134.9 ms | **104.7 ms** | 119.4 ms |
| `ssh-add -l` (process wall time) | 60 ms | 22 ms | 13 ms |
| `ssh-keygen -Y sign` through the agent | 58 ms | 23 ms | 24 ms |
| New connection + list, back-to-back, p50 / p95 | 48.9 / 54.4 ms | 18.6 / 27.8 ms | 8.5 / 9.9 ms |
| `list` on one connection, p50 | 141 µs | 176 µs (relay hop) | 10.4 ms |
| `sign` on one connection, p50 | 707 µs | 742 µs | 12.3 ms |
| 8 concurrent signers, p50 | 910 µs | 944 µs | 80.2 ms |
| Idle RAM, Linux (PSS) | **188 KB** | 496 KB (daemon 188 + standby `/init` 308) | 4,148 KB |
| Idle RAM, Windows | **0** | WS 3,296 KB, private WS 280 KB, commit 720 KB, 1 thread | WS 94,492 KB, private 40,184 KB, commit 74,480 KB |
| Idle CPU / wakeups (60 s) | 0 / 0 | 0 / 0 (Windows 0 ms) | — |
| Startup to first reply | 55 ms | 61 ms | 421–440 ms |
| Binary | 30,928 B | (same binary) | 4,129,114 B |

Reading the numbers:

* **Login latency.** `-prespawn` takes 23 ms off p50 and 26 ms off p95
  compared with the default, and is now about 11 ms *faster* than the
  original as well. In this session the default mode's penalty against the
  original was +12 ms p50 (the first session measured +27 ms). The
  run-to-run spread is about ±5 ms, so treat the default-vs-original gap as
  "10–30 ms".
* **What it costs.** One resident helper: +308 KB PSS on Linux and +280 KB
  private working set (720 KB commit) on Windows, with no CPU or wakeups at
  idle. That is roughly 0.6 MB private, about 1/70 of what the original's
  PowerShell kept resident.
* **Where it doesn't help.** Requests on an already-open connection
  (+35 µs for the relay hop), and bursts faster than the ~40 ms standby
  restart (see §8).

**Size cost of the launch handshake** (added after the measurements
above): the helper is 6,656 B (+512 B, one PE file-alignment unit, for the
`--ready` parsing) and the Linux binary 35,024 B (+4,096 B). The new code
itself is about 1.4 KB (.text +735 B, .rodata +691 B incl. the helper), but
.text crossed a 4 KB page boundary. Linking with `-z noseparate-code` would
get back to 30,928 B, but it maps .rodata, including the embedded helper
bytes, executable together with the code, so it is not used. The latency and
memory numbers are unaffected: the daemon reads one extra byte per standby.

**Recommendation:** keep per-connection as the default (lowest footprint,
matching the stated priorities), and use `-prespawn` if you open many short
SSH connections interactively (git over SSH, `ssh` in loops) and the
~0.6 MB is irrelevant to you.

### Tests

`tests/daemon.sh` has 13 `-prespawn` cases with a stand-in helper: one
standby while idle; half-close relayed; a 200 KB round trip; 32 concurrent
clients; exactly one standby left after 200 connections; a killed standby
is replaced and the next connection is served; zero idle wakeups; the
standby exits with the daemon; a standby whose launch fails before the
ready byte is retried and the client's request replayed; no replay once the
helper was ready; a helper that dies immediately backs off (16 context
switches in 3 s, no spin); and the fallback to the direct path with no hang.
`PRESPAWN=1 tests/run.sh` runs the whole 37-test end-to-end suite with
`-prespawn` and reports any launch failures seen in the daemon logs.

The tests now find the daemon's PID with `ss -xlp` (the only process holding
the listening socket) instead of `pgrep -f`, because relay processes are
forks of the daemon and share its command line.

After the fix, `PRESPAWN=1 tests/run.sh` passed 20 times in a row (740/740
tests; before it, one run in about 20 failed). None of those 20 runs hit an
interop launch failure, so on the real system the retry path was not
exercised; it is covered by the stand-in helper cases above.

## 10. Memory-safety verification

Checked after a code review of both C files:

| Check | Daemon (`src/`) | Helper (`windows/`) |
|---|---|---|
| Review findings fixed | `SO_PEERCRED` failure no longer logs an uninitialised uid; `stat()` of the new socket is checked; paths built from `HOME`/`XDG_RUNTIME_DIR` now fail instead of being silently truncated (`pathf`); signal handlers are installed before `listen()` (previously a SIGTERM or `-stop` right after the start snippet was printed could kill the daemon with the default action and leave a stale socket) | — |
| ASan + UBSan (`make sanitize`) | `tests/daemon.sh` 36/36, `tests/run.sh` 37/37, `PRESPAWN=1 tests/run.sh` 37/37; **no sanitizer reports**; LeakSanitizer clean at every daemon exit | — |
| Randomized framing tests (`make fuzz`) | — | helper.c built on Linux against a Win32 shim (`tests/fuzz/`), ASan + UBSan, **40,002 runs, 0 problems**. Echo agent: output must equal the exactly expected frames (every 4th seed runs as a `--ready` standby and must emit `0xff` first). Hostile agent: random and malformed replies. Missing and permanently busy pipe must give up, on a virtual clock. |
| Harness sensitivity | — | Planted bugs are caught: length check `> MAX_MSG + 16` gives 43 overflow reports, and `> MAX_MSG + 1` (off-by-one) gives 12, in 1,200 runs |
| Static analysis (`make analyze`) | `gcc -fanalyzer`, **0 warnings** (`-Werror`) | **unverified**: gcc 16.2.1's analyzer hits an internal compiler error on helper.c (at -O0 and -O1); MSVC `/analyze` not run |
| valgrind 3.25.1 memcheck (`make valgrind`) | `tests/daemon.sh` 36/36 on a `-O1 -g` build under valgrind, every forked process traced (855 daemon/relay processes): **0 errors**, no definite/indirect leaks. `--track-fds=yes` is not used because it counts fds still open at exit (stdio, log) as errors; fd leaks are covered by the fd-count test. | Same generator/oracle as the fuzzer, without ASan: **600 runs, 0 errors** (uninitialised reads included) |
| cppcheck 2.22.0 (`warning,style,performance,portability`) | **0 findings** (daemon, sockcat, agentbench). Fixed on the way: the helper's size symbol is now emitted by the assembler instead of subtracting two unrelated pointers; unchecked `malloc`/`fopen` in test and bench tools. | **0 findings** through the shim; one `knownConditionTrueFalse` suppressed, caused by the shim's constant stubs |

* **RSS under ASan.** With the sanitizer, the daemon's RSS rises about
  280–420 bytes per connection, with or without the quarantine. The shipped
  build stays flat at 884 KB across 3,000 connections in both modes, and LSan
  reports no leaks, so this is sanitizer bookkeeping rather than a leak. The
  tests skip the RSS check for sanitizer builds and keep it for real builds.
* **`/GS-` on the helper.** The helper is built without stack cookies
  because `/GS` needs the CRT's cookie initialisation, which `/NODEFAULTLIB`
  drops. Its only stack arrays are fixed-size and bounded: `path[MAX_PATH]`
  is written by `SearchPathW` with its size, and `d[12]` holds a decimal
  `DWORD`. The message buffer is static and every read is checked against
  `MAX_MSG` before use. The fuzz harness now guards that check, but there is
  no compiler safety net if a future change adds a stack buffer.
* CI runs `make sanitize`, `make fuzz N=5000` and `make analyze` alongside
  the daemon tests.

