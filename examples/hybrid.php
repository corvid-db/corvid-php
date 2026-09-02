<?php

/**
 * hybrid — the flagship: filter + vector + BM25, RRF fusion, MMR
 * rerank, limit.
 *
 * Hybrid retrieval over a 4-document corpus: a pre-ranking `kind`
 * filter, a vector (ANN) source and a BM25 text source, both
 * contributing top-2 candidate lists, fused with Reciprocal Rank
 * Fusion (k = 60) and reranked for diversity with MMR (lambda = 1.0),
 * capped at 2 rows. The printed scores are RRF rank sums: s1 is rank 1
 * of both sources (1/61 + 1/61 = 2/61), s3 rank 2 of both (2/62).
 *
 * Run: php -d extension=<path>/corvid.so examples/hybrid.php
 */

// docs:begin:hybrid
$db = Corvid\Db::openMemory();
$docs = $db->collection('docs');

$docs->insert('s1', [
    'kind' => 'doc', 'body' => 'rust embedded database',
    'v' => new Corvid\Vector([1.0, 0.0]),
]);
$docs->insert('s2', [
    'kind' => 'doc', 'body' => 'python web frameworks',
    'v' => new Corvid\Vector([0.0, 1.0]),
]);
$docs->insert('s3', [
    'kind' => 'doc', 'body' => 'rust again database',
    'v' => new Corvid\Vector([0.9, 0.1]),
]);
$docs->insert('m1', ['kind' => 'meta']); // filtered out below

// The flagship query: filter + vector + text, RRF + MMR + limit.
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
// docs:end:hybrid
