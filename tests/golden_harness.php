<?php

/**
 * golden_harness.php — the golden-suite port, corvid-php's port of the
 * engine's reference harness (corvid-db/corvid,
 * crates/corvid-ffi/c/smoke.c, MIT) as ported standalone by
 * corvid-c/test/golden.c and corvid-go/golden_test.go.
 *
 * Same job as upstream, different moment of truth: the engine's harness
 * links the cdylib cargo just built and reads the golden/ fixtures
 * committed in the engine repo; this one drives the cdylib DOWNLOADED
 * from the pinned GitHub release (fetch.sh put it, corvid.h, and the
 * release's golden/ under deps/current) through THIS BINDING — the
 * Corvid namespace's PHP API wherever it can express the op, the
 * Corvid\Values value-mapping surface where the op is inherently a
 * value-handle exercise (VTYPE, VLEN, VAS_*, V*_REF, VNEST, VCLONE,
 * VPUSH, VPUT go through Values' encode-then-read shapes; the plan doc
 * records that nested borrows surface as decode-then-walk, the
 * mapping's own shape). If the
 * published .so/.dylib, header, or fixtures disagree with the engine's
 * suite, THIS fails where that one stayed green — divergence is a
 * finding for the engine repo, never patched around here.
 *
 * The mechanics are kept deliberately IDENTICAL to the C/Go harnesses
 * so the suites are diffable and their verdicts comparable: the same
 * fixture grammar (OP<TAB>args<TAB>expected; value literals with
 * bits:/bits32: NaN specials; ~x computed-double tolerance), the same
 * dispatch, the same checks, one line at a time, every line dispatched,
 * every expectation checked — no softened asserts. Two counting rules
 * carry over verbatim: `lines` comes from an INDEPENDENT pre-scan (so a
 * dispatch loop that skips a counted line diverges from `executed`),
 * and the first failure names file:line + OP + expected-vs-got.
 *
 * Verdict protocol: stdout carries one "SMOKE <file> lines=<n>
 * executed=<n>" line per fixture.
 *
 * Float-bits note: NaN payload and -0.0 comparisons use pack/unpack
 * IEEE-754 bit views (little-endian assumed — every supported platform
 * is LE IEEE-754, the same assumption the C harness makes via memcpy).
 */

declare(strict_types=1);

namespace CorvidGolden;

use Corvid\Bytes;
use Corvid\Db;
use Corvid\Exception as CorvidException;
use Corvid\Field;
use Corvid\FieldDef;
use Corvid\Metric;
use Corvid\Quant;
use Corvid\Values;
use Corvid\Vector;
use RuntimeException;

final class Harness
{
    private ?Db $db = null;
    /** @var \Corvid\Collection|null */
    private $coll = null;
    private string $file = '';
    private int $line = 0;
    private string $op = '';
    private string $workdir = '';
    private string $dbPath = '';
    private string $db2Path = '';
    private string $dumpPath = '';
    private string $backupPath = '';
    private int $lastAutoID = 0;

    // ---------------------------------------------------------------
    // failure protocol
    // ---------------------------------------------------------------

    private function fail(string $format, ...$args): void
    {
        throw new RuntimeException(sprintf(
            'FAIL %s:%d OP=%s: %s',
            $this->file,
            $this->line,
            $this->op,
            sprintf($format, ...$args)
        ));
    }

    private function check(bool $cond, string $format, ...$args): void
    {
        if (!$cond) {
            $this->fail($format, ...$args);
        }
    }

    private function expectOk(mixed $r): void
    {
        if ($r instanceof CorvidException) {
            $this->fail('expected ok, got code %d (%s)', $r->getCode(), $r->getMessage());
        }
    }

    private function expectErr(mixed $r, int $code): void
    {
        if ($r === null) {
            $this->fail('expected Corvid\\Exception code %d, got success', $code);
        }
        if (!($r instanceof CorvidException)) {
            $this->fail('expected a Corvid\\Exception, got %s', $this->repr($r));
        }
        /** @var CorvidException $r */
        if ($r->getCode() !== $code) {
            $this->fail('expected error code %d, got %d (%s)', $code, $r->getCode(), $r->getMessage());
        }
        if ($r->getMessage() === '') {
            $this->fail('error code %d recorded but the message is missing', $code);
        }
    }

    /** Run $fn, mapping any thrown Corvid\Exception to itself (the Go
     * error surface), and anything else to a failure. */
    private function caught(callable $fn): mixed
    {
        try {
            return $fn();
        } catch (CorvidException $e) {
            return $e;
        }
    }

    // ---------------------------------------------------------------
    // scenario state
    // ---------------------------------------------------------------

    private function closeColl(): void
    {
        if ($this->coll !== null) {
            unset($this->coll); // the destructor frees the handle
            $this->coll = null;
        }
    }

    private function closeDb(): void
    {
        $this->closeColl();
        if ($this->db !== null) {
            $this->db->close();
            unset($this->db);
            $this->db = null;
        }
    }

    private function docs(): \Corvid\Collection
    {
        if ($this->coll === null) {
            $this->check($this->db !== null, 'no database open in this scenario');
            $c = $this->db->collection('docs');
            $this->check($c !== null, 'Collection(docs) failed');
            $this->coll = $c;
        }
        return $this->coll;
    }

    private function openMemory(): void
    {
        $this->closeDb();
        $this->db = Db::openMemory();
        $this->docs();
    }

    private function openFile(string $path): void
    {
        $this->closeDb();
        $this->db = Db::open($path);
        $this->docs();
    }

    private function setColl(string $name): void
    {
        $this->closeColl();
        $c = $this->db->collection($name);
        $this->check($c->name() === $name, 'collection_name round trip failed');
        $this->coll = $c;
    }

    // ---------------------------------------------------------------
    // spans and tokenizing (the C harness's split_top, verbatim)
    // ---------------------------------------------------------------

    /** @return string[] */
    private static function splitTop(string $s): array
    {
        $out = [];
        $depth = 0;
        $start = 0;
        $len = strlen($s);
        for ($i = 0; $i <= $len; $i++) {
            $c = $i < $len ? $s[$i] : ',';
            if ($c === '[' || $c === '{' || $c === '(') {
                $depth++;
            } elseif ($c === ']' || $c === '}' || $c === ')') {
                $depth--;
            }
            if ($c === ',' && $depth === 0) {
                $end = $i;
                while ($end > $start && ($s[$end - 1] === ' ' || $s[$end - 1] === "\r")) {
                    $end--;
                }
                if ($end > $start) {
                    $out[] = substr($s, $start, $end - $start);
                }
                $start = $i + 1;
            }
        }
        return $out;
    }

    private function parseI64(string $s): int
    {
        if (!preg_match('/^-?\d+$/', $s)) {
            $this->fail('bad int token %s', $s);
        }
        return (int)$s;
    }

    private function parseInt(string $s): int
    {
        return $this->parseI64($s);
    }

    private function parseHex(string $s): int
    {
        $s = str_starts_with($s, '0x') || str_starts_with($s, '0X') ? substr($s, 2) : $s;
        if (!preg_match('/^[0-9a-fA-F]+$/', $s)) {
            $this->fail('bad hex token %s', $s);
        }
        return (int)hexdec($s);
    }

    private function parseDouble(string $s): float
    {
        if (str_starts_with($s, 'bits:')) {
            return self::f64FromBits($this->parseHex(substr($s, 5)));
        }
        if ($s === 'inf')  { return INF; }
        if ($s === '-inf') { return -INF; }
        if ($s === 'nan')  { return NAN; }
        if (!is_numeric($s)) {
            $this->fail('bad float token %s', $s);
        }
        return (float)$s;
    }

    private static function f64Bits(float $d): int
    {
        return unpack('P', pack('d', $d))[1];
    }

    private static function f64FromBits(int $u): float
    {
        return unpack('d', pack('P', $u))[1];
    }

