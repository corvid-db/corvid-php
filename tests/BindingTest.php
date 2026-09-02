<?php

/**
 * BindingTest.php — the extension's own regression tests (the golden
 * suite replays the ENGINE's fixtures; this file pins binding-local
 * contracts the fixtures never express).
 *
 * Covered here:
 *   - scalar-coercion keep-alive: int/float arguments cross
 *     zval_get_string into the ABI as freshly allocated zend_strings;
 *     they must stay alive across the engine call (deleteBatch /
 *     Query::select / createCompoundIndex / loadWithRenames). The
 *     release-before-use this guards against was observable as garbage
 *     field names, keys, and rename targets.
 *   - the encode depth cap (PHP_CORVID_MAX_NESTING = 128, mirroring
 *     corvid::value::MAX_NESTING): deep PHP arrays are rejected with a
 *     clean Corvid\Exception (CODE_ARGUMENT) instead of unbounded C
 *     recursion; the 128 boundary stays round-trippable.
 *   - callback-exception surfacing (§1.6): a throwing scan/update
 *     callback's exception is re-thrown VERBATIM at the call site
 *     (never unwound through C frames) and the engine stays usable.
 */

declare(strict_types=1);

namespace CorvidTests;

use Corvid\Db;
use Corvid\Exception as CorvidException;
use PHPUnit\Framework\TestCase;
use RuntimeException;

final class BindingTest extends TestCase
{
    private string $dir = '';

    protected function setUp(): void
    {
        if (!extension_loaded('corvid')) {
            self::fail('the corvid extension is not loaded (run scripts/build-ext.sh; CI loads modules/corvid.so)');
        }
        $this->dir = sys_get_temp_dir() . '/corvid-php-binding-' . getmypid() . '-' . uniqid();
        mkdir($this->dir, 0777, true);
    }

    protected function tearDown(): void
    {
        foreach (array_reverse($this->globRecursive($this->dir)) as $f) {
            @is_dir($f) ? @rmdir($f) : @unlink($f);
        }
        @rmdir($this->dir);
    }

    /** @return string[] */
    private function globRecursive(string $dir): array
    {
        $out = [];
        foreach (scandir($dir) ?: [] as $f) {
            if ($f === '.' || $f === '..') {
                continue;
            }
            $p = "$dir/$f";
            if (is_dir($p)) {
                $out[] = $p;
                array_push($out, ...$this->globRecursive($p));
            } else {
                $out[] = $p;
            }
        }
        return $out;
    }

    /* ---------------------------------------------------------------
     * scalar-coercion keep-alive (int args through zval_get_string)
     * --------------------------------------------------------------- */

    public function testDeleteBatchAcceptsIntegerKeys(): void
    {
        $c = Db::openMemory()->collection('docs');
        $c->insert('42', ['n' => 1]);
        $c->insert('1337', ['n' => 2]);
        $c->insert('other', ['n' => 3]);

        $this->assertSame(2, $c->deleteBatch(42, 1337), 'int keys must reach the ABI as their decimal strings');
        $this->assertSame(1, $c->len());
        $this->assertSame(1, $c->deleteBatch('other'));
    }

    public function testQuerySelectAcceptsIntegerFieldNames(): void
    {
        $c = Db::openMemory()->collection('docs');
        $c->insert('k', ['42' => 'hit', 'a' => 1]);

        $rows = $c->query()->select(42)->run();
        $this->assertCount(1, $rows);
        $this->assertSame(['42' => 'hit'], $rows[0]->doc, 'int field names must project their decimal-string field');
    }

    public function testCreateCompoundIndexAcceptsIntegerFieldNames(): void
    {
        $c = Db::openMemory()->collection('docs');

        $c->createCompoundIndex(10, 20);
        $c->insert('k', ['10' => 1, '20' => 2]);
        $this->assertSame(1, $c->len(), 'the collection must stay usable after indexing by int field names');
    }

