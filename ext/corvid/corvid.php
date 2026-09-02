<?php

/**
 * corvid.php — IDE stub for the corvid PHP extension (ext/corvid).
 *
 * The classes below are IMPLEMENTED IN C by the extension; this file
 * exists so IDEs and static analyzers can type-check code that uses it.
 * It is never executed: the extension registers these definitions at
 * MINIT. Keep the signatures byte-compatible with ext/corvid/corvid.c.
 *
 * The value mapping (docs/PLAN.md): PHP null/bool/int/float → engine
 * Null/Bool/Int/Float; string → Text (must be valid UTF-8); Corvid\Bytes
 * → Bytes (binary-safe); PHP list array → Array; assoc array → Map (the
 * empty array is an empty Map — the document shape); Corvid\Vector →
 * Vector (f32 embeddings, bit-exact round-trip).
 *
 * Lifecycle: handles are freed by their object destructors; Db::close()
 * is the explicit path. Query/Predicate objects are consumed by their
 * terminal calls (run()/aggregates/filter()/...) and become inert.
 *
 * Callbacks (update/scan) run between engine operations (FFI §1.6): do
 * NOT call back into corvid from inside one — the portable contract is
 * "no reentrant corvid calls".
 */

namespace Corvid;

/**
 * The FFI version of the loaded libcorvid (bindings verify it is 1).
 */
function ffiVersion(): int {}

/**
 * Every corvid failure surfaces as this exception; getCode() carries
 * the engine's corvid_err code (the CODE_* constants, 0..19).
 */
class Exception extends \Exception
{
    public const CODE_OK = 0;
    public const CODE_DATABASE = 1;
    public const CODE_TRANSACTION = 2;
    public const CODE_TABLE = 3;
    public const CODE_STORAGE = 4;
    public const CODE_COMMIT = 5;
    public const CODE_SET_DURABILITY = 6;
    public const CODE_COMPACTION = 7;
    public const CODE_DECODE = 8;
    public const CODE_CORRUPT_INDEX = 9;
    public const CODE_RESERVED_COLLECTION = 10;
    public const CODE_INVALID_NAME = 11;
    public const CODE_ARGUMENT = 12;
    public const CODE_INCOMPATIBLE_FORMAT = 13;
    public const CODE_EMPTY_INDEX_TRAINING = 14;
    public const CODE_SCHEMA_VIOLATION = 15;
    public const CODE_INVALID_DUMP = 16;
    public const CODE_BACKUP_TARGET_EXISTS = 17;
    public const CODE_IO = 18;
    public const CODE_BUSY = 19;
}

/** Distance metrics for vector sources/indexes (frozen ABI values). */
final class Metric
{
    public const COSINE = 0;
    public const DOT = 1;
    public const L2 = 2;
}

/** Vector quantization modes for HNSW indexes (frozen ABI values). */
final class Quant
{
    public const NONE = 0;
    public const BINARY = 1;
    public const SCALAR = 2;
}

/**
 * An open database: file-backed via Db::open(), in-memory via
 * Db::openMemory(). Handles die with refcount; close() is explicit and
 * idempotent. Under FPM a Db stored in a global survives request
 * shutdown as long as userland keeps a reference — the documented
 * lifecycle (docs/PLAN.md).
 */
final class Db
{
    private function __construct() {}

    /** Open (creating if absent) a file-backed database. */
    public static function open(string $path): Db {}

    /** A purely in-memory database (no file). */
    public static function openMemory(): Db {}

    /** A handle to the named collection (created lazily on first write). */
    public function collection(string $name): Collection {}

    /** User collection names, in name order. */
    public function collections(): array {}

    /** Write a version-stamped logical dump of the whole database. */
    public function dump(string $path): void {}

    /** Replay a dump into this database (merges per the engine contract). */
    public function load(string $path): void {}

    /** Replay a dump, renaming every collection occurrence per the map. */
    public function loadWithRenames(string $path, array $renames): void {}

    /** Physical backup to a FRESH file (an existing target is CODE_BACKUP_TARGET_EXISTS). */
    public function backup(string $path): void {}

