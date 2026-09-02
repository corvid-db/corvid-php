<?php

/**
 * geo — radius, bbox, and k-nearest with haversine kilometres.
 *
 * Four cities as [lat, lon] arrays under a geo index; every hit carries
 * its distance from the query point.
 *
 * Run: php -d extension=<path>/corvid.so examples/geo.php
 */

$cities = [
    ['berlin', 52.52, 13.40],
    ['potsdam', 52.40, 13.06],
    ['hamburg', 53.55, 9.99],
    ['munchen', 48.14, 11.58],
];

function show(string $label, array $hits): void
{
    $parts = [];
    foreach ($hits as $h) {
        $parts[] = sprintf('%s %.6fkm', $h->key, $h->distanceKm);
    }
    printf("%-34s [%s]\n", $label, implode(' ', $parts));
}

$db = Corvid\Db::openMemory();
$places = $db->collection('places');

foreach ($cities as [$name, $lat, $lon]) {
    $places->insert($name, [
        'name' => $name,
        'loc' => [$lat, $lon], // the [lat, lon] array encoding
    ]);
}
$places->createGeoIndex('loc');

show('within 600km of Berlin:', $places->geoWithinRadius('loc', 52.52, 13.40, 600.0));
show('bbox 47..55N, 5..15E:', $places->geoWithinBBox('loc', 47, 5, 55, 15));
show('nearest 2 to Berlin:', $places->geoNearest('loc', 52.52, 13.40, 2));
