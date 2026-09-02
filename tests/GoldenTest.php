<?php

/**
 * GoldenTest.php — the PHPUnit front of the golden-suite port.
 *
 * The harness (tests/golden_harness.php) replays the engine's golden
 * fixtures (267 executable lines across 8 files) through THIS binding's
 * public PHP API against the DOWNLOADED cdylib; this file just gives
 * each fixture its own test so a failure names the fixture at a glance.
 */

declare(strict_types=1);

namespace CorvidTests;

use CorvidGolden\Harness;
use PHPUnit\Framework\TestCase;

final class GoldenTest extends TestCase
{
    public static function setUpBeforeClass(): void
    {
        if (!extension_loaded('corvid')) {
            self::fail('the corvid extension is not loaded (run scripts/build-ext.sh; CI loads modules/corvid.so)');
        }
        self::assertSame(1, \Corvid\ffiVersion(), 'bindings verify the ABI version before anything else');
    }

    /** @return iterable<string, array{string}> */
    public static function fixtureProvider(): iterable
    {
        foreach (['values', 'mutations', 'queries', 'schema', 'geo', 'graph', 'admin', 'persist'] as $name) {
            yield $name => ["$name.txt"];
        }
    }

    /** @dataProvider fixtureProvider */
    public function testGoldenFixture(string $fixture): void
    {
        $h = new Harness();
        $counted = $h->runFixture(dirname(__DIR__) . '/golden/' . $fixture);
        $this->assertGreaterThan(0, $counted);
    }
}
