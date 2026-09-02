# corvid-php — developer entry points.
#
# The engine artifacts are NOT vendored: `make deps` fetches the pinned
# release (fetch.sh / fetch.ps1), sha256-verifies it against the
# release's checksums.txt, and normalizes corvid.h + the cdylib into
# deps/current, which config.m4 points the extension build at. After
# `make deps`, `make ext test examples` work offline.

PHP ?= php
PHPUNIT ?= php phpunit.phar

.PHONY: deps ext test examples all gate clean

deps:            ## fetch + verify the pinned engine artifacts into deps/
	./fetch.sh

ext:             ## phpize-configure-make the extension (requires deps)
	@./scripts/build-ext.sh

test:            ## run the golden suite via PHPUnit (requires ext)
	$(PHPUNIT) --configuration phpunit.xml.dist

test-direct:     ## run the golden suite via the direct driver (no PHPUnit deps)
	$(PHP) -d extension=ext/corvid/modules/corvid.so tests/run-golden.php

examples:        ## run the six-example tour (requires ext)
	@for ex in quickstart hybrid vector_index text_search graph geo; do \
	    echo "== examples/$$ex.php =="; \
	    $(PHP) -d extension=ext/corvid/modules/corvid.so examples/$$ex.php || exit 1; \
	done

gate:            ## the surface manifest gate (docs/SURFACE.tsv vs the pin)
	./scripts/surface-gate.sh

all: ext test examples gate

clean:           ## drop fetched artifacts and the phpize build tree
	rm -rf deps
	cd ext/corvid && [ -f Makefile ] && make clean >/dev/null 2>&1 || true
	rm -rf ext/corvid/.libs ext/corvid/modules ext/corvid/config.h \
	       ext/corvid/config.nice ext/corvid/Makefile ext/corvid/autom4te.cache \
	       ext/corvid/build ext/corvid/configure ext/corvid/aclocal.m4 \
	       ext/corvid/install-sh ext/corvid/ltmain.sh ext/corvid/missing \
	       ext/corvid/mkinstalldirs ext/corvid/run-tests.php || true