    /**
     * Reclaim file space after heavy deletes. Requires quiescence (every
     * collection/query handle freed) — otherwise CODE_BUSY. Returns
     * whether any data moved.
     */
    public function compact(): bool {}

    /** Release the handle's reference (idempotent; the destructor calls the same path). */
    public function close(): void {}
}

/**
 * A named collection: writes, reads, queries, graph, geo, indexes,
 * schema, TTL. The handle keeps the engine alive independently of Db.
 */
final class Collection
{
    /** The collection's name. */
    public function name(): string {}

    /** Insert or overwrite the document at $key (atomic with index maintenance). */
    public function insert(string $key, mixed $doc): void {}

    /** Single-transaction bulk load of a key => document map (atomic; last-write-wins on dup keys). */
    public function putMany(array $documents): void {}

    /** Insert under a fresh 20-digit zero-padded key; returns the key. */
    public function insertAuto(mixed $doc): string {}

    /**
     * Read-modify-write $key. The callable receives the current document
     * (null when absent) and returns the replacement (null deletes the
     * key). A throwing callback aborts the write (nothing is written;
     * the engine records its CODE_ARGUMENT abort status) and its
     * exception is re-thrown VERBATIM after the engine call returns —
     * the same contract as scan(). No reentrant corvid calls from
     * inside the callback (§1.6).
     */
    public function update(string $key, callable $callback): void {}

    /** Merge the patch's top-level fields into the map at $key (creating it if absent). */
    public function patch(string $key, mixed $patch): void {}

    /**
     * Atomic conditional write. Null $expected means "must be absent";
     * null $replacement means "delete on match". Returns whether it
     * applied (a failed compare is NOT an error). Equality is the
     * engine's: NaN == NaN, -0.0 == 0.0, containers element-wise.
     */
    public function compareAndSet(string $key, mixed $expected, mixed $replacement): bool {}

    /** Remove the document at $key (graph edges cascade); returns whether it existed. */
    public function delete(string $key): bool {}

    /** Delete every document matching the predicate (consumed); returns the count. */
    public function deleteWhere(Predicate $predicate): int {}

    /** Delete each key (edges cascade); returns how many existed. */
    public function deleteBatch(string ...$keys): int {}

    /** Insert with an expiry timestamp (caller's epoch; atomic with the row). */
    public function insertWithTtl(string $key, mixed $doc, int $expiresAt): void {}

    /** Set (or replace) $key's expiry without rewriting the document. */
    public function setTtl(string $key, int $expiresAt): void {}

    /** The key's expiry, or null when none is set. */
    public function getTtl(string $key): ?int {}

    /** Delete every record whose expiry is <= $now (inclusive); returns the count. */
    public function purgeExpired(int $now): int {}

    /** The document at $key (decoded per the value mapping), or null when absent. */
    public function get(string $key): mixed {}

    /** The named top-level fields of the document at $key (absent fields omitted). */
    public function getFields(string $key, string ...$fields): array {}

    /**
     * Stream every (key, document) in key order to the callback; return
     * false to stop early (stopping is not an error). A throwing
     * callback stops the scan and the exception propagates afterwards.
     */
    public function scan(callable $callback): void {}

    /**
     * Keyset pagination: up to $limit documents in key order strictly
     * after $after (null starts at the beginning). ->rows is Row[];
     * ->next is the resume cursor or null at the end.
     */
    public function page(?string $after, int $limit): Page {}

    /** The document count (O(1)). */
    public function len(): int {}

    /**
     * DIRECT positional phrase search over a text field: consecutive,
     * in-order analyzed tokens (stop words collapse out of adjacency).
     * Rows carry the BM25 phrase score; k = 0 is an empty result, never
     * an error.
     */
    public function phraseSearch(string $field, string $phrase, int $k): array {}

    /** Add a directed edge (idempotent; overwrites a weighted edge's weight with 1.0). */
    public function link(string $from, string $relation, string $to): void {}