    private static function f32Bits(float $f): int
    {
        return unpack('V', pack('f', $f))[1];
    }

    private static function f32FromBits(int $u): float
    {
        return unpack('f', pack('V', $u))[1];
    }

    private static function f32(float $d): float
    {
        return unpack('f', pack('f', $d))[1];
    }

    private static function doubleExact(float $got, float $want): bool
    {
        return self::f64Bits($got) === self::f64Bits($want);
    }

    private static function doubleNear(float $got, float $want): bool
    {
        return abs($got - $want) <= 1e-6 * (1.0 + abs($want));
    }

    private function doubleMatches(float $got, string $tok): bool
    {
        if (str_starts_with($tok, '~')) {
            return self::doubleNear($got, $this->parseDouble(substr($tok, 1)));
        }
        if (str_starts_with($tok, '=')) {
            return self::doubleExact($got, $this->parseDouble(substr($tok, 1)));
        }
        return self::doubleExact($got, $this->parseDouble($tok));
    }

    private function errToken(string $expected): int
    {
        $this->check(str_starts_with($expected, 'err:'), 'error expectation must be err:N, got %s', $expected);
        return (int)substr($expected, 4);
    }

    // ---------------------------------------------------------------
    // value literals → PHP values (Bytes/Vector tagged per the mapping)
    // ---------------------------------------------------------------

    private function startsWord(string $s, int $i, string $word): bool
    {
        if (!str_starts_with(substr($s, $i), $word)) {
            return false;
        }
        $after = $i + strlen($word);
        if ($after >= strlen($s)) {
            return true;
        }
        $c = $s[$after];
        return $c === ',' || $c === ']' || $c === '}' || $c === ' ' || $c === "\r";
    }

    private function matchParen(string $s, int $open): int
    {
        $depth = 0;
        for ($q = $open, $n = strlen($s); $q < $n; $q++) {
            if ($s[$q] === '(') {
                $depth++;
            } elseif ($s[$q] === ')') {
                $depth--;
                if ($depth === 0) {
                    return $q;
                }
            }
        }
        $this->fail('unbalanced () in literal');
        return strlen($s);
    }

    private function matchBracket(string $s, int $open): int
    {
        $closer = $s[$open] === '[' ? ']' : '}';
        $depth = 0;
        for ($q = $open, $n = strlen($s); $q < $n; $q++) {
            if ($s[$q] === $s[$open]) {
                $depth++;
            } elseif ($s[$q] === $closer) {
                $depth--;
                if ($depth === 0) {
                    return $q;
                }
            }
        }
        $this->fail('unbalanced %s in literal', $s[$open]);
        return strlen($s);
    }

    private static function skipWS(string $s, int $i): int
    {
        while ($i < strlen($s) && ($s[$i] === ' ' || $s[$i] === "\r")) {
            $i++;
        }
        return $i;
    }

    /** @return array{0: mixed, 1: int} */
    private function buildNumber(string $s, int $i): array
    {
        $start = $i;
        if ($this->startsWord($s, $i, 'inf'))  { return [INF, $i + 3]; }
        if ($this->startsWord($s, $i, '-inf')) { return [-INF, $i + 4]; }
        if ($this->startsWord($s, $i, 'nan'))  { return [NAN, $i + 3]; }
        $isFloat = false;
        $isBits = false;
        if (str_starts_with(substr($s, $i), 'bits:')) {
            $isFloat = true;
            $isBits = true;
            $i += 5;
        }
        $n = strlen($s);
        while ($i < $n) {
            $c = $s[$i];
            if (($c >= '0' && $c <= '9') || $c === '-' || $c === '+') {
                $i++;
            } elseif ($c === '.' || $c === 'e' || $c === 'E') {
                $isFloat = true;
                $i++;
            } elseif ($isBits && (($c >= 'a' && $c <= 'f') || ($c >= 'A' && $c <= 'F') || $c === 'x' || $c === 'X')) {
                $i++;
            } else {
                break;
            }
        }
        $tok = substr($s, $start, $i - $start);
        if ($tok === '') {
            $this->fail('empty numeric literal');
        }
        if ($isBits) { // re-include the prefix, as the C harness does
            return [$this->parseDouble($tok), $i];
        }
        if ($isFloat) {
            if (!is_numeric($tok)) {
                $this->fail('bad float literal %s', $tok);
            }
            return [(float)$tok, $i];
        }
        return [$this->parseI64($tok), $i];
    }

    /** @return array{0: mixed, 1: int} */
    private function buildLit(string $s, int $i): array
    {
        $i = self::skipWS($s, $i);
        if ($i >= strlen($s)) {
            $this->fail('empty literal');
        }
        $c = $s[$i];

        if ($c === '-' || ($c >= '0' && $c <= '9')) {
            return $this->buildNumber($s, $i);
        }
        // bits:/inf/-inf/nan start with letters but are NUMBERS; they must
        // win over the b(...)/t(...) literal heads.
        if (str_starts_with(substr($s, $i), 'bits:') || $this->startsWord($s, $i, 'inf')
            || $this->startsWord($s, $i, '-inf') || $this->startsWord($s, $i, 'nan')) {
            return $this->buildNumber($s, $i);
        }
        if ($this->startsWord($s, $i, 'null'))  { return [null, $i + 4]; }
        if ($this->startsWord($s, $i, 'true'))  { return [true, $i + 4]; }
        if ($this->startsWord($s, $i, 'false')) { return [false, $i + 5]; }

        if (($c === 't' || $c === 'b') && $i + 1 < strlen($s) && $s[$i + 1] === '(') {
            $close = $this->matchParen($s, $i + 1);
            $body = substr($s, $i + 2, $close - $i - 2);
            $i = $close + 1;
            if ($c === 't') {
                return [$body, $i];
            }
            return [new Bytes($body), $i];
        }
        if (str_starts_with(substr($s, $i), 'vec(')) {
            $close = $this->matchParen($s, $i + 3);
            $body = substr($s, $i + 4, $close - $i - 4);
            $i = $close + 1;
            return [$this->buildVec($body), $i];
        }

        if ($c === '[') {
            $close = $this->matchBracket($s, $i);
            $arr = [];
            $j = $i + 1;
            while ($j < $close) {
                [$item, $j] = $this->buildLit($s, $j);
                $arr[] = $item;
                $j = self::skipWS($s, $j);
                if ($j < $close && $s[$j] === ',') {
                    $j++;
                }
            }
            return [$arr, $close + 1];
        }

        if ($c === '{') {
            $close = $this->matchBracket($s, $i);
            $m = [];
            $j = $i + 1;
            while ($j < $close) {
                $j = self::skipWS($s, $j);
                $ks = $j;
                while ($j < $close && $s[$j] !== '=' && $s[$j] !== ',' && $s[$j] !== '}') {
                    $j++;
                }
                if ($j >= $close || $s[$j] !== '=') {
                    $this->fail('map literal needs k=v pairs');
                }
                $key = ltrim(substr($s, $ks, $j - $ks), ' ');
                $j++; // past '='
                [$val, $j] = $this->buildLit($s, $j);
                $m[$key] = $val;
                $j = self::skipWS($s, $j);
                if ($j < $close && $s[$j] === ',') {
                    $j++;
                }
            }
            return [$m, $close + 1];
        }

        $snippet = substr($s, $i);
        if (strlen($snippet) > 24) {
            $snippet = substr($snippet, 0, 24);
        }
        $this->fail('unparseable literal at %s', $snippet);
        return [null, $i];
    }

    private function buildVec(string $body): Vector
    {
        $vals = [];
        foreach (self::splitTop($body) as $tk) {
            if (str_starts_with($tk, 'bits32:')) {
                $vals[] = self::f32FromBits($this->parseHex(substr($tk, 7)));
            } else {
                $vals[] = self::f32($this->parseDouble($tk));
            }
        }
        return new Vector($vals);
    }

