<?php

/**
 * tests/bootstrap.php — PHPUnit bootstrap: require the harness lib and
 * make sure the extension is loadable (CI runs phpunit under
 * `-d extension=.../corvid.so`; a developer-installed extension works
 * too).
 */

declare(strict_types=1);

require __DIR__ . '/golden_harness.php';

if (!extension_loaded('corvid')) {
    $mod = dirname(__DIR__) . '/ext/corvid/modules/corvid.so';
    if (is_file($mod)) {
        dl('corvid.so') || fwrite(STDERR, "note: extension not loaded; run scripts/build-ext.sh and use -d extension=$mod\n");
    }
}
