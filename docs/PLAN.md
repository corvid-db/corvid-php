# corvid-php — the binding's plan

corvid-php is the **PHP binding** for the `corvid` embedded database. Like its
siblings `corvid-c`/`corvid-go`, it exists to prove, continuously and outside
the engine repo, that corvid's **published FFI artifacts** — the platform
cdylib, `corvid.h`, and the golden fixtures shipped in each release archive —
drive a real consumer to the same verdicts the engine's own suite produces; on
top of that proof it carries the idiomatic PHP API.

Engine repo: `corvid-db/corvid` (read-only upstream; never a submodule, never
vendored). Canonical docs: the corvid docs site's FFI section (the
`docs/FFI.md` contract — 124 symbols at v0.3.4, frozen enums, §8 idiom gate).

## The architecture ruling: a native PHP extension in C over libcorvid

**A native extension (`ext/corvid`, C over the published cdylib), not
`FFI\FFI` and not a Rust-source build.** Why:

- **PHP users install extensions, not toolchains.** The ecosystem's install
  story for native code is PECL/PIE: `pie install corvid/php-corvid` compiles
  a small C extension against a `phpize` toolchain PHP distributions already
  ship, and links the downloaded libcorvid. An `FFI\FFI` binding needs no C
  compiler but is second-class in production: FFI is disabled or restricted
  in many shared hosts and FPM pools (`ffi.enable` defaults to "preload"),
  its binding declarations are a stringly-typed parallel surface the IDE
  cannot type-check, its per-request parse cost is real, and it forfeits the
  ability to expose real PHP classes with proper object handlers and
  exceptions. A compiled extension is the format the ecosystem trusts for
  database drivers (redis, mongodb, pdo_*).
- **The C ABI is the engine's locked surface (FFI.md §8).** Enum values
  frozen, symbols append-only. The extension binds to the contract, not to
  Rust crate internals.
- **A Rust-source build (the corvid-node/corvid-python shape) would drag the
  heaviest possible install** — a pinned Rust toolchain plus a full engine
  compile — into the one ecosystem whose users least expect to own one.
  Linking the fetched, checksummed cdylib keeps the requirement at "a C
  compiler", which building any PHP extension already requires.

Consequences, all locked:

- **No Rust toolchain, ever.** `./fetch.sh` (macOS/Linux) / `fetch.ps1`
  (Windows) downloads the pinned release archive for the host platform,
  sha256-verifies it against the release's `checksums.txt`, extracts into
  gitignored `deps/`, byte-compares the vendored `golden/` fixtures against
  the release's copies, and normalizes `corvid.h` + the cdylib into
  `deps/current/` (stable name, so `config.m4`'s flags stay
  platform-independent).
- **Pin EXACT engine tags.** One engine version at a time; today `v0.3.4`.
  The pin lives in exactly one variable per fetch script
  (`CORVID_VERSION` / `$CorvidVersion`) and is stamped into
  `deps/version.txt`.
- **No vendored binaries in git.** `deps/` is gitignored.
- **No network at build time.** `phpize && configure && make` consumes
  `deps/current/` only.
- **The extension verifies `corvid_ffi_version() == 1` at MINIT** and refuses
  to load otherwise (FFI.md §4.1: bindings verify before anything else).
- **Published-artifact defects are findings, not patches.** Divergence is
  reported upstream, never worked around here.

## The locked rule: golden port BEFORE ergonomic sugar

Inherited from the bindings program's master plan and non-negotiable:

> **A binding opens with the golden-suite port.** The engine's golden
> fixtures (267 executable lines across 8 files) are the contract; a binding
> that wraps the ABI before it can replay the contract is building on
> unverified ground.

corvid-php's first substantive deliverable is `tests/golden_harness.php` +
`tests/GoldenTest.php` — a port of the engine's harness (as ported
standalone by `corvid-c/test/golden.c` and `corvid-go/golden_test.go`) —
replaying every fixture line **through this binding's public PHP API** (the
`\Corvid\*` classes wherever they express the op; the `\Corvid\Values`
value-mapping surface where the op is inherently a value-handle exercise).
The fixtures are vendored byte-identical under `golden/`. No softened
asserts: the same expectation checks, the same `executed == counted`
dispatch rule, first failure naming file:line + OP + expected-vs-got.

Only with the port green does the ergonomic surface count.