    /** Add a directed edge carrying a weight (readable back via neighborsWeighted). */
    public function linkWeighted(string $from, string $relation, string $to, float $weight): void {}

    /** Remove the edge and its reverse atomically; returns whether the FORWARD edge existed. */
    public function unlink(string $from, string $relation, string $to): bool {}

    /** Targets of every from --relation--> ? edge, in key order. */
    public function neighbors(string $from, string $relation): array {}

    /** Sources of every ? --relation--> to edge, in key order. */
    public function inNeighbors(string $to, string $relation): array {}

    /** (target, weight) for every from --relation--> ? edge, in key order. */
    public function neighborsWeighted(string $from, string $relation): array {}

    /** BFS over relation up to $hops (excluding the start; cycles terminate). */
    public function traverse(string $start, string $relation, int $hops): array {}

    /** Documents whose $field point lies within $radiusKm (inclusive), nearest first. */
    public function geoWithinRadius(string $field, float $lat, float $lon, float $radiusKm): array {}

    /** Documents inside the lat/lon box, in key order (antimeridian-wrapping). */
    public function geoWithinBBox(string $field, float $minLat, float $minLon, float $maxLat, float $maxLon): array {}

    /** The true $k nearest documents by $field point, nearest first. */
    public function geoNearest(string $field, float $lat, float $lon, int $k): array {}

    /** Create (or replace) a scalar secondary index on $field. */
    public function createScalarIndex(string $field): void {}

    /** Create (or replace) a compound index over the ordered field list. */
    public function createCompoundIndex(string ...$fields): void {}

    /** Create (or replace) an in-memory inverted text index on $field. */
    public function createTextIndex(string $field): void {}

    /** Create (or replace) an on-disk inverted text index on $field. */
    public function createTextIndexOnDisk(string $field): void {}

    /** Create (or replace) a spatial index on $field. */
    public function createGeoIndex(string $field): void {}

    /** Full-precision in-memory HNSW on $field. */
    public function createVectorIndex(string $field, int $metric = Metric::COSINE): void {}

    /** Quantized in-memory HNSW (binary ~32x / scalar ~4x smaller). */
    public function createVectorIndexQuantized(string $field, int $metric, int $quant): void {}

    /** On-disk HNSW on $field (persists; backfills synchronously). */
    public function createVectorIndexOnDisk(string $field, int $metric): void {}

    /** Quantized on-disk HNSW on $field. */
    public function createVectorIndexOnDiskQuantized(string $field, int $metric, int $quant): void {}

    /** In-memory product-quantized HNSW (dim % m == 0; trains from existing vectors). */
    public function createVectorIndexPQ(string $field, int $metric, int $m, int $k): void {}

    /** On-disk product-quantized HNSW. */
    public function createVectorIndexOnDiskPQ(string $field, int $metric, int $m, int $k): void {}

    /**
     * Declare (or replace) the collection's schema — enforced on
     * subsequent writes only. Empty call declares an empty schema.
     */
    public function setSchema(FieldDef ...$definitions): void {}

    /** The declared schema as FieldDef[], or null when none is declared. */
    public function schema(): ?array {}

    /** Begin a query builder over this collection. */
    public function query(): Query {}
}

/**
 * The fluent query builder. Setters mutate and return $this; the
 * terminal (run() or an aggregate) CONSUMES the builder — using it
 * afterwards throws CODE_ARGUMENT.
 */
final class Query
{
    /** Add a filter (multiple filters AND together; consumes the predicate). */
    public function filter(Predicate $predicate): Query {}

    /** Add a vector-search source (Corvid\Vector or an array of numbers). */
    public function vector(string $field, Vector|array $query, int $k, int $metric = Metric::COSINE): Query {}

    /** Add a BM25 text-search source. */
    public function text(string $field, string $query, int $k): Query {}

    /** Set the Reciprocal Rank Fusion constant (engine default 60). */
    public function fuseRrf(float $k): Query {}

    /** Diversify with Maximal Marginal Relevance (lambda in [0,1]). */
    public function rerankMmr(float $lambda): Query {}

