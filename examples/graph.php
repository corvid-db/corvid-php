<?php

/**
 * graph — directed edges, weighted routes, BFS traversal, delete
 * cascade.
 *
 * Nodes linked by parent_of edges (one edge dangles on a key that never
 * exists as a document), weighted route edges, then the deletes: both a
 * real document and the never-a-document key cascade their edges in the
 * same transaction.
 *
 * Run: php -d extension=<path>/corvid.so examples/graph.php
 */

use Corvid\Collection;

// docs:begin:graph
function show(string $label, array $keys): void
{
    printf("%-36s [%s]\n", $label, implode(' ', $keys));
}

$db = Corvid\Db::openMemory();
$nodes = $db->collection('nodes');

foreach (['ga', 'gb', 'gc'] as $key) {
    $nodes->insert($key, ['n' => $key]);
}

$nodes->link('ga', 'parent_of', 'gb');
$nodes->link('ga', 'parent_of', 'gc');
$nodes->link('gb', 'parent_of', 'gd'); // gd never exists as a document
$nodes->linkWeighted('ga', 'route', 'gb', 2.5);
$nodes->linkWeighted('ga', 'route', 'gd', 0.75);

show('neighbors(ga)', $nodes->neighbors('ga', 'parent_of'));
show('in_neighbors(gb)', $nodes->inNeighbors('gb', 'parent_of'));

$routes = $nodes->neighborsWeighted('ga', 'route');
$parts = [];
foreach ($routes as $r) {
    $parts[] = sprintf('%s=%.2f', $r->key, $r->weight);
}
printf("%-36s [%s]\n", 'routes from ga (weighted):', implode(' ', $parts));

show('traverse(ga, 1 hop)', $nodes->traverse('ga', 'parent_of', 1));
show('traverse(ga, 2 hops)', $nodes->traverse('ga', 'parent_of', 2));

// Delete cascade: remove gc (a document) and gd (never a document).
printf("delete gc: existed = %s\n", $nodes->delete('gc') ? 'true' : 'false');
printf("delete gd: existed = %s (never a document; its edges still cascade)\n", $nodes->delete('gd') ? 'true' : 'false');

show('neighbors(ga) after deletes', $nodes->neighbors('ga', 'parent_of'));
show('neighbors(gb) after deletes', $nodes->neighbors('gb', 'parent_of'));
show('traverse(ga, 2 hops) after', $nodes->traverse('ga', 'parent_of', 2));
// docs:end:graph