    public function testLoadWithRenamesAcceptsIntegerTargets(): void
    {
        $dump = "$this->dir/int-renames.dump";
        $src = Db::openMemory();
        $src->collection('docs')->insert('a', ['n' => 1]);
        $src->dump($dump);

        $dst = Db::openMemory();
        $dst->loadWithRenames($dump, ['docs' => 42]);
        $this->assertSame(['42'], $dst->collections(), 'the int rename target must arrive as its decimal string');
    }

    /* ---------------------------------------------------------------
     * the encode depth cap (corvid::value::MAX_NESTING = 128)
     * --------------------------------------------------------------- */

    /** @return mixed a list nested $depth levels around 'leaf' */
    private static function nestedList(int $depth): mixed
    {
        $v = 'leaf';
        for ($i = 0; $i < $depth; $i++) {
            $v = [$v];
        }
        return $v;
    }

    public function testEncodingDeeperThanMaxNestingIsRejectedCleanly(): void
    {
        $c = Db::openMemory()->collection('docs');

        try {
            $c->insert('deep', self::nestedList(129));
            $this->fail('a 129-deep array must be rejected at encode time');
        } catch (CorvidException $e) {
            $this->assertSame(CorvidException::CODE_ARGUMENT, $e->getCode());
            $this->assertStringContainsString('128', $e->getMessage());
        }
        $this->assertSame(0, $c->len(), 'a rejected encode must write nothing');
    }

    public function testMapNestingBeyondMaxIsRejectedToo(): void
    {
        $v = 'leaf';
        for ($i = 0; $i < 129; $i++) {
            $v = ['k' => $v];
        }
        $c = Db::openMemory()->collection('docs');

        $this->expectException(CorvidException::class);
        $this->expectExceptionCode(CorvidException::CODE_ARGUMENT);
        $c->insert('deepmap', $v);
    }

    public function testMaxNestingBoundaryRoundTrips(): void
    {
        $c = Db::openMemory()->collection('docs');

        $c->insert('boundary', self::nestedList(128));
        $back = $c->get('boundary');

        $depth = 0;
        for ($v = $back; is_array($v); $v = $v[0]) {
            $depth++;
        }
        $this->assertSame(128, $depth, 'the inclusive 128 boundary must round-trip');
        $this->assertSame('leaf', $v);
    }

    /* ---------------------------------------------------------------
     * callback-exception surfacing (§1.6): verbatim, engine usable
     * --------------------------------------------------------------- */

    public function testScanCallbackExceptionSurfacesVerbatim(): void
    {
        $c = Db::openMemory()->collection('docs');
        $c->insert('a', ['n' => 1]);
        $c->insert('b', ['n' => 2]);
        $marker = new RuntimeException('scan-marker: stop the walk');

        try {
            $c->scan(static function () use ($marker): void {
                throw $marker;
            });
            $this->fail('the scan callback exception must surface at the call site');
        } catch (RuntimeException $e) {
            $this->assertSame($marker, $e, 'the ORIGINAL exception must be re-thrown, not a wrap');
        }

        // the engine must still be usable after the aborted scan
        $seen = [];
        $c->scan(static function (string $key) use (&$seen): bool {
            $seen[] = $key;
            return true;
        });
        $this->assertSame(['a', 'b'], $seen);
    }

    public function testUpdateCallbackExceptionSurfacesVerbatim(): void
    {
        $c = Db::openMemory()->collection('docs');
        $c->insert('a', ['n' => 1]);
        $marker = new RuntimeException('update-marker: abort the write');

        try {
            $c->update('a', static function () use ($marker): void {
                throw $marker;
            });
            $this->fail('the update callback exception must surface at the call site');
        } catch (RuntimeException $e) {
            $this->assertSame($marker, $e, 'the ORIGINAL exception must be re-thrown, not a wrap');
        }

        $this->assertSame(['n' => 1], $c->get('a'), 'an aborted update must write nothing');
        $c->update('a', static fn ($cur) => ['n' => $cur['n'] + 1]);
        $this->assertSame(['n' => 2], $c->get('a'), 'the engine must stay usable after the aborted update');
    }
}