    /** Allow approximate execution (filtered ANN with over-fetch). */
    public function approx(): Query {}

    /** Cap the result at $n rows (0 yields an empty result). */
    public function limit(int $n): Query {}

    /** Skip the first $n rows (after ordering, before limit). */
    public function offset(int $n): Query {}

    /** Order by a scalar field instead of rank (the engine's class rule). */
    public function orderBy(string $field, bool $descending = false): Query {}

    /** Project result documents to these top-level fields. */
    public function select(string ...$fields): Query {}

    /** Execute and return Corvid\Row[] (documents always decoded). */
    public function run(): array {}

    /** Count the matching documents (O(1) when unfiltered). */
    public function count(): int {}

    /** Distinct values at $field (canonical group keys). */
    public function countDistinct(string $field): int {}

    /** Sum the numeric values at $field (all-skipped sums to 0.0). */
    public function sum(string $field): float {}

    /** Mean of the numeric values at $field, or null when none exist. */
    public function avg(string $field): ?float {}

    /** The minimum comparable value at $field, or null when none exists. */
    public function min(string $field): mixed {}

    /** The maximum comparable value at $field, or null when none exists. */
    public function max(string $field): mixed {}

    /** Count grouped by the value at $field → Group[] (ascending group key). */
    public function groupCount(string $field): array {}

    /** Sum $valueField grouped by $groupField → Group[]. */
    public function groupSum(string $groupField, string $valueField): array {}

    /** Mean of $valueField grouped by $groupField → Group[]. */
    public function groupAvg(string $groupField, string $valueField): array {}
}

/**
 * A predicate over a dotted field path, built from Corvid\Field.
 * Consumed by Query::filter(), Collection::deleteWhere(), and the
 * and()/or()/not() combinators (which return a fresh Predicate).
 */
final class Predicate
{
    /** Conjunction — consumes both $this and $other. */
    public function and(Predicate $other): Predicate {}

    /** Disjunction — consumes both $this and $other. */
    public function or(Predicate $other): Predicate {}

    /** Negation — consumes $this. */
    public function not(): Predicate {}
}

/** The predicate factory: new Corvid\Field('a.b') yields a fluent path. */
final class Field
{
    /** @var string The dotted field path. */
    public string $path;

    public function __construct(string $path) {}

    public function eq(mixed $value): Predicate {}
    public function ne(mixed $value): Predicate {}
    public function lt(mixed $value): Predicate {}
    public function le(mixed $value): Predicate {}
    public function gt(mixed $value): Predicate {}
    public function ge(mixed $value): Predicate {}
    public function in(mixed ...$values): Predicate {}
    public function between(mixed $low, mixed $high): Predicate {}
    public function startsWith(string $prefix): Predicate {}
    public function contains(string $substr): Predicate {}
    public function geoWithin(float $lat, float $lon, float $radiusKm): Predicate {}
    public function exists(): Predicate {}
}

/** One result row of run()/phraseSearch(). */
final class Row
{
    /** @var string The document key. */
    public string $key;

    /** @var mixed The decoded document. */
    public mixed $doc;

    /** @var float The score (fused RRF for run(); BM25 phrase for phraseSearch(); 0.0 for page()). */
    public float $score;
}

/** One keyset-pagination page. */
final class Page
{
    /** @var Row[] Up to $limit rows in key order. */
    public array $rows;

    /** @var string|null The resume cursor, or null at the end of the collection. */
    public ?string $next;
}

/** One geo hit. */
final class GeoHit
{
    /** @var string The document key. */
    public string $key;

    /** @var float Kilometres from the query point (0.0 sentinel for bbox hits). */
    public float $distanceKm;

    /** @var mixed The decoded document. */
    public mixed $doc;
}

/** One weighted graph hit (from neighborsWeighted). */
final class WeightedHit
{
    /** @var string The target key. */
    public string $key;

    /** @var float The edge weight (1.0 for unweighted edges). */
    public float $weight;
}

/** One aggregation group. */
final class Group
{
    /** @var string The canonical group key (text bare; i:/f:/b: tagged). */
    public string $key;