## PHP lifecycle discipline (THE hard part, ruled up front)

PHP's SAPI lifecycle is unlike anything else in the bindings program: in FPM
a process serves many requests, request state dies at request end, and (in
ZTS) one process hosts many interpreter threads. The rulings:

- **MINIT/MSHUTDOWN are per process.** The extension registers its class
  entries (persistent) in `MINIT` and does nothing engine-side in
  `MSHUTDOWN` — the corvid ABI has no global init/teardown (nothing in
  `corvid.h` to call). There is no `RINIT`/`RSHUTDOWN` work at all.
- **Handles are PHP objects; the destructor is the free path.** `Corvid\Db`,
  `Corvid\Collection`, `Corvid\Query`, `Corvid\Predicate` wrap their C
  handles in PHP objects whose object handlers free the handle when the
  object's refcount hits zero (`corvid_close` / `corvid_collection_free` /
  `corvid_query_free` for a never-run builder / `corvid_pred_free` for a
  never-consumed root — both guarded by consumed flags so the ABI's §8
  unconditional-consumption rule can never double-free). `close()` exists on
  `Db` for deterministic release; the destructor is the identical code path,
  idempotent. Result cursors (rows/strs/geohits/groupiter/schemaiter) and
  ABI-returned buffers (`insert_auto` key, `page`'s next-after cursor) are
  walked/copied and freed **inside the single call that produced them** — a
  PHP array of fully-decoded values is the only thing that crosses back.
- **NO per-request state lives in handles.** A handle holds a C pointer and
  a consumed flag; nothing request-scoped is stashed in it. Consequences,
  stated honestly:
  - **CLI and long-running workers** (queues, ReactPHP, Swoole-less
    workers, RoadRunner with persistent workers) are the natural fit: open
    a `Db` once, serve unbounded work from it, the engine's `Arc<Db>`
    sharing does the rest.
  - **Under FPM, handles die with refcount, not with the request.** A
    `Db` stored in a static/superglobal survives request shutdown only if
    something still references it; when the last reference drops (usually
    at request end, during shutdown_sequence), the destructor runs
    `corvid_close` on whatever thread dropped it — legal per FFI §6. If
    userland parks a `Db` in a global that FPM keeps alive across
    requests, the handle simply lives on and the next request reuses it;
    that is a userland decision, documented, not policed. RINIT cleans
    **nothing engine-side because handles are userland objects** — there
    is no extension-owned registry to sweep. The file-backed case is the
    engine's ordinary multi-open story (same-process redb handles share
    the file), the in-memory case is just a longer-lived database.
- **ZTS: every engine call from any thread is safe (FFI §6 — the db is
  `Arc`-shared, reads concurrent, writes serialized), with one sharp
  edge**: the last-error slot is **thread-local**, so the error read must
  happen on the same thread as the failing call. The extension guarantees
  that by construction: every wrapper reads `corvid_last_error_code` /
  `corvid_last_error_message` **immediately after the failing call, in the
  same C function, before returning to PHP** — no deferral, no batching, no
  cross-thread error ferrying exists in the code. A PHP request in ZTS is
  pinned to one interpreter thread for its whole life, so the failing call
  and its error read also share that request's thread. CI carries a ZTS
  leg (linux) that runs the identical golden suite + examples to hold this
  posture.
