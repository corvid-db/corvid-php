<?php

/**
 * run-golden.php — the direct golden-suite driver.
 *
 * Same suite as tests/GoldenTest.php (PHPUnit), runnable on builds where
 * PHPUnit's extension set is unavailable — notably the CI ZTS leg
 * (php:8.x-zts containers ship no dom/mbstring stack for the phar). One
 * process, all eight fixtures, exit code 0 on green, 1 with the first
 * failure named file:line + OP.
 */

declare(strict_types=1);

require __DIR__ . '/golden_harness.php';

$root = dirname(__DIR__);
try {
    $total = \CorvidGolden\run_golden_suite($root);
    echo "golden: OK — $total executable lines, 8 fixtures\n";
    exit(0);
} catch (Throwable $e) {
    fwrite(STDERR, $e->getMessage() . "\n");
    exit(1);
}
