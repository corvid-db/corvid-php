/* php_corvid.h — corvid-php: the PHP extension over libcorvid's typed C ABI.
 *
 * The binding's plan and lifecycle rulings live in docs/PLAN.md; the value
 * mapping table and handle lifetimes documented there are implemented in
 * corvid.c. MIT licensed, same line as the engine.
 */

#ifndef PHP_CORVID_H
# define PHP_CORVID_H

# include "php.h"

# define PHP_CORVID_EXTNAME  "corvid"
# define PHP_CORVID_VERSION  "0.3.0"
# define PHP_CORVID_NS       "Corvid"

extern zend_module_entry corvid_module_entry;
# ifdef PHP_WIN32
#  define PHP_CORVID_API __declspec(dllexport)
# elif defined(__GNUC__) && __GNUC__ >= 4
#  define PHP_CORVID_API __attribute__((visibility("default")))
# else
#  define PHP_CORVID_API
# endif

# ifdef ZTS
#  include "TSRM.h"
# endif

/* Fetched artifacts (deps/current via --with-corvid=<dir>, config.m4). */
# include "corvid.h"

PHP_MINIT_FUNCTION(corvid);
PHP_MSHUTDOWN_FUNCTION(corvid);
PHP_MINFO_FUNCTION(corvid);

/* Corvid\ffiVersion(): int — the ABI version gate (FFI.md §4.1). */
PHP_FUNCTION(corvid_ffi_version);

#endif /* PHP_CORVID_H */
