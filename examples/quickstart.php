<?php

/**
 * quickstart — the README tour as a runnable file.
 *
 * Open an in-memory database, create a collection, insert three small
 * documents carrying 2-d embeddings, run a kNN vector query under
 * cosine, and print the ranked rows. Handles die with refcount; nothing
 * to close by hand.
 *
 * Run: php -d extension=<path>/corvid.so examples/quickstart.php
 */

use Corvid\Db;
use Corvid\Metric;
use Corvid\Vector;

// docs:begin:quickstart
$db = Corvid\Db::openMemory();
$docs = $db->collection('docs');

$docs->insert('p1', [
    'title' => 'rust embedded database',
    'kind' => 'doc',
    'v' => new Corvid\Vector([1.0, 0.0]),
]);
$docs->insert('p2', [
    'title' => 'python web frameworks',
    'kind' => 'doc',
    'v' => new Corvid\Vector([0.0, 1.0]),
]);
$docs->insert('p3', [
    'title' => 'rust again database',
    'kind' => 'doc',
    'v' => new Corvid\Vector([0.9, 0.1]),
]);

// kNN: the 3 nearest documents to (1, 0) under cosine. The builder
// methods chain; run() consumes the builder and every row carries its
// decoded document.
$rows = $docs->query()
    ->vector('v', new Corvid\Vector([1.0, 0.0]), 3, Corvid\Metric::COSINE)
    ->run();

foreach ($rows as $rank => $r) {
    printf("%d. %s score=%.6f %s\n", $rank + 1, $r->key, $r->score, $r->doc['title']);
}
// docs:end:quickstart