    /** @var float The group's count/sum/mean. */
    public float $value;
}

/**
 * The engine's Bytes value under a PHP type tag: fully binary-safe
 * ("\x00\xff" round-trips exactly). Cast to string (or ->bytes) to
 * read. Plain PHP strings encode as Text; only Corvid\Bytes encodes as
 * Bytes.
 */
final class Bytes
{
    /** @var string The raw bytes. */
    public string $bytes;

    public function __construct(string $bytes) {}

    public function __toString(): string {}
}

/**
 * The engine's Vector value: a dense f32 embedding constructed from an
 * array of numbers. Plain PHP arrays encode as Array/Map — the wrapper
 * is what makes an embedding distinguishable from a list of numbers.
 */
final class Vector
{
    /** @var float[] The f32 payload (bit-exact round-trip). */
    public array $values;

    /** @param array<int, float|int> $values */
    public function __construct(array $values) {}

    /** @return float[] */
    public function values(): array {}
}

/** One declared schema field (see Collection::setSchema). */
final class FieldDef
{
    public const TYPE_ANY = 0;
    public const TYPE_BOOL = 1;
    public const TYPE_INT = 2;
    public const TYPE_FLOAT = 3;
    public const TYPE_TEXT = 4;
    public const TYPE_BYTES = 5;
    public const TYPE_VECTOR = 6;
    public const TYPE_ARRAY = 7;
    public const TYPE_MAP = 8;

    /** @var string */
    public string $name;

    /** @var int One of the TYPE_* constants. */
    public int $type;

    /** @var bool The field must be present and non-null on every write. */
    public bool $required;

    /** @var bool No two documents may share this field's value. */
    public bool $unique;

    public function __construct(string $name, int $type, bool $required = false, bool $unique = false) {}
}

/**
 * The value-mapping surface: engine-side type interrogations and the
 * map-key iterator over PHP values (encode → read → free). Primarily
 * the conformance harness's channel into the value family; also the
 * userland answer to "what kind of value is this decoded field?".
 */
final class Values
{
    /** The engine-side discriminant of the mapped value: 'null'|'bool'|'int'|'float'|'text'|'bytes'|'array'|'map'|'vector'. */
    public static function type(mixed $value): string {}

    /** The engine-side length (array items / map entries / vector dims / text or bytes length). */
    public static function len(mixed $value): int {}

    /** The int read (null on wrong type — the engine's Option convention). */
    public static function asInt(mixed $value): ?int {}

    /** The float read (null on wrong type). NaN/±INF/-0.0 cross bit-exact. */
    public static function asFloat(mixed $value): ?float {}

    /** The bool read (null on wrong type). */
    public static function asBool(mixed $value): ?bool {}

    /** The text read (null on wrong type). */
    public static function asText(mixed $value): ?string {}

    /** The bytes read (null on wrong type). */
    public static function asBytes(mixed $value): ?Bytes {}

    /** The vector read (null on wrong type). */
    public static function asVector(mixed $value): ?Vector {}

    /**
     * The map's keys in ascending key-BYTE order (the v0.3.0 §4.4
     * iterator). Non-map values answer [] — inert, not an error.
     */
    public static function mapKeys(mixed $value): array {}

    /** Deep-copy round-trip through the ABI's corvid_value_clone. */
    public static function clone(mixed $value): mixed {}

    /**
     * Encode $container, append $item via corvid_value_array_push,
     * return the container's engine-side length. (Harness-facing.)
     */
    public static function push(mixed $container, mixed $item): int {}

    /**
     * Encode $map, insert $value under $key via corvid_value_map_put,
     * return the map's engine-side length. (Harness-facing.)
     */
    public static function put(mixed $map, string $key, mixed $value): int {}

    /**
     * Exercise the ABI's documented inert shapes (§7): every _free(NULL)
     * is a no-op, NULL-cursor next answers 0, corvid_value_type(NULL)
     * answers TYPE_NULL, and the FFI version is 1. Throws on violation.
     */
    public static function selfCheck(): bool {}
}
