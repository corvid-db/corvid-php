# corvid-php

The PHP binding for [corvid](https://github.com/corvid-db/corvid) — an
embedded database with vector search, BM25 full-text search, metadata
filtering, graph traversal, and geo, fused in one transactionally
consistent query builder. This repo is a **native PHP extension in C**
(`ext/corvid`) over the engine's **published, checksummed FFI artifacts**
(the platform cdylib + `corvid.h`): no Rust toolchain, no vendored
binaries, ever.

**Documentation:** the [corvid docs site](https://corvid-db.github.io/docs/)
is canonical — this binding has its own
[corvid-php page](https://corvid-db.github.io/docs/bindings/corvid-php/),
and the [C ABI section](https://corvid-db.github.io/docs/ffi/) documents
every symbol this extension links (handles, ownership, errors,
threading).

A taste of the API:

```php
$db = Corvid\Db::openMemory();
$docs = $db->collection('docs');

$docs->insert('s1', [
    'kind' => 'doc', 'body' => 'rust embedded database',
    'v' => new Corvid\Vector([1.0, 0.0]),
]);

// filter + vector + text, RRF-fused and MMR-reranked,
// in one transactionally consistent call.
$rows = $docs->query()
    ->filter((new Corvid\Field('kind'))->eq('doc'))
    ->vector('v', new Corvid\Vector([1.0, 0.0]), 2, Corvid\Metric::COSINE)
    ->text('body', 'rust database', 2)
    ->fuseRrf(60.0)
    ->rerankMmr(1.0)
    ->limit(2)
    ->run();

foreach ($rows as $rank => $r) {
    printf("%d. %s score=%.6f %s\n", $rank + 1, $r->key, $r->score, $r->doc['body']);
}
```

Failures throw `Corvid\Exception` with the engine's error code in
`getCode()` (`Corvid\Exception::CODE_*`).

## Quick start (from source)

Requirements: PHP 8.3+ with its dev headers (`phpize`, `php-config`),
a C compiler, and `curl` + `shasum`/`sha256sum`.

```sh
./fetch.sh                        # download + sha256-verify corvid v0.4.1
./scripts/build-ext.sh            # phpize + configure + make
php -d extension=ext/corvid/modules/corvid.so tests/run-golden.php
php -d extension=ext/corvid/modules/corvid.so examples/quickstart.php
```

PHPUnit instead of the direct driver (after `curl -L -o phpunit.phar
https://phar.phpunit.de/phpunit-12.phar`):

```sh
php -d extension=ext/corvid/modules/corvid.so phpunit.phar
```

Or make-driven: `make deps ext test examples gate`.

The extension embeds an absolute rpath to the fetched `deps/current`
at build time. To install system-wide, put `libcorvid.dylib`/`libcorvid.so`
and `corvid.h` somewhere standard (e.g. `/usr/local/lib`) and rebuild with
`--with-corvid=/usr/local`.

## Installing (PIE over Packagist)

The package is **published on Packagist** as
[`corvid/php-corvid`](https://packagist.org/packages/corvid/php-corvid),
tag-synced with engine releases, and installs with PIE (the PHP
Installer for Extensions — the modern replacement for `pecl`):

```sh
pie install corvid/php-corvid --with-corvid=/path/to/artifacts
```

The `--with-corvid` directory must hold the engine's published
`corvid.h` + cdylib (the configure step fails with a clear error
without them — this extension links the real engine library and never
vendored binaries). Get the artifacts either way: run `./fetch.sh` in
a clone (downloads + sha256-verifies the pinned release), or install
`libcorvid` + `corvid.h` system-wide and point at that (e.g.
`--with-corvid=/usr/local`).

## The value mapping

| engine value | PHP |
| --- | --- |
| Null / Bool / Int / Float | `null` / `bool` / `int` / `float` (NaN, ±INF, -0.0 cross bit-exact) |
| Text (UTF-8) | `string` — invalid UTF-8 throws `CODE_ARGUMENT` |
| Bytes (binary-safe) | `Corvid\Bytes` (a string wrapper; `"\x00\xff"` round-trips exactly) |
| Array / Map | PHP list array / assoc array (the empty array is an empty Map — the document shape) |
| Vector (f32) | `Corvid\Vector` (constructed from an array of numbers; bit-exact round-trip) |

PHP has one byte-opaque string type and shapeless arrays, so `Corvid\Bytes`
and `Corvid\Vector` are the type tags that keep Text/Bytes and
Array/Vector distinguishable — the same disambiguation Go solved with
`string` vs `[]byte`. `Corvid\Values` exposes the engine-side
interrogations on this mapping (`type()`, `len()`, `asInt()/asFloat()/
asBool()/asText()/asBytes()/asVector()`, `mapKeys()`, `clone()`).

## Lifecycle: CLI, workers, FPM, ZTS

- **Handles are PHP objects.** `Corvid\Db`, `Collection`, `Query`,
  `Predicate` free their C handles in their object destructors
  (`Db::close()` is the explicit, idempotent path). No per-request
  state lives anywhere: MINIT registers classes (per process) and
  verifies `corvid_ffi_version() === 1`; RINIT does nothing.
- **CLI and long-running workers** are the natural fit: open a `Db`
  once and serve unbounded work from it.
- **Under FPM**, handles die with refcount, not with the request. A
  `Db` parked in a global survives request shutdown for as long as
  userland holds a reference — a userland decision, documented, not
  policed. There is no extension-owned registry to sweep at RINIT.
- **ZTS**: the engine is thread-safe per FFI §6 (`Arc<Db>`), and the
  one sharp edge — the thread-local last-error slot — is closed by
  construction: every extension wrapper reads
  `corvid_last_error_code/message` immediately after the failing call,
  in the same C function, on the same thread. A PHP request is pinned
  to one interpreter thread in ZTS. CI carries a linux ZTS leg running
  the identical golden suite.
- **Callbacks** (`update`, `scan`) run between engine operations; do
  not call back into corvid from inside one (FFI §1.6 — UB or a
  deadlock, not a checked error). A throwing callback aborts/stops
  safely and the exception is re-thrown after the engine call returns,
  never unwound through C frames.

## The correctness floor

`tests/run-golden.php` (and the PHPUnit front) replays the engine's
entire **golden fixture suite** — 267 executable lines across 8 files,
including the v0.3.0 `VMAP_KEYS`/`GET_KEYS` (map-key iteration) and
`PHRASE`/`PHRASE_K0` (direct positional search) lines — against the
**downloaded** cdylib, through this binding's public PHP API: every
counted line must dispatch, the first failure names file:line + OP +
expected-vs-got, and every handle is freed on its creation path. On
top sits `docs/SURFACE.tsv` (327 engine constructs at this pin),
gated in CI.

## Windows, stated honestly

`ext/corvid/config.w32` is shipped for the standard MSVC build, but it
has **not been compiled** against a Windows PHP toolchain and there is
no Windows CI leg — reproducing PHP's Windows build matrix
(php-sdk-binary-tools + Visual Studio + matching source drops) was
ruled impractical for this bootstrap. The file is syntax-checked in CI
(`node --check`); treat the first real Windows compile as the review it
still needs.

## Versioning

The engine pin lives in one variable per fetch script
(`CORVID_VERSION` in `fetch.sh`, `$CorvidVersion` in `fetch.ps1`,
mirrored in `.engine-pin`) — today `v0.4.1`. Artifacts are always taken
from that exact tag's GitHub release and sha256-verified; `deps/` is
never committed. Engine-pin bumps fan out to every binding via the
engine repo's `scripts/bindings/bump.sh`.

## License

MIT — see [LICENSE](LICENSE).
