dnl config.m4 — the unix (phpize) build for the corvid PHP extension.
dnl
dnl The extension links the FETCHED, sha256-verified corvid artifacts
dnl (fetch.sh normalizes corvid.h + the cdylib into deps/current — never
dnl vendored in git). Point --with-corvid at that directory; the default
dnl looks in the repo's deps/current relative to this extension dir.

PHP_ARG_WITH([corvid],
  [path to the fetched corvid FFI artifacts (run fetch.sh; defaults to <repo>/deps/current)],
  [PHP_CORVID],
  [no])

if test "$PHP_CORVID" = "no"; then
  AC_MSG_CHECKING([for fetched corvid artifacts in the default location])
  for d in "$PHP_EXT_BUILDDIR/../../deps/current" "$srcdir/../../deps/current" "$PHP_EXT_SRCDIR/../../deps/current"; do
    if test -f "$d/corvid.h"; then
      PHP_CORVID="$d"
      break
    fi
  done
  if test "$PHP_CORVID" = "no"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([corvid artifacts not found. Run ./fetch.sh in the repo root (downloads + sha256-verifies the pinned engine release into deps/current), or pass --with-corvid=<dir holding corvid.h + libcorvid>])
  fi
  AC_MSG_RESULT([$PHP_CORVID])
fi

AC_MSG_CHECKING([for corvid.h and the cdylib in $PHP_CORVID])
if test ! -f "$PHP_CORVID/corvid.h"; then
  AC_MSG_ERROR([corvid.h not found in $PHP_CORVID])
fi
if test ! -f "$PHP_CORVID/libcorvid.dylib" && test ! -f "$PHP_CORVID/libcorvid.so"; then
  AC_MSG_ERROR([neither libcorvid.dylib nor libcorvid.so found in $PHP_CORVID])
fi
AC_MSG_RESULT([found])

dnl The rpath makes the built corvid.so resolve libcorvid at load time
dnl (the darwin dylib's install name is @rpath/libcorvid.dylib; the linux
dnl .so carries its SONAME) — CI and local builds load the extension
dnl directly from the phpize build tree.
PHP_CORVID_RPATH="`cd \"$PHP_CORVID\" && pwd`"
PHP_ADD_INCLUDE([$PHP_CORVID])

PHP_CHECK_LIBRARY(corvid, corvid_ffi_version,
  [],
  [AC_MSG_ERROR([cannot link libcorvid in $PHP_CORVID])],
  [-L$PHP_CORVID])

PHP_ADD_LIBRARY_WITH_PATH([corvid], [$PHP_CORVID], [CORVID_SHARED_LIBADD])
EXTRA_LDFLAGS="$EXTRA_LDFLAGS -Wl,-rpath,$PHP_CORVID_RPATH"

PHP_NEW_EXTENSION([corvid], [corvid.c], [$ext_shared])
PHP_INSTALL_HEADERS([ext/corvid], [php_corvid.h])
PHP_SUBST([CORVID_SHARED_LIBADD])