- **Callbacks (§1.6) are zif callbacks.** `Corvid\Collection::update()` and
  `::scan()` take PHP callables executed from C trampolines via
  `call_user_function`. The discipline:
  - A callback exception **never unwinds through C frames**: the
    trampoline catches it (`zend_clear_exception` after stashing the
    object), aborts/stops per the ABI's §1.6 contract, and the original
    exception is re-thrown **verbatim** — for `scan` and `update`
    alike, the exact object the callback threw (an earlier revision
    wrapped update's in a `Corvid\Exception` carrying
    `CODE_ARGUMENT`; the asymmetry was reviewed out — the engine's
    abort status is its own bookkeeping, and what the caller sees is
    the callback's own exception, whatever it is). Proven by
    `tests/BindingTest.php` (marker exceptions surface as the same
    object, the engine stays usable, an aborted update writes
    nothing); the golden `UPDATE_ABORT` line stays `err:12` because
    its harness callback throws `CorvidException(CODE_ARGUMENT)`.
  - The trampoline decodes the borrowed `current`/`doc` value inside the
    callback frame (copies at the boundary — nothing borrowed is retained
    past the callback's return), and encodes the callback's replacement
    into a fresh owned value.
  - **No reentrant corvid calls from callbacks** (FFI §1.6): the portable
    contract is "do not call into corvid from inside the callback" — the
    engine holds no engine-lock reentrancy promise; violating it is UB or
    a deadlock, not a checked error. Documented in the stub and README,
    not policed at runtime.

## Value mapping (PHP ↔ engine), with its ambiguities ruled

| engine value | PHP (encode) | PHP (decode) |
| --- | --- | --- |
| `Null` | `null` | `null` |
| `Bool` | `true` / `false` | `bool` |
| `Int` (i64) | `int` | `int` |
| `Float` (f64) | `float` (NaN / ±INF / -0.0 cross bit-exact; documented) | `float` |
| `Text` (UTF-8) | `string` (must be valid UTF-8 — invalid bytes throw `CODE_ARGUMENT`) | `string` |
| `Bytes` | `Corvid\Bytes` (a string wrapper) | `Corvid\Bytes` |
| `Array` | PHP list array `[1, [2, 3]]` | PHP list array |
| `Map` | PHP assoc array `["n" => 1]` | PHP assoc array |
| `Vector` (f32s) | `Corvid\Vector` (holds an array of floats) | `Corvid\Vector` |

The rulings this table embodies:

- **PHP has one string type and it is byte-opaque; the engine has Text and
  Bytes.** Plain `string` is **Text** (the common case; PHP strings carry
  UTF-8 by convention and the ABI requires UTF-8 text). **Bytes** are
  `Corvid\Bytes` — itself nothing but a PHP string under a type tag, fully
  binary-safe (`"\x00\xff"` round-trips exactly). This is the same
  disambiguation every byte-vs-text language needed (Go: `string` vs
  `[]byte`); PHP just needs the tag as a class because it has no byte type.
- **PHP arrays are shapeless; the engine has Array and Vector.** A plain
  PHP array encodes as **Array** (list) or **Map** (assoc; int keys become
  their decimal string form, an empty array encodes as an empty Map — the
  document shape). **Vector** is `Corvid\Vector` (constructed from an array
  of floats: `new Corvid\Vector([0.9, 0.1])`), which decodes back with its
  f32 payload bit-exact. The brief's "Vector as array of floats" is
  honored through the constructor/`values()` pair; the wrapper exists so
  `[1.0, 0.0]` (an Array of two floats) and a 2-dim embedding can never
  collapse into each other — the golden fixtures pin both shapes.
- **Container depth is capped at the engine's own bound.** The encode
  recursion (PHP zval → `corvid_value`) enforces
  `PHP_CORVID_MAX_NESTING = 128`, mirroring `corvid::value::MAX_NESTING`
  (value.rs — the bound `Value::decode` applies to untrusted bytes): the
  boundary is inclusive and accounted identically, so a 128-deep value
  round-trips and a 129-deep one throws a clean
  `Corvid\Exception(CODE_ARGUMENT)` at the call site. Without the cap a
  deeply nested PHP array would recurse in C and smash the stack —
  uncatchable from PHP; `tests/BindingTest.php` pins both sides of the
  boundary.

`Corvid\Values` exposes the value-family reads on this mapping —
`type()`, `len()`, `asInt()` / `asFloat()` / `asBool()` / `asText()` /
`asBytes()` / `asVector()` (each `null` on wrong type, mirroring the
engine's `Option` accessors), `mapKeys()` (the v0.3.0 §4.4 iterator:
ascending key-byte order; non-maps answer `[]` — inert), `clone()`, and
the harness-facing `push()` / `put()` container-mutation checks, plus
`selfCheck()` (the §7 inert shapes: every `_free(NULL)` no-op, NULL-cursor
`next` answering 0, and the `ffi_version() == 1` gate).

## C-handle lifetime mapping (FFI.md §2 → PHP)

| C handle | PHP owner | Release |
| --- | --- | --- |
| `corvid_db` | `Corvid\Db` | `close()` (idempotent) and the object destructor — same path |
| `corvid_coll` | `Corvid\Collection` | destructor → `corvid_collection_free` (survives `Db::close()` per §2) |
| `corvid_query` | `Corvid\Query` | consumed by `run()`/every aggregate (even on failure, §8); destructor frees an abandoned builder only |
| `corvid_pred` | `Corvid\Predicate` | consumed by `and()`/`or()`/`not()`/`filter()`/`deleteWhere()` (even on failure); destructor frees a never-consumed root only |
| cursors (`rows`, `strs`, `geohits`, `groupiter`, `schemaiter`) | walked to exhaustion inside the producing call, freed in the same call | — |
| buffers (`insert_auto` key, `page` next-after) | copied to a PHP string inside the call, `corvid_free`'d in it | — |
| `corvid_value` (owned) | transient: built inside the wrapper call, freed at its end | — |

Errors are `Corvid\Exception extends \Exception` — `getCode()` carries the
`corvid_err` code (constants `CODE_DATABASE` … `CODE_IO`, `CODE_BUSY`), the
message is the thread-local last-error message captured at the failing call.

## Toolchain policy

Per the engine's `scripts/bindings/README.md` (2026-09 policy): modern
minimums, CI tests latest + previous.

- **PHP floor: 8.3** (the floor a current Debian/Ubuntu LTS line ships in
  2026); **CI matrix: 8.4 (latest) + 8.3 (floor)**, NTS.
- **A ZTS leg on linux** (PHP 8.3-zts) runs the identical golden suite +
  examples — the threading posture above is executed, not asserted.
- **NTS + ZTS, CLI/FPM-shaped**: the extension compiles under both build
  types (no globals beyond persistent class entries; `ZEND_MODULE_API_NO`
  guard keeps the ABI floor honest).
- C compiler: whatever the platform's PHP toolchain already requires
  (clang/Xcode on macOS, gcc/clang on Linux, MSVC on Windows via
  `config.w32`).
- **Windows**: `config.w32` is shipped for the MSVC build, but there is NO
  Windows CI leg and the file has **not** been compiled against a Windows
  PHP toolchain — reproducing PHP's Windows build matrix (php-sdk +
  Visual Studio + matching source drops) for one bootstrap was ruled
  impractical for the CI budget. Stated honestly in the README; the file
  is syntax-checked in CI (`node --check`), nothing more is claimed.
- PHPUnit ^12 via `phpunit.phar` (CI downloads the phar; `composer.json`
  carries the same constraint for `composer test`).

## Phase PHP1 (this bootstrap) — scope

1. **Plan doc** (this file) — ruling, lifecycle section, value mapping,
   lifetime table, toolchain policy.
2. **Repo scaffold** — MIT LICENSE (engine's copyright line),
   `.gitignore`, fetch scripts, Makefile, README (PIE/PECL-pending),
   composer.json, phpunit.xml.dist.
3. **The extension** — `ext/corvid/`: `config.m4` (unix phpize build
   against `deps/current`), `config.w32` (Windows, compile-checked only —
   see above), `corvid.c` + `php_corvid.h` (the whole binding: value
   mapping, all handle classes, callbacks, admin), `corvid.php` (IDE
   stubs), `package.xml` (PECL/PIE metadata).
4. **The golden port** — `tests/` harness, PHPUnit-driven on NTS legs,
   identical suite via a direct driver on the ZTS leg.
5. **Examples tour** — six runnable programs (quickstart, hybrid,
   vector-index, text-search incl. CJK + phrase, graph, geo), CI-run with
   deterministic output.
6. **Surface manifest** — `docs/SURFACE.tsv` vs the pinned engine list +
   `scripts/surface-gate.sh` in CI.
7. **CI** — linux + macos × PHP 8.4/8.3 NTS, linux ZTS leg, surface gate;
   the whole set in minutes.
8. **Publish prep** — `package.xml` for PIE/PECL; README documents
   `pie install corvid/php-corvid` as pending until published.

Out of scope for PHP1: async wrappers (the engine is sync; v1 is
context-free everywhere in the program), an `ffprobe`-style admin CLI,
persistent/preloaded handle pools (a userland pattern to document, not
extension machinery).

## Verdict protocol

Same as corvid-c/corvid-go's: the golden suite logs one
`SMOKE <file> lines=<n> executed=<n>` line per fixture; green means every
expectation of every executable line passed and the dispatch count matches
the pre-scan count. Divergence from the engine-side suite's verdicts is a
defect here; divergence of the artifacts from the engine repo is a finding
for the engine repo.
