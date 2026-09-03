<?php

/**
 * vector_index — the three index families vs the exact scan, plus a
 * close/reopen.
 *
 * One corpus under three fields: in-memory HNSW, on-disk HNSW, and
 * binary-quantized HNSW — each answering the same top-4 kNN the exact
 * scan produces (the quantized lane trades recall for ~32x size). The
 * file-backed database is reopened at the end: the on-disk graph
 * reloads without a rebuild and answers again.
 *
 * Run: php -d extension=<path>/corvid.so examples/vector_index.php
 */

use Corvid\Metric;
use Corvid\Quant;
use Corvid\Vector;

// docs:begin:vector_index
$path = sys_get_temp_dir() . '/corvid-php-example-vector-index.redb';
@unlink($path); // reruns start clean (single-file db)

$corpus = [
    ['k0', [1.0, 0.0, 0.0, 0.0]], // nearest
    ['k1', [0.95, 0.05, 0.0, 0.0]],
    ['k2', [0.0, 1.0, 0.0, 0.0]],
    ['k3', [0.0, 0.9, 0.1, 0.0]],
    ['k4', [0.0, 0.0, 1.0, 0.0]],
    ['k5', [0.7, 0.7, 0.0, 0.0]],
    ['k6', [0.0, 0.0, 0.0, 1.0]],
    ['k7', [0.98, 0.02, 0.0, 0.0]],
];
$probe = [1.0, 0.0, 0.0, 0.0];

function runQuery(Corvid\Collection $items, string $field, bool $approx, string $label, array $probe): void
{
    $q = $items->query()->vector($field, new Corvid\Vector($probe), 4, Corvid\Metric::COSINE);
    if ($approx) {
        $q->approx();
    }
    $rows = $q->run();
    printf('%-38s', $label);
    foreach ($rows as $r) {
        printf(' %s(%.6f)', $r->key, $r->score);
    }
    echo "\n";
}

$db = Corvid\Db::open($path);
$items = $db->collection('items');
foreach ($corpus as [$key, $v]) {
    $items->insert($key, [
        'v_mem' => new Corvid\Vector($v),
        'v_disk' => new Corvid\Vector($v),
        'v_q' => new Corvid\Vector($v),
    ]);
}
$items->createVectorIndex('v_mem', Corvid\Metric::COSINE);
$items->createVectorIndexOnDisk('v_disk', Corvid\Metric::COSINE);
$items->createVectorIndexQuantized('v_q', Corvid\Metric::COSINE, Corvid\Quant::BINARY);

echo "top-4 nearest to (1,0,0,0) under cosine:\n";
runQuery($items, 'v_mem', false, 'exact (scan):', $probe);
runQuery($items, 'v_mem', true, 'ann in-memory HNSW:', $probe);
runQuery($items, 'v_disk', true, 'ann on-disk HNSW:', $probe);
runQuery($items, 'v_q', true, 'ann binary-quantized:', $probe);
echo "(the quantized lane trades recall for a ~32x smaller index)\n";

unset($items); // free the derived handle before close
$db->close();

// Reopen: the on-disk graph reloads (no rebuild) and answers again.
$db = Corvid\Db::open($path);
$items = $db->collection('items');
runQuery($items, 'v_disk', true, 'ann on-disk after reopen:', $probe);
// docs:end:vector_index