    private function lit(string $s): mixed
    {
        [$v,] = $this->buildLit($s, 0);
        return $v;
    }

    // ---------------------------------------------------------------
    // structural comparison of PHP-side values (bit-exact floats)
    // ---------------------------------------------------------------

    private function repr(mixed $v): string
    {
        if ($v instanceof \Corvid\Collection || $v instanceof \Corvid\Query) {
            return get_class($v);
        }
        if ($v instanceof CorvidException) {
            return sprintf('Corvid\\Exception(code=%d, %s)', $v->getCode(), $v->getMessage());
        }
        return var_export($v, true);
    }

    private function valuesEqual(mixed $a, mixed $b): bool
    {
        if (is_null($a)) {
            return is_null($b);
        }
        if (is_bool($a)) {
            return is_bool($b) && $a === $b;
        }
        if (is_int($a)) {
            return is_int($b) && $a === $b;
        }
        if (is_float($a)) {
            return is_float($b) && self::f64Bits($a) === self::f64Bits($b);
        }
        if (is_string($a)) {
            return is_string($b) && $a === $b;
        }
        if ($a instanceof Bytes) {
            return $b instanceof Bytes && $a->bytes === $b->bytes;
        }
        if ($a instanceof Vector) {
            if (!($b instanceof Vector) || count($a->values) !== count($b->values)) {
                return false;
            }
            foreach ($a->values as $i => $x) {
                if (self::f32Bits($x) !== self::f32Bits($b->values[$i])) {
                    return false;
                }
            }
            return true;
        }
        if (is_array($a)) {
            if (!is_array($b) || count($a) !== count($b)) {
                return false;
            }
            $listA = array_is_list($a);
            if ($listA !== array_is_list($b)) {
                return false;
            }
            foreach ($a as $k => $v) {
                if (!array_key_exists($k, $b) || !$this->valuesEqual($v, $b[$k])) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    private function checkValue(mixed $got, string $wantTok): void
    {
        $want = $this->lit($wantTok);
        $this->check($this->valuesEqual($got, $want),
            'value mismatch: got %s, want %s', $this->repr($got), $this->repr($want));
    }

    // ---------------------------------------------------------------
    // cursor walkers (over the binding's returned rows/hits)
    // ---------------------------------------------------------------

    /** @param \Corvid\Row[] $rows */
    private static function rowKeys(array $rows): array
    {
        return array_map(static fn ($r) => $r->key, $rows);
    }

    /** @param \Corvid\Row[] $rows */
    private static function rowScores(array $rows): array
    {
        return array_map(static fn ($r) => $r->score, $rows);
    }

    /** @param string[] $keys */
    private function checkKeys(array $keys, string $expected): void
    {
        $this->check(strlen($expected) >= 3 && $expected[0] === 'k' && $expected[1] === '('
            && substr($expected, -1) === ')',
            'key expectation must be k(...), got %s', $expected);
        $body = substr($expected, 2, -1);
        $want = $body === '' ? [] : self::splitTop($body);
        $this->check(count($keys) === count($want),
            'row count %d, expected %d (%s)', count($keys), count($want), implode(',', $keys));
        foreach ($want as $i => $w) {
            $this->check($keys[$i] === $w, 'row %d key %s, expected %s', $i, $keys[$i], $w);
        }
    }

    /** @param float[] $scores */
    private function checkScores(array $scores, string $suffix): void
    {
        if ($suffix === '') {
            return;
        }
        $this->check($suffix[0] === '|', 'score suffix must start with |, got %s', $suffix);
        $body = substr($suffix, 1);
        if ($body === '') {
            return;
        }
        $toks = self::splitTop($body);
        $this->check(count($scores) === count($toks), 'score count %d, expected %d', count($scores), count($toks));
        foreach ($toks as $i => $tok) {
            $this->check($this->doubleMatches($scores[$i], $tok),
                'row %d score %.9g does not match %s', $i, $scores[$i], $tok);
        }
    }

    private static function keyPart(string $expected): string
    {
        $i = strpos($expected, '|');
        return $i === false ? $expected : substr($expected, 0, $i);
    }

    private static function suffixPart(string $expected): string
    {
        $i = strpos($expected, '|');
        return $i === false ? '' : substr($expected, $i);
    }

    private function textBody(string $tok): string
    {
        $this->check(strlen($tok) >= 3 && $tok[0] === 't' && $tok[1] === '(' && substr($tok, -1) === ')',
            'expected a t(...) literal, got %s', $tok);
        return substr($tok, 2, -1);
    }

    private function listBody(string $tok): string
    {
        $this->check(strlen($tok) >= 3 && $tok[0] === 'k' && $tok[1] === '(' && substr($tok, -1) === ')',
            'expected a k(...) list, got %s', $tok);
        return substr($tok, 2, -1);
    }

    // ---------------------------------------------------------------
    // predicate / enum helpers
    // ---------------------------------------------------------------

    private function fieldCmp(string $path, string $op, mixed $v): \Corvid\Predicate
    {
        $f = new Field($path);
        switch ($op) {
            case 'eq': return $f->eq($v);
            case 'ne': return $f->ne($v);
            case 'lt': return $f->lt($v);
            case 'le': return $f->le($v);
            case 'gt': return $f->gt($v);
            case 'ge': return $f->ge($v);
        }
        $this->fail('bad cmp op %s', $op);
    }

    private function parseMetric(string $s): int
    {
        switch ($s) {
            case 'cosine': return Metric::COSINE;
            case 'dot':    return Metric::DOT;
            case 'l2':     return Metric::L2;
        }
        $this->fail('bad metric %s', $s);
    }

    private function parseQuant(string $s): int
    {
        switch ($s) {
            case 'none':   return Quant::NONE;
            case 'binary': return Quant::BINARY;
            case 'scalar': return Quant::SCALAR;
        }
        $this->fail('bad quant %s', $s);
    }

    private function parseFieldType(string $s): int
    {
        switch ($s) {
            case 'any':    return FieldDef::TYPE_ANY;
            case 'bool':   return FieldDef::TYPE_BOOL;
            case 'int':    return FieldDef::TYPE_INT;
            case 'float':  return FieldDef::TYPE_FLOAT;
            case 'text':   return FieldDef::TYPE_TEXT;
            case 'bytes':  return FieldDef::TYPE_BYTES;
            case 'vector': return FieldDef::TYPE_VECTOR;
            case 'array':  return FieldDef::TYPE_ARRAY;
            case 'map':    return FieldDef::TYPE_MAP;
        }
        $this->fail('bad field type %s', $s);
    }

    private function filteredCount(\Corvid\Predicate $p): int
    {
        return $this->caught(fn () => $this->docs()->query()->filter($p)->count())
            ?? $this->fail('unreachable');
    }

    private function expectNum(string $expected, int $got): void
    {
        $this->check($this->parseI64($expected) === $got, 'expected %d, want %s', $got, $expected);
    }

    // ---------------------------------------------------------------
    // OP implementations (runLine)
    // ---------------------------------------------------------------

    private function runLine(string $op, string $args, string $expected): void
    {
        $a = self::splitTop($args);

        // ---- pure value ops (no db) ----
        switch ($op) {
            case 'VERSION':
                $this->check(\Corvid\ffiVersion() === 1, 'FFI_VERSION must be 1, got %d', \Corvid\ffiVersion());
                return;

            case 'VTYPE':
                $names = ['null', 'bool', 'int', 'float', 'text', 'bytes', 'array', 'map', 'vector'];
                $t = Values::type($this->lit($a[0]));
                $this->check(in_array($t, $names, true), 'type tag %s out of range', $t);
                $this->check($expected === $t, 'type %s, want %s', $t, $expected);
                return;

            case 'VLEN':
                $this->expectNum($expected, Values::len($this->lit($a[0])));
                return;

            case 'VAS_INT':
            case 'VAS_FLOAT':
            case 'VAS_BOOL':
                $v = $this->lit($a[0]);
                if ($op === 'VAS_INT') {
                    $got = Values::asInt($v);
                    if ($expected === 'fail') {
                        $this->check($got === null, 'as_int unexpectedly ok (%s)', $this->repr($got));
                    } else {
                        $this->check($got !== null, 'as_int failed');
                        $this->check($expected === 'ok:' . $got, 'as_int ok:%s, want %s', $got, $expected);
                    }
                } elseif ($op === 'VAS_FLOAT') {
                    $got = Values::asFloat($v);
                    if ($expected === 'fail') {
                        $this->check($got === null, 'as_float unexpectedly ok');
                    } else {
                        $this->check($got !== null, 'as_float failed');
                        $this->check(str_starts_with($expected, 'ok:'), 'as_float expectation must be ok:<double>, got %s', $expected);
                        $this->check($this->doubleMatches($got, substr($expected, 3)),
                            'as_float 0x%016x (%g) does not match %s', self::f64Bits($got), $got, substr($expected, 3));
                    }
                } else {
                    $got = Values::asBool($v);
                    if ($expected === 'fail') {
                        $this->check($got === null, 'as_bool unexpectedly ok');
                    } else {
                        $this->check($got !== null, 'as_bool failed');
                        $want = 'ok:' . ($got ? '1' : '0');
                        $this->check($expected === $want, 'as_bool %s, want %s', $want, $expected);
                    }
                }
                return;

            case 'VTEXT_REF':
                $got = Values::asText($this->lit($a[0]));
                $this->check($got !== null, 'text read returned null for a text value');
                $body = $this->textBody($expected);
                $this->check($got === $body, 'text bytes differ: got %s, want %s', $got, $body);
                return;

            case 'VBYTES_REF':
                $got = Values::asBytes($this->lit($a[0]));
                $this->check($got !== null, 'bytes read returned null for a bytes value');
                $this->check(strlen($expected) >= 3 && $expected[0] === 'b' && $expected[1] === '(',
                    'bytes expectation must be b(...), got %s', $expected);
                $body = substr($expected, 2, -1);
                $this->check((string)$got === $body, 'bytes differ: got %s, want %s', (string)$got, $body);
                return;

            case 'VVECTOR_REF':
                $want = $this->lit($a[0]);
                $this->check($want instanceof Vector, 'vector literal did not parse to Corvid\\Vector');
                $got = Values::asVector($want);
                $this->check($got !== null, 'vector read returned null for a vector value');
                $this->check(count($got->values) === count($want->values),
                    'ref dim %d, rebuilt dim %d', count($got->values), count($want->values));
                foreach ($want->values as $i => $x) {
                    $this->check(self::f32Bits($got->values[$i]) === self::f32Bits($x),
                        'vector elem %d differs bit-exactly', $i);
                }
                return;

            case 'VNEST':
            case 'VCLONE':
                $root = $op === 'VCLONE' ? Values::clone($this->lit($a[0])) : $this->lit($a[0]);
                $child = $this->walkPath($root, $a[1]);
                if ($expected === 'absent') {
                    $this->check($child === null, 'path unexpectedly present');
                } else {
                    $this->check($child !== null, 'path unexpectedly absent, want %s', $expected);
                    $this->checkValue($child, $expected);
                }
                return;

            case 'VPUSH':
                $n = Values::push($this->lit($a[0]), $this->lit($a[1]));
                $this->expectNum($expected, $n);
                return;

            case 'VPUT':
                $n = Values::put($this->lit($a[0]), $a[1], $this->lit($a[2]));
                $this->expectNum($expected, $n);
                return;

            case 'VMAP_KEYS':
                // The v0.3.0 key iterator over a LITERAL: ascending key-BYTE
                // order whatever the construction order; empty map, non-maps,
                // and scalars answer an EMPTY cursor — inert.
                $this->checkKeys(Values::mapKeys($this->lit($a[0])), $expected);
                return;

            case 'NULLFREES':
                Values::selfCheck(); // every _free(NULL) shape + the inert cursor shapes (§7)
                return;
        }

        // ---- db-required ops ----
        switch ($op) {
            case 'COLL':
                $this->setColl($a[0]);
                return;

            case 'INSERT':
            case 'INSERT_ERR':
                $err = $this->caught(fn () => $this->docs()->insert($a[0], $this->lit($a[1])));
                if ($op === 'INSERT_ERR') {
                    $this->expectErr($err, $this->errToken($expected));
                } else {
                    $this->expectOk($err);
                }
                return;

            case 'LEN':
                $this->expectNum($expected, $this->docs()->len());
                return;

            case 'GET':
                if ($expected === 'absent') {
                    $doc = $this->caught(fn () => $this->docs()->get($a[0]));
                    $this->expectOk($doc);
                    $this->check($doc === null, 'expected absence, got a document: %s', $this->repr($doc));
                    return;
                }
                $doc = $this->caught(fn () => $this->docs()->get($a[0]));
                $this->expectOk($doc);
                $this->check($doc !== null, 'expected a document, got absence');
                $this->checkValue($doc, $expected);
                return;

            case 'GETFIELD':
                $m = $this->caught(fn () => $this->docs()->getFields($a[0], $a[1]));
                $this->expectOk($m);
                if ($expected === 'absent') {
                    $this->check(!array_key_exists($a[1], $m), 'field unexpectedly present');
                } else {
                    $this->check(array_key_exists($a[1], $m), 'field unexpectedly absent, want %s', $expected);
                    $this->checkValue($m[$a[1]], $expected);
                }
                return;

            case 'GET_KEYS':
                // Key enumeration over a DECODED document (fetch by key
                // first): the storage round-trip keeps every key;
                // ascending byte order.
                $v = $this->caught(fn () => $this->docs()->get($a[0]));
                $this->expectOk($v);
                $this->check($v !== null, 'GET_KEYS on an absent document');
                $this->checkKeys(Values::mapKeys($v), $expected);
                return;

            case 'PUTMANY':
            case 'PUTMANY_ROLLBACK':
                $this->check(count($a) % 2 === 0, 'PUTMANY wants key/literal pairs');
                $docs = [];
                for ($i = 0; $i < count($a); $i += 2) {
                    $docs[$a[$i]] = $this->lit($a[$i + 1]);
                }
                $err = $this->caught(fn () => $this->docs()->putMany($docs));
                if ($op === 'PUTMANY_ROLLBACK') {
                    $this->expectErr($err, $this->errToken($expected));
                } else {
                    $this->expectOk($err);
                }
                return;

            case 'INSERT_AUTO':
                $key = $this->caught(fn () => $this->docs()->insertAuto($this->lit($a[0])));
                $this->expectOk($key);
                $this->check(is_string($key) && strlen($key) === 20, 'auto key length %d, want 20', strlen((string)$key));
                $id = 0;
                foreach (str_split((string)$key) as $ch) {
                    $this->check($ch >= '0' && $ch <= '9', 'auto key not zero-padded digits: %s', $key);
                    $id = $id * 10 + (int)$ch;
                }
                $this->check($this->lastAutoID === 0 || $id > $this->lastAutoID,
                    'auto id %d not monotonic (previous %d)', $id, $this->lastAutoID);
                $this->lastAutoID = $id;
                return;

            case 'UPDATE':
                $err = $this->caught(function () use ($a) {
                    $this->docs()->update($a[0], function ($current) {
                        $n = 0;
                        if ($current !== null) {
                            $this->check(is_array($current), 'update_bump: current doc is not a map');
                            $this->check(array_key_exists('n', $current), 'update_bump: current doc lacks field n');
                            $f = $current['n'];
                            $this->check(is_int($f), 'update_bump: field n is not an int');
                            $n = $f;
                        }
                        return ['n' => $n + 1];
                    });
                });
                $this->expectOk($err);
                return;

            case 'UPDATE_ABORT':
                // The callback aborts the write; the binding re-throws the
                // callback's OWN exception verbatim (the §1.6 ruling —
                // symmetric with SCAN). Throwing CorvidException(err:12)
                // keeps the fixture's err:12 expectation truthful: the
                // engine's abort status IS CORVID_E_ARGUMENT, and that is
                // exactly what surfaces here.
                $err = $this->caught(function () use ($a) {
                    $this->docs()->update($a[0], function ($current) {
                        throw new CorvidException('update_abort: aborting per the fixture',
                            CorvidException::CODE_ARGUMENT);
                    });
                });
                $this->expectErr($err, CorvidException::CODE_ARGUMENT);
                return;

            case 'PATCH':
                $this->expectOk($this->caught(fn () => $this->docs()->patch($a[0], $this->lit($a[1]))));
                return;

            case 'CAS':
                $ex = $a[1] === 'absent' ? null : $this->lit($a[1]);
                $re = $a[2] === 'absent' ? null : $this->lit($a[2]);
                $applied = $this->caught(fn () => $this->docs()->compareAndSet($a[0], $ex, $re));
                $this->expectOk($applied);
                $want = $applied ? 'applied:1' : 'applied:0';
                $this->check($expected === $want, 'CAS applied=%s, want %s (expected %s)',
                    $applied ? 'true' : 'false', $want, $expected);
                return;

            case 'DELETE':
                $existed = $this->caught(fn () => $this->docs()->delete($a[0]));
                $this->expectOk($existed);
                $want = $existed ? 'existed:1' : 'existed:0';
                $this->check($expected === $want, 'delete existed=%s, want %s', $existed ? 'true' : 'false', $want);
                return;

            case 'DELETE_WHERE':
                $removed = $this->caught(fn () => $this->docs()->deleteWhere($this->fieldCmp($a[0], $a[1], $this->lit($a[2]))));
                $this->expectOk($removed);
                $this->check($expected === sprintf('removed:%d', $removed), 'removed %d, want %s', $removed, $expected);
                return;

            case 'DELETE_IN':
                $vals = array_map(fn ($tok) => $this->lit($tok), array_slice($a, 1));
                $removed = $this->caught(fn () => $this->docs()->deleteWhere((new Field($a[0]))->in(...$vals)));
                $this->expectOk($removed);
                $this->check($expected === sprintf('removed:%d', $removed), 'removed %d, want %s', $removed, $expected);
                return;

            case 'DELETE_BATCH':
                $removed = $this->caught(fn () => $this->docs()->deleteBatch(...$a));
                $this->expectOk($removed);
                $this->check($expected === sprintf('removed:%d', $removed), 'removed %d, want %s', $removed, $expected);
                return;

            case 'INSERT_TTL':
                $this->expectOk($this->caught(fn () => $this->docs()->insertWithTtl($a[0], $this->lit($a[1]), $this->parseI64($a[2]))));
                return;

            case 'GET_TTL':
                $at = $this->caught(fn () => $this->docs()->getTtl($a[0]));
                $this->expectOk($at);
                $got = $at === null ? 'nottl' : sprintf('ttl:%d', $at);
                $this->check($expected === $got, 'ttl %s, want %s', $got, $expected);
                return;

            case 'SET_TTL':
                $this->expectOk($this->caught(fn () => $this->docs()->setTtl($a[0], $this->parseI64($a[1]))));
                return;

            case 'PURGE':
                $purged = $this->caught(fn () => $this->docs()->purgeExpired($this->parseI64($a[0])));
                $this->expectOk($purged);
                $this->check($expected === sprintf('purged:%d', $purged), 'purged %d, want %s', $purged, $expected);
                return;

            case 'SCAN':
            case 'SCAN_STOP':
                $stop = $op === 'SCAN_STOP' ? $this->parseInt($a[0]) : 0;
                $count = 0;
                $err = $this->caught(function () use ($stop, &$count) {
                    $this->docs()->scan(function ($key, $doc) use ($stop, &$count) {
                        $count++;
                        return $stop <= 0 || $count < $stop;
                    });
                });
                $this->expectOk($err);
                $this->expectNum($expected, $count);
                return;

            case 'PAGE':
                $after = $a[0] === '-' ? null : $a[0];
                $page = $this->caught(fn () => $this->docs()->page($after, $this->parseInt($a[1])));
                $this->expectOk($page);
                $this->checkKeys(self::rowKeys($page->rows), self::keyPart($expected));
                $want = $page->next === null ? '|end' : '|more';
                $this->check(self::suffixPart($expected) === $want, 'page cursor %s, want %s', $want, self::suffixPart($expected));
                return;
        }

        // ---- predicates + queries ----
        switch ($op) {
            case 'QF_COUNT':
                $this->expectNum($expected, $this->filteredCount($this->fieldCmp($a[0], $a[1], $this->lit($a[2]))));
                return;

            case 'QF_EXISTS':
                $this->expectNum($expected, $this->filteredCount((new Field($a[0]))->exists()));
                return;

            case 'QF_BETWEEN':
                $this->expectNum($expected, $this->filteredCount((new Field($a[0]))->between($this->lit($a[1]), $this->lit($a[2]))));
                return;

            case 'QF_STARTS':
            case 'QF_CONTAINS':
                $body = $this->textBody($a[1]);
                $f = new Field($a[0]);
                $p = $op === 'QF_STARTS' ? $f->startsWith($body) : $f->contains($body);
                $this->expectNum($expected, $this->filteredCount($p));
                return;

            case 'QF_GEO':
                $this->expectNum($expected, $this->filteredCount(
                    (new Field($a[0]))->geoWithin($this->parseDouble($a[1]), $this->parseDouble($a[2]), $this->parseDouble($a[3]))));
                return;

            case 'QF_AND':
            case 'QF_OR':
                $l = $this->fieldCmp($a[0], $a[1], $this->lit($a[2]));
                $r = $this->fieldCmp($a[3], $a[4], $this->lit($a[5]));
                $p = $op === 'QF_AND' ? $l->and($r) : $l->or($r);
                $this->expectNum($expected, $this->filteredCount($p));
                return;

            case 'QF_NOT':
                $this->expectNum($expected, $this->filteredCount($this->fieldCmp($a[0], $a[1], $this->lit($a[2]))->not()));
                return;

            case 'PRED_FREE':
                $p = $this->fieldCmp($a[0], $a[1], $this->lit($a[2]));
                unset($p); // the never-consumed-root destructor free path
                return;

            case 'Q_ABANDON':
                $q = $this->docs()->query();
                unset($q); // the abandoned-builder destructor free path
                return;

            case 'QVEC':
            case 'APPROX':
                $vec = $this->lit($a[1]);
                $this->check($vec instanceof Vector, 'vector literal did not parse to Corvid\\Vector');
                $rows = $this->caught(function () use ($a, $op, $vec) {
                    $q = $this->docs()->query()->vector($a[0], $vec, $this->parseInt($a[2]), Metric::COSINE);
                    if ($op === 'APPROX') {
                        $q->approx();
                    }
                    return $q->run();
                });
                $this->expectOk($rows);
                $this->checkKeys(self::rowKeys($rows), self::keyPart($expected));
                $this->checkScores(self::rowScores($rows), self::suffixPart($expected));
                return;

            case 'QTEXT':
                $rows = $this->caught(fn () => $this->docs()->query()->text($a[0], $this->textBody($a[1]), $this->parseInt($a[2]))->run());
                $this->expectOk($rows);
                $this->checkKeys(self::rowKeys($rows), $expected);
                return;

            case 'PHRASE':
            case 'PHRASE_K0':
                // The v0.3.0 direct positional search through the binding's
                // phraseSearch: order-sensitive adjacency, BM25 phrase scores
                // in the score suffix; PHRASE_K0 is the inert k==0 shape —
                // an EMPTY result, never an error.
                $rows = $this->caught(fn () => $this->docs()->phraseSearch($a[0], $this->textBody($a[1]), $this->parseInt($a[2])));
                $this->expectOk($rows);
                $this->checkKeys(self::rowKeys($rows), self::keyPart($expected));
                $this->checkScores(self::rowScores($rows), self::suffixPart($expected));
                if ($op === 'PHRASE_K0') {
                    $this->check(count($rows) === 0, 'k == 0 must answer an empty cursor');
                }
                return;

            case 'HYBRID':
            case 'HYBRID_F':
                // args: vfield vec k tfield t(query) tk [tagvalue] limit —
                // the tagvalue (HYBRID_F) slides the limit to the LAST slot.
                $tagged = $op === 'HYBRID_F';
                $vk = $this->parseInt($a[2]);
                $tk = $this->parseInt($a[5]);
                $limitIdx = 6;
                if ($tagged) {
                    $filter = (new Field('tag'))->eq($this->lit($a[6]));
                    $limitIdx = 7;
                } else {
                    $filter = (new Field('kind'))->eq('doc');
                }
                $vec = $this->lit($a[1]);
                $this->check($vec instanceof Vector, 'vector literal did not parse to Corvid\\Vector');
                $rows = $this->caught(function () use ($a, $filter, $vec, $vk, $tk, $limitIdx) {
                    return $this->docs()->query()
                        ->filter($filter)
                        ->vector($a[0], $vec, $vk, Metric::COSINE)
                        ->text($a[3], $this->textBody($a[4]), $tk)
                        ->fuseRrf(60.0)
                        ->rerankMmr(1.0)
                        ->limit($this->parseInt($a[$limitIdx]))
                        ->run();
                });
                $this->expectOk($rows);
                $this->checkKeys(self::rowKeys($rows), self::keyPart($expected));
                $this->checkScores(self::rowScores($rows), self::suffixPart($expected));
                return;

            case 'ORDER_BY':
                $rows = $this->caught(fn () => $this->docs()->query()
                    ->orderBy($a[0], $this->parseInt($a[1]) !== 0)
                    ->offset($this->parseInt($a[2]))
                    ->limit($this->parseInt($a[3]))
                    ->run());
                $this->expectOk($rows);
                $this->checkKeys(self::rowKeys($rows), $expected);
                return;

            case 'SELECT':
                // args: (field,field,...) k(row-key); expected: that row's
                // projected document.
                $this->check(strlen($a[0]) >= 2 && $a[0][0] === '(' && substr($a[0], -1) === ')',
                    'SELECT\'s first arg must be a (field,...) group, got %s', $a[0]);
                $fields = self::splitTop(substr($a[0], 1, -1));
                $rows = $this->caught(fn () => $this->docs()->query()->select(...$fields)->run());
                $this->expectOk($rows);
                $wantKey = $this->listBody($a[1]);
                $doc = null;
                $found = false;
                foreach ($rows as $r) {
                    if ($r->key === $wantKey) {
                        $doc = $r->doc;
                        $found = true;
                    }
                }
                $this->check($found, 'row %s not in the result', $wantKey);
                $this->checkValue($doc, $expected);
                return;

            case 'AGG_COUNT':
                $n = $this->caught(fn () => $this->docs()->query()->count());
                $this->expectOk($n);
                $this->expectNum($expected, $n);
                return;

            case 'AGG_DISTINCT':
                $n = $this->caught(fn () => $this->docs()->query()->countDistinct($a[0]));
                $this->expectOk($n);
                $this->expectNum($expected, $n);
                return;

            case 'AGG_SUM':
                $sum = $this->caught(fn () => $this->docs()->query()->sum($a[0]));
                $this->expectOk($sum);
                $this->check($this->doubleMatches($sum, $expected), 'sum %.17g vs %s', $sum, $expected);
                return;

            case 'AGG_AVG':
                $avg = $this->caught(fn () => $this->docs()->query()->avg($a[0]));
                $this->expectOk($avg);
                if ($expected === 'none') {
                    $this->check($avg === null, 'avg has value, want none');
                } else {
                    $this->check($avg !== null, 'avg null, want %s', $expected);
                    $this->check($this->doubleMatches($avg, $expected), 'avg %.17g vs %s', $avg, $expected);
                }
                return;

            case 'AGG_MIN':
            case 'AGG_MAX':
                $out = $this->caught(fn () => $op === 'AGG_MIN'
                    ? $this->docs()->query()->min($a[0])
                    : $this->docs()->query()->max($a[0]));
                $this->expectOk($out);
                if ($expected === 'absent') {
                    $this->check($out === null, 'expected absence');
                } else {
                    $this->check($out !== null, 'expected a value, got absence');
                    $this->checkValue($out, $expected);
                }
                return;

            case 'AGG_GCOUNT':
            case 'AGG_GSUM':
            case 'AGG_GAVG':
                $groups = $this->caught(function () use ($op, $a) {
                    $q = $this->docs()->query();
                    switch ($op) {
                        case 'AGG_GCOUNT': return $q->groupCount($a[0]);
                        case 'AGG_GSUM':   return $q->groupSum($a[0], $a[1]);
                        default:           return $q->groupAvg($a[0], $a[1]);
                    }
                });
                $this->expectOk($groups);
                // §7 inert rule exercised once per group op (Values::selfCheck
                // drives the NULL-cursor shapes; also re-verifies the frees).
                $this->check(Values::selfCheck(), 'selfCheck must hold');
                $this->check(strlen($expected) >= 3 && $expected[0] === 'g' && $expected[1] === '('
                    && substr($expected, -1) === ')',
                    'group expectation must be g(...), got %s', $expected);
                $body = substr($expected, 2, -1);
                $pairs = $body === '' ? [] : self::splitTop($body);
                $this->check(count($groups) === count($pairs),
                    'group count %d, expected %d', count($groups), count($pairs));
                foreach ($pairs as $i => $pair) {
                    $eq = strrpos($pair, '=');
                    $this->check($eq !== false && $eq > 0, 'group pair needs key=val, got %s', $pair);
                    $key = substr($pair, 0, $eq);
                    $vtok = substr($pair, $eq + 1);
                    $this->check($groups[$i]->key === $key, 'group key %s, want %s', $groups[$i]->key, $key);
                    $this->check($this->doubleMatches($groups[$i]->value, $vtok),
                        'group %s value %.17g vs %s', $key, $groups[$i]->value, $vtok);
                }
                return;
        }

        // ---- graph ----
        switch ($op) {
            case 'LINK':
                $this->expectOk($this->caught(fn () => $this->docs()->link($a[0], $a[1], $a[2])));
                return;

            case 'LINK_W':
                $this->expectOk($this->caught(fn () => $this->docs()->linkWeighted($a[0], $a[1], $a[2], $this->parseDouble($a[3]))));
                return;

            case 'UNLINK':
                $removed = $this->caught(fn () => $this->docs()->unlink($a[0], $a[1], $a[2]));
                $this->expectOk($removed);
                $want = $removed ? 'removed:1' : 'removed:0';
                $this->check($expected === $want, 'unlink removed=%s, want %s', $removed ? 'true' : 'false', $want);
                return;

            case 'NEIGHBORS':
            case 'IN_NEIGHBORS':
                $keys = $this->caught(fn () => $op === 'NEIGHBORS'
                    ? $this->docs()->neighbors($a[0], $a[1])
                    : $this->docs()->inNeighbors($a[0], $a[1]));
                $this->expectOk($keys);
                $this->checkKeys($keys, $expected);
                return;

            case 'NEIGHBORS_W':
                $weighted = $this->caught(fn () => $this->docs()->neighborsWeighted($a[0], $a[1]));
                $this->expectOk($weighted);
                $this->check(strlen($expected) >= 3 && $expected[0] === 'g' && $expected[1] === '('
                    && substr($expected, -1) === ')',
                    'weighted expectation must be g(...), got %s', $expected);
                $body = substr($expected, 2, -1);
                $pairs = $body === '' ? [] : self::splitTop($body);
                $this->check(count($weighted) === count($pairs),
                    'weighted hits %d, expected %d', count($weighted), count($pairs));
                foreach ($pairs as $i => $pair) {
                    $eq = strrpos($pair, '=');
                    $this->check($eq !== false && $eq > 0, 'weighted pair needs key=val, got %s', $pair);
                    $key = substr($pair, 0, $eq);
                    $vtok = substr($pair, $eq + 1);
                    $this->check($weighted[$i]->key === $key, 'weighted key %s, want %s', $weighted[$i]->key, $key);
                    $this->check($this->doubleMatches($weighted[$i]->weight, $vtok),
                        'weight of %s %.17g vs %s', $key, $weighted[$i]->weight, $vtok);
                }
                return;

            case 'TRAVERSE':
                $keys = $this->caught(fn () => $this->docs()->traverse($a[0], $a[1], $this->parseInt($a[2])));
                $this->expectOk($keys);
                $this->checkKeys($keys, $expected);
                return;
        }

        // ---- geo ----
        switch ($op) {
            case 'GINSERT':
            case 'GINSERT_M':
                if ($op === 'GINSERT_M') { // {lat, lon} map form
                    $loc = ['lat' => $this->parseDouble($a[1]), 'lon' => $this->parseDouble($a[2])];
                } else {
                    $loc = [$this->parseDouble($a[1]), $this->parseDouble($a[2])];
                }
                $this->expectOk($this->caught(fn () => $this->docs()->insert($a[0], ['loc' => $loc])));
                return;

            case 'RADIUS':
            case 'NEAREST':
            case 'BBOX':
                $hits = $this->caught(function () use ($op, $a) {
                    switch ($op) {
                        case 'RADIUS':
                            return $this->docs()->geoWithinRadius($a[0], $this->parseDouble($a[1]), $this->parseDouble($a[2]), $this->parseDouble($a[3]));
                        case 'NEAREST':
                            return $this->docs()->geoNearest($a[0], $this->parseDouble($a[1]), $this->parseDouble($a[2]), $this->parseInt($a[3]));
                        default:
                            return $this->docs()->geoWithinBBox($a[0], $this->parseDouble($a[1]), $this->parseDouble($a[2]), $this->parseDouble($a[3]), $this->parseDouble($a[4]));
                    }
                });
                $this->expectOk($hits);
                $keys = array_map(static fn ($h) => $h->key, $hits);
                $dists = array_map(static fn ($h) => $h->distanceKm, $hits);
                $this->checkKeys($keys, self::keyPart($expected));
                if (($sp = self::suffixPart($expected)) !== '') {
                    $this->check($sp[0] === '|', 'geo suffix must start with |, got %s', $sp);
                    $body = substr($sp, 1);
                    $toks = $body === '' ? [] : self::splitTop($body);
                    $this->check(count($dists) === count($toks), 'distance count %d, expected %d', count($dists), count($toks));
                    foreach ($toks as $i => $tok) {
                        $this->check($this->doubleMatches($dists[$i], $tok), 'hit %d distance %.9g vs %s', $i, $dists[$i], $tok);
                    }
                }
                return;

            case 'BBOX_ERR':
                $err = $this->caught(fn () => $this->docs()->geoWithinBBox($a[0], $this->parseDouble($a[1]), $this->parseDouble($a[2]), $this->parseDouble($a[3]), $this->parseDouble($a[4])));
                $this->expectErr($err, $this->errToken($expected));
                return;
        }

        // ---- schema & indexes ----
        switch ($op) {
            case 'SET_SCHEMA':
                $specs = self::splitTop($args);
                $defs = [];
                foreach ($specs as $spec) {
                    $part = explode('#', $spec);
                    $this->check(count($part) === 4, 'field spec needs name#type#required#unique, got %s', $spec);
                    $defs[] = new FieldDef($part[0], $this->parseFieldType($part[1]), $part[2] === '1', $part[3] === '1');
                }
                $this->expectOk($this->caught(fn () => $this->docs()->setSchema(...$defs)));
                return;

            case 'SCHEMA':
                $tn = [
                    FieldDef::TYPE_ANY => 'any', FieldDef::TYPE_BOOL => 'bool', FieldDef::TYPE_INT => 'int',
                    FieldDef::TYPE_FLOAT => 'float', FieldDef::TYPE_TEXT => 'text', FieldDef::TYPE_BYTES => 'bytes',
                    FieldDef::TYPE_VECTOR => 'vector', FieldDef::TYPE_ARRAY => 'array', FieldDef::TYPE_MAP => 'map',
                ];
                $defs = $this->caught(fn () => $this->docs()->schema());
                $this->expectOk($defs);
                $this->check($defs !== null, 'a schema must be declared first');
                $parts = [];
                foreach ($defs as $f) {
                    $parts[] = sprintf('%s/%s/%d/%d', $f->name, $tn[$f->type], $f->required ? 1 : 0, $f->unique ? 1 : 0);
                }
                $got = implode(',', $parts);
                $this->check($expected === $got, 'schema %s, want %s', $got, $expected);
                return;

            case 'SCHEMA9':
                $names = ['f_any', 'f_bool', 'f_int', 'f_float', 'f_text', 'f_bytes', 'f_vector', 'f_array', 'f_map'];
                $types = [
                    FieldDef::TYPE_ANY, FieldDef::TYPE_BOOL, FieldDef::TYPE_INT, FieldDef::TYPE_FLOAT,
                    FieldDef::TYPE_TEXT, FieldDef::TYPE_BYTES, FieldDef::TYPE_VECTOR, FieldDef::TYPE_ARRAY,
                    FieldDef::TYPE_MAP,
                ];
                $defs = [];
                foreach ($names as $i => $name) {
                    $defs[] = new FieldDef($name, $types[$i], $i === 1, $i === 8);
                }
                $this->expectOk($this->caught(fn () => $this->docs()->setSchema(...$defs)));
                $got = $this->caught(fn () => $this->docs()->schema());
                $this->expectOk($got);
                $this->check($got !== null, 'the 9-field schema must be declared');
                $tags = [];
                foreach ($got as $i => $f) {
                    $this->check($i < 9 && $f->type === $types[$i] && $f->name === $names[$i], 'field %d did not round-trip', $i);
                    $tags[] = (string)$f->type;
                }
                $this->check(count($got) === 9, 'expected exactly 9 fields, saw %d', count($got));
                $joined = implode(',', $tags);
                $this->check($expected === $joined, 'schema9 %s, want %s', $joined, $expected);
                return;

            case 'SCHEMA_ERR':
                $err = $this->caught(fn () => $this->docs()->insert($a[0], $this->lit($a[1])));
                $this->expectErr($err, $this->errToken($expected));
                return;

            case 'IDX_SCALAR':
                $this->expectOk($this->caught(fn () => $this->docs()->createScalarIndex($a[0])));
                return;

            case 'IDX_COMPOUND':
                $this->expectOk($this->caught(fn () => $this->docs()->createCompoundIndex(...self::splitTop($args))));
                return;

            case 'IDX_TEXT':
                $this->expectOk($this->caught(fn () => $this->docs()->createTextIndex($a[0])));
                return;

            case 'IDX_TEXT_DISK':
                $this->expectOk($this->caught(fn () => $this->docs()->createTextIndexOnDisk($a[0])));
                return;

            case 'IDX_GEO':
                $this->expectOk($this->caught(fn () => $this->docs()->createGeoIndex($a[0])));
                return;

            case 'IDX_VEC':
                $this->expectOk($this->caught(fn () => $this->docs()->createVectorIndex($a[0], $this->parseMetric($a[1]))));
                return;

            case 'IDX_VEC_Q':
                $this->expectOk($this->caught(fn () => $this->docs()->createVectorIndexQuantized($a[0], $this->parseMetric($a[1]), $this->parseQuant($a[2]))));
                return;

            case 'IDX_VEC_DISK':
                $this->expectOk($this->caught(fn () => $this->docs()->createVectorIndexOnDisk($a[0], $this->parseMetric($a[1]))));
                return;

            case 'IDX_VEC_DISK_Q':
                $this->expectOk($this->caught(fn () => $this->docs()->createVectorIndexOnDiskQuantized($a[0], $this->parseMetric($a[1]), $this->parseQuant($a[2]))));
                return;

            case 'IDX_PQ':
            case 'IDX_PQ_DISK':
            case 'IDX_PQ_ERR':
                $err = $op === 'IDX_PQ_DISK'
                    ? $this->caught(fn () => $this->docs()->createVectorIndexOnDiskPQ($a[0], $this->parseMetric($a[1]), $this->parseInt($a[2]), $this->parseInt($a[3])))
                    : $this->caught(fn () => $this->docs()->createVectorIndexPQ($a[0], $this->parseMetric($a[1]), $this->parseInt($a[2]), $this->parseInt($a[3])));
                if ($op === 'IDX_PQ_ERR') {
                    $this->expectErr($err, $this->errToken($expected));
                } else {
                    $this->expectOk($err);
                }
                return;
        }

        // ---- admin & persistence ----
        switch ($op) {
            case 'FILEDB':
                $this->openFile($this->dbPath);
                return;

            case 'FILEDB2':
                $this->openFile($this->db2Path);
                return;

            case 'DUMP':
                $this->expectOk($this->caught(fn () => $this->db->dump($this->dumpPath)));
                return;

            case 'LOAD':
                $this->expectOk($this->caught(fn () => $this->db->load($this->dumpPath)));
                return;

            case 'LOAD_RENAMES':
                $err = $this->caught(fn () => $this->db->loadWithRenames($this->dumpPath, [$a[0] => $a[1]]));
                if (str_starts_with($expected, 'err:')) {
                    $this->expectErr($err, $this->errToken($expected));
                } else {
                    $this->expectOk($err);
                }
                return;

            case 'COLLECTIONS':
                $names = $this->caught(fn () => $this->db->collections());
                $this->expectOk($names);
                $this->checkKeys($names, $expected);
                return;

            case 'BACKUP':
                $this->expectOk($this->caught(fn () => $this->db->backup($this->backupPath)));
                return;

            case 'BACKUP_DUP':
                $err = $this->caught(fn () => $this->db->backup($this->backupPath));
                $this->expectErr($err, CorvidException::CODE_BACKUP_TARGET_EXISTS);
                return;

            case 'COMPACT_BUSY':
                $err = $this->caught(fn () => $this->db->compact());
                $this->expectErr($err, CorvidException::CODE_BUSY);
                return;

            case 'COMPACT':
                $this->closeColl(); // quiesce: the derived-handle gate (§4.13)
                $moved = $this->caught(fn () => $this->db->compact());
                $this->expectOk($moved);
                $this->docs(); // re-acquire for subsequent lines
                return;

            case 'REOPEN':
                $path = $this->dbPath;
                $this->closeDb();
                $this->db = Db::open($path);
                $this->check($this->db !== null, 'reopen of %s failed', $path);
                $this->docs();
                return;
        }

        $this->fail('unknown OP %s', $op);
    }

    /**
     * Walk a dotted path over a decoded PHP value (the binding's shape of
     * the borrowed nested-child reads: the decode copies at the boundary,
     * so the walk is over fully-owned PHP data).
     */
    private function walkPath(mixed $v, string $path): mixed
    {
        $cur = $v;
        foreach (explode('.', $path) as $seg) {
            if (is_array($cur) && array_key_exists($seg, $cur)) {
                $cur = $cur[$seg];
            } elseif (is_array($cur) && is_int($seg) === false && ctype_digit($seg) && array_key_exists((int)$seg, $cur)) {
                $cur = $cur[(int)$seg];
            } elseif (is_array($cur) && ctype_digit($seg)) {
                $idx = (int)$seg;
                $list = array_values($cur);
                if ($idx < count($list)) {
                    $cur = $list[$idx];
                } else {
                    return null;
                }
            } else {
                return null;
            }
        }
        return $cur;
    }

    // ---------------------------------------------------------------
    // fixture-file driver
    // ---------------------------------------------------------------

    public function runFixture(string $path): int
    {
        $data = file_get_contents($path);
        if ($data === false) {
            throw new RuntimeException("cannot open fixture $path");
        }
        $base = basename($path);
        $stem = substr($base, 0, -strlen('.txt'));
        $dir = sys_get_temp_dir() . '/corvid-php-golden-' . $stem . '-' . getmypid();
        if (is_dir($dir)) {
            $this->rrmdir($dir);
        }
        mkdir($dir, 0777, true);

        $this->file = $path;
        $this->workdir = $dir;
        $this->dbPath = "$dir/$stem.redb";
        $this->db2Path = "$dir/$stem-2.redb";
        $this->dumpPath = "$dir/$stem.dump";
        $this->backupPath = "$dir/$stem.backup.redb";
        $this->lastAutoID = 0;

        // values.txt runs against no db; every other file starts
        // in-memory (admin/persist switch to file dbs via their OPs).
        if (basename($path) !== 'values.txt') {
            $this->openMemory();
        }

        try {
            $lines = explode("\n", $data);

            // `lines` is counted in an INDEPENDENT pre-scan (the same rule
            // the Rust/C/Go drivers apply), so a dispatch loop that skips a
            // counted line diverges from `executed` below.
            $counted = 0;
            foreach ($lines as $raw) {
                $first = 0;
                while ($first < strlen($raw) && ($raw[$first] === ' ' || $raw[$first] === "\r")) {
                    $first++;
                }
                if ($first < strlen($raw) && $raw[$first] !== '#') {
                    $counted++;
                }
            }

            $executed = 0;
            foreach ($lines as $raw) {
                $line = rtrim($raw, "\r");
                if ($line === '' || $line[0] === '#') {
                    continue;
                }
                $this->line = $executed + 1;
                $this->op = $line; // refined below; kept whole for the unknown-OP message

                // OP \t ARGS \t EXPECTED
                $parts = explode("\t", $line, 3);
                $op = $parts[0];
                $args = $parts[1] ?? '';
                $expected = $parts[2] ?? '';
                $this->op = $op;
                $this->runLine($op, $args, $expected);
                $executed++;
            }

            if ($executed !== $counted) {
                $this->fail('dispatched %d of %d counted executable lines', $executed, $counted);
            }
            echo sprintf('SMOKE %s lines=%d executed=%d', $path, $counted, $executed), "\n";
            return $counted;
        } finally {
            $this->closeDb();
            $this->rrmdir($dir);
        }
    }

    private function rrmdir(string $dir): void
    {
        if (!is_dir($dir)) {
            return;
        }
        foreach (scandir($dir) ?: [] as $f) {
            if ($f === '.' || $f === '..') {
                continue;
            }
            $p = "$dir/$f";
            if (is_dir($p)) {
                $this->rrmdir($p);
            } else {
                @unlink($p);
            }
        }
        @rmdir($dir);
    }
}

/**
 * Replay the full golden suite (all eight fixtures, the engine's order).
 * Throws on the first failing expectation; prints one SMOKE line per
 * fixture on success. Returns the total executed line count.
 */
function run_golden_suite(string $repoRoot): int
{
    if (\Corvid\ffiVersion() !== 1) {
        throw new RuntimeException('FAIL wrong FFI_VERSION ' . \Corvid\ffiVersion());
    }
    $h = new Harness();
    $total = 0;
    foreach (['values', 'mutations', 'queries', 'schema', 'geo', 'graph', 'admin', 'persist'] as $name) {
        $path = "$repoRoot/golden/$name.txt";
        $total += $h->runFixture($path);
    }
    return $total;
}
