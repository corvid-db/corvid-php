<?php

/**
 * text_search — BM25 incl. CJK bigram segmentation, plus the v0.3.0
 * direct phrase search.
 *
 * Six notes (three English, three CJK) under an inverted text index;
 * BM25 queries, then PHRASE queries — consecutive, in-order analyzed
 * tokens, stop words collapsing out of adjacency. Reversed word order
 * matches nothing.
 *
 * Run: php -d extension=<path>/corvid.so examples/text_search.php
 */

use Corvid\Collection;

$corpus = [
    ['n1', 'the quick brown fox jumps over the lazy dog'],
    ['n2', 'a quick red fox leaps over a sleeping dog'],
    ['n3', 'slow green turtle crosses the road'],
    ['n4', '东京是一座巨大的城市'],  // Tokyo is a huge city
    ['n5', '大阪是关西最大的城市'],  // Osaka is Kansai's biggest city
    ['n6', '机器学习正在改变数据库'], // ML is changing databases
];

function search(Collection $notes, string $query, string $label): void
{
    $rows = $notes->query()->text('body', $query, 3)->run();
    printf('%-28s ->', $label);
    foreach ($rows as $r) {
        printf(' %s(%.6f)', $r->key, $r->score);
    }
    echo "\n";
}

function phrase(Collection $notes, string $query, string $label): void
{
    $rows = $notes->phraseSearch('body', $query, 3);
    printf('%-28s ->', $label);
    foreach ($rows as $r) {
        printf(' %s(%.6f)', $r->key, $r->score);
    }
    echo "\n";
}

$db = Corvid\Db::openMemory();
$notes = $db->collection('notes');
foreach ($corpus as [$key, $body]) {
    $notes->insert($key, ['body' => $body]);
}
$notes->createTextIndex('body');

search($notes, 'quick fox', 'bm25 "quick fox":');
search($notes, 'quick dog', 'bm25 "quick dog":');
search($notes, '城市', 'bm25 CJK 城市 (city):');
search($notes, '数据库', 'bm25 CJK 数据库 (database):');

phrase($notes, 'fox jumps over', 'phrase "fox jumps over":');
phrase($notes, 'over jumps fox', 'phrase "over jumps fox" (reversed — no match):');
phrase($notes, 'leaps over a sleeping', 'phrase with stop words collapsed:');
