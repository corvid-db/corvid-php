/* corvid.c — the corvid-php extension: PHP classes over libcorvid's typed
 * C ABI (docs/FFI.md, 124 symbols at engine v0.3.0).
 *
 * The rulings this file implements are locked in docs/PLAN.md:
 *
 *   - lifecycle: class entries in MINIT (per process), nothing in
 *     RINIT/MSHUTDOWN (the ABI has no global init/teardown); handles are
 *     PHP objects freed in their object destructors (corvid_close /
 *     corvid_collection_free / guarded corvid_query_free +
 *     corvid_pred_free); cursors and ABI buffers never escape a call —
 *     they are walked/copied/freed inside the producing wrapper;
 *   - error discipline: every wrapper reads corvid_last_error_code /
 *     _message IMMEDIATELY after the failing call, in the same C
 *     function, on the same thread (FFI.md §3's thread-local slot), and
 *     throws Corvid\Exception with that code + message;
 *   - callbacks (§1.6): update/scan callables run through C trampolines;
 *     exceptions are caught at the trampoline boundary (never unwound
 *     through C frames), the ABI's abort/stop contract is honored, and
 *     the exception is re-thrown after the engine call returns;
 *   - value mapping: null/bool/int/float/string(text)/Corvid\Bytes(bytes)/
 *     array(array|map)/Corvid\Vector(vector) — docs/PLAN.md's table.
 *
 * Copyright (c) 2026 Rocky — MIT, see LICENSE.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php_corvid.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

/* ------------------------------------------------------------------ */
/* object layouts                                                      */
/* ------------------------------------------------------------------ */

typedef struct _php_corvid_db {
	corvid_db      *handle;
	zend_object     std;
} php_corvid_db;

typedef struct _php_corvid_coll {
	corvid_coll    *handle;
	zend_object     std;
} php_corvid_coll;

typedef struct _php_corvid_query {
	corvid_query   *handle;
	zend_object     std;
} php_corvid_query;

typedef struct _php_corvid_pred {
	corvid_pred    *handle;
	zend_object     std;
} php_corvid_pred;

#ifndef XtOffsetOf
# define XtOffsetOf(s, m) offsetof(s, m)
#endif

static inline php_corvid_db *php_corvid_db_from(zend_object *o) {
	return (php_corvid_db *)((char *)o - XtOffsetOf(php_corvid_db, std));
}
static inline php_corvid_coll *php_corvid_coll_from(zend_object *o) {
	return (php_corvid_coll *)((char *)o - XtOffsetOf(php_corvid_coll, std));
}
static inline php_corvid_query *php_corvid_query_from(zend_object *o) {
	return (php_corvid_query *)((char *)o - XtOffsetOf(php_corvid_query, std));
}
static inline php_corvid_pred *php_corvid_pred_from(zend_object *o) {
	return (php_corvid_pred *)((char *)o - XtOffsetOf(php_corvid_pred, std));
}

#define Z_CORVID_DB_P(zv)      php_corvid_db_from(Z_OBJ_P(zv))
#define Z_CORVID_COLL_P(zv)    php_corvid_coll_from(Z_OBJ_P(zv))
#define Z_CORVID_QUERY_P(zv)   php_corvid_query_from(Z_OBJ_P(zv))
#define Z_CORVID_PRED_P(zv)    php_corvid_pred_from(Z_OBJ_P(zv))

static zend_class_entry *corvid_exception_ce;
static zend_class_entry *corvid_db_ce;
static zend_class_entry *corvid_coll_ce;
static zend_class_entry *corvid_query_ce;
static zend_class_entry *corvid_pred_ce;
static zend_class_entry *corvid_field_ce;
static zend_class_entry *corvid_row_ce;
static zend_class_entry *corvid_page_ce;
static zend_class_entry *corvid_geohit_ce;
static zend_class_entry *corvid_whit_ce;
static zend_class_entry *corvid_group_ce;
static zend_class_entry *corvid_fielddef_ce;
static zend_class_entry *corvid_bytes_ce;
static zend_class_entry *corvid_vector_ce;
static zend_class_entry *corvid_values_ce;
static zend_class_entry *corvid_metric_ce;
static zend_class_entry *corvid_quant_ce;

static zend_object_handlers php_corvid_db_handlers;
static zend_object_handlers php_corvid_coll_handlers;
static zend_object_handlers php_corvid_query_handlers;
static zend_object_handlers php_corvid_pred_handlers;

/* ------------------------------------------------------------------ */
/* error discipline (docs/PLAN.md): read the thread-local last error    */
/* IMMEDIATELY, on the same thread, and throw.                          */
/* ------------------------------------------------------------------ */

static zend_never_inline void php_corvid_throw(int code, const char *message)
{
	zend_throw_exception(corvid_exception_ce, message ? message : "(no message)", code);
}

/* Throw from the ABI's thread-local last-error slot. The fallback covers
 * a CORVID_ERR with nothing recorded (not expected, never silent). */
static zend_never_inline void php_corvid_throw_last(const char *fallback_msg)
{
	corvid_err   code = corvid_last_error_code();
	const char  *msg  = corvid_last_error_message(NULL);

	if (code == CORVID_E_OK) {
		code = CORVID_E_ARGUMENT;
	}
	if (msg == NULL) {
		msg = fallback_msg ? fallback_msg : "corvid call failed with no recorded message";
	}
	php_corvid_throw((int)code, msg);
}

static zend_never_inline void php_corvid_throw_arg(const char *fmt, ...) /* CODE_ARGUMENT */
{
	char    buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	php_corvid_throw((int)CORVID_E_ARGUMENT, buf);
}

/* ------------------------------------------------------------------ */
/* UTF-8 validation (§1.5: engine text/map keys are Rust Strings)       */
/* ------------------------------------------------------------------ */

static bool php_corvid_utf8_valid(const char *s, size_t len)
{
	size_t i = 0;

	while (i < len) {
		unsigned char c = (unsigned char)s[i];
		size_t         need;
		uint32_t       cp;

		if (c < 0x80) { i++; continue; }
		if (c >= 0xC2 && c <= 0xDF)      { need = 1; cp = c & 0x1F; }
		else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
		else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
		else { return false; }               /* continuation or overlong lead byte */

		if (i + need >= len) { return false; } /* truncated sequence */
		for (size_t k = 1; k <= need; k++) {
			unsigned char cc = (unsigned char)s[i + k];
			if ((cc & 0xC0) != 0x80) { return false; }
			cp = (cp << 6) | (cc & 0x3F);
		}
		if (c == 0xE0 && cp < 0x800)      { return false; } /* overlong */
		if (c == 0xF0 && cp < 0x10000)    { return false; } /* overlong */
		if (cp >= 0xD800 && cp <= 0xDFFF) { return false; } /* surrogate */
		if (cp > 0x10FFFF)                { return false; }
		i += need + 1;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* value mapping: PHP zval → corvid_value (encode; throws, NULL=fail)   */
/* ------------------------------------------------------------------ */

/* Container-depth cap for the encode recursion. Mirrors the engine's
 * corvid::value::MAX_NESTING (crates/corvid/src/value.rs: "Maximum
 * container nesting accepted by Value::decode ... bounds recursion so a
 * crafted payload errors instead of overflowing the stack"). Without it,
 * a deeply nested PHP array would recurse through php_corvid_encode/
 * _encode_array in C and smash the stack — uncatchable from PHP. The
 * boundary is inclusive, accounted exactly like the engine's decoder
 * (the top-level value sits at depth 0; each array/map child descends
 * one): a 128-deep value encodes, a 129-deep one throws a clean
 * CorvidException (CORVID_E_ARGUMENT) instead. */
#define PHP_CORVID_MAX_NESTING 128

static corvid_value *php_corvid_encode(zval *val, uint32_t depth);

static float *php_corvid_floats_from_array(zval *val, size_t *dim_out) /* throws; NULL=fail */
{
	HashTable *ht  = Z_ARRVAL_P(val);
	size_t     n   = zend_hash_num_elements(ht);
	size_t     i   = 0;
	float     *buf;
	zval      *item;

	buf = safe_emalloc(sizeof(float), (n ? n : 1), 0);
	ZEND_HASH_FOREACH_VAL(ht, item) {
		double d;
		switch (Z_TYPE_P(item)) {
			case IS_LONG:   d = (double)Z_LVAL_P(item); break;
			case IS_DOUBLE: d = Z_DVAL_P(item);         break;
			default:
				efree(buf);
				php_corvid_throw_arg("vector values must be an array of numbers");
				return NULL;
		}
		buf[i++] = (float)d;
	} ZEND_HASH_FOREACH_END();
	*dim_out = n;
	return buf;
}

static corvid_value *php_corvid_encode_array(zval *val, uint32_t depth)
{
	HashTable    *ht = Z_ARRVAL_P(val);
	corvid_value *out;
	bool          is_list;

	/* The ruled mapping (docs/PLAN.md): a list-shaped PHP array encodes
	 * as an engine Array; every other array — including the empty one —
	 * encodes as a Map (the document shape). */
	if (zend_array_is_list(ht)) {
		is_list = true;
	} else {
		zend_string *key;
		zend_long    idx, expect = 0;
		bool         strings = false, seq = true;

		ZEND_HASH_FOREACH_KEY(ht, idx, key) {
			if (key != NULL) { strings = true; break; }
			if (idx != expect) { seq = false; break; }
			expect++;
		} ZEND_HASH_FOREACH_END();
		is_list = !strings && seq;
	}

	if (is_list) {
		zval *item;
		out = corvid_value_array_new();
		ZEND_HASH_FOREACH_VAL(ht, item) {
			corvid_value *v = php_corvid_encode(item, depth + 1);
			if (v == NULL) { corvid_value_free(out); return NULL; }
			corvid_value_array_push(out, v); /* consumes v unconditionally (§8) */
		} ZEND_HASH_FOREACH_END();
		return out;
	}

	{
		zend_string *key;
		zend_long    idx;
		zval        *item;
		out = corvid_value_map_new();
		ZEND_HASH_FOREACH_KEY_VAL(ht, idx, key, item) {
			zend_string  *kcopy;
			corvid_value *v = php_corvid_encode(item, depth + 1);

			if (v == NULL) { corvid_value_free(out); return NULL; }
			if (key != NULL) {
				if (!php_corvid_utf8_valid(ZSTR_VAL(key), ZSTR_LEN(key))) {
					corvid_value_free(v);
					corvid_value_free(out);
					php_corvid_throw_arg("map key is not valid UTF-8");
					return NULL;
				}
				kcopy = zend_string_copy(key);
			} else {
				kcopy = zend_long_to_str(idx); /* PHP int key → decimal string */
			}
			corvid_value_map_put(out, ZSTR_VAL(kcopy), ZSTR_LEN(kcopy), v); /* consumes v (§8) */
			zend_string_release(kcopy);
		} ZEND_HASH_FOREACH_END();
	}
	return out;
}

static corvid_value *php_corvid_encode(zval *val, uint32_t depth)
{
	if (depth > PHP_CORVID_MAX_NESTING) {
		php_corvid_throw_arg("value nests deeper than %d (corvid::value::MAX_NESTING)",
			(int)PHP_CORVID_MAX_NESTING);
		return NULL;
	}
	switch (Z_TYPE_P(val)) {
		case IS_NULL:   return corvid_value_null();
		case IS_TRUE:   return corvid_value_bool(1);
		case IS_FALSE:  return corvid_value_bool(0);
		case IS_LONG:   return corvid_value_int((int64_t)Z_LVAL_P(val));
		case IS_DOUBLE: return corvid_value_float(Z_DVAL_P(val));
		case IS_STRING: {
			if (!php_corvid_utf8_valid(Z_STRVAL_P(val), Z_STRLEN_P(val))) {
				php_corvid_throw_arg("text value is not valid UTF-8 (use Corvid\\Bytes for raw bytes)");
				return NULL;
			}
			return corvid_value_text(Z_STRVAL_P(val), Z_STRLEN_P(val));
		}
		case IS_ARRAY:
			return php_corvid_encode_array(val, depth);
		case IS_OBJECT: {
			if (instanceof_function(Z_OBJCE_P(val), corvid_bytes_ce)) {
				zval tmp, *pz;
				zend_string *b;
				corvid_value *v;

				pz = zend_read_property(corvid_bytes_ce, Z_OBJ_P(val), "bytes", sizeof("bytes") - 1, 0, &tmp);
				b  = zval_get_string(pz);
				v  = corvid_value_bytes((const uint8_t *)ZSTR_VAL(b), ZSTR_LEN(b));
				zend_string_release(b);
				return v;
			}
			if (instanceof_function(Z_OBJCE_P(val), corvid_vector_ce)) {
				zval tmp, *pz;
				size_t dim = 0;
				float *buf;
				corvid_value *v;

				pz = zend_read_property(corvid_vector_ce, Z_OBJ_P(val), "values", sizeof("values") - 1, 0, &tmp);
				if (Z_TYPE_P(pz) != IS_ARRAY) {
					php_corvid_throw_arg("Corvid\\Vector holds a non-array values property");
					return NULL;
				}
				buf = php_corvid_floats_from_array(pz, &dim);
				if (buf == NULL) { return NULL; }
				v = corvid_value_vector(buf, dim);
				efree(buf);
				return v;
			}
			php_corvid_throw_arg("cannot map a %s instance into a corvid value", ZSTR_VAL(Z_OBJCE_P(val)->name));
			return NULL;
		}
		default:
			php_corvid_throw_arg("cannot map this PHP type into a corvid value");
			return NULL;
	}
}

/* ------------------------------------------------------------------ */
/* value mapping: corvid_value → PHP zval (decode; never fails)         */
/* ------------------------------------------------------------------ */

static void php_corvid_decode_into(corvid_value *v, zval *out);

static void php_corvid_decode_map(const corvid_value *v, zval *out)
{
	corvid_strs *keys = corvid_value_map_keys(v);
	const char  *k;
	size_t       klen;

	array_init(out);
	if (keys == NULL) { return; }
	while (corvid_strs_next(keys, &k, &klen) == 1) {
		zend_string         *kz = zend_string_init(k, klen, 0);
		const corvid_value  *child = corvid_value_map_get(v, k, klen);
		zval                 iz;

		if (child != NULL) {
			php_corvid_decode_into((corvid_value *)child, &iz);
			zend_symtable_update(Z_ARRVAL_P(out), kz, &iz);
		}
		zend_string_release(kz);
	}
	corvid_strs_free(keys);
}

static void php_corvid_decode_into(corvid_value *v, zval *out)
{
	switch (corvid_value_type(v)) {
		case CORVID_TYPE_NULL: {
			ZVAL_NULL(out);
			break;
		}
		case CORVID_TYPE_BOOL: {
			int ok = 0;
			ZVAL_BOOL(out, corvid_value_as_bool(v, &ok) != 0);
			break;
		}
		case CORVID_TYPE_INT: {
			int ok = 0;
			ZVAL_LONG(out, (zend_long)corvid_value_as_int(v, &ok));
			break;
		}
		case CORVID_TYPE_FLOAT: {
			int ok = 0;
			ZVAL_DOUBLE(out, corvid_value_as_float(v, &ok));
			break;
		}
		case CORVID_TYPE_TEXT: {
			size_t len = 0;
			const char *s = corvid_value_text_ref(v, &len);
			if (s == NULL) { ZVAL_NULL(out); break; }
			ZVAL_STR(out, zend_string_init(s, len, 0));
			break;
		}
		case CORVID_TYPE_BYTES: {
			size_t len = 0;
			const uint8_t *b = corvid_value_bytes_ref(v, &len);
			zval bv;

			object_init_ex(out, corvid_bytes_ce);
			if (b == NULL) { len = 0; }
			ZVAL_STRINGL(&bv, (const char *)b, len);
			zend_update_property(corvid_bytes_ce, Z_OBJ_P(out), "bytes", sizeof("bytes") - 1, &bv);
			zval_ptr_dtor(&bv);
			break;
		}
		case CORVID_TYPE_VECTOR: {
			size_t dim = 0;
			const float *f = corvid_value_vector_ref(v, &dim);
			zval arr, zv;
			size_t i;

			object_init_ex(out, corvid_vector_ce);
			array_init_size(&arr, (uint32_t)dim);
			for (i = 0; i < dim; i++) {
				ZVAL_DOUBLE(&zv, (double)f[i]);
				zend_hash_next_index_insert(Z_ARRVAL(arr), &zv);
			}
			zend_update_property(corvid_vector_ce, Z_OBJ_P(out), "values", sizeof("values") - 1, &arr);
			zval_ptr_dtor(&arr);
			break;
		}
		case CORVID_TYPE_ARRAY: {
			size_t n = corvid_value_len(v);
			size_t i;

			array_init_size(out, (uint32_t)n);
			for (i = 0; i < n; i++) {
				const corvid_value *child = corvid_value_array_get(v, i);
				zval iz;
				if (child != NULL) {
					php_corvid_decode_into((corvid_value *)child, &iz);
				} else {
					ZVAL_NULL(&iz);
				}
				zend_hash_next_index_insert(Z_ARRVAL_P(out), &iz);
			}
			break;
		}
		case CORVID_TYPE_MAP:
		default:
			php_corvid_decode_map(v, out);
			break;
	}
}

/* ------------------------------------------------------------------ */
/* cursors → PHP values (walked + freed inside the producing call)      */
/* ------------------------------------------------------------------ */

static void php_corvid_rows_to_array(corvid_rows *rows, zval *out) /* consumes rows */
{
	const uint8_t      *key;
	size_t              klen;
	const corvid_value *doc;
	float               score;
	zval                rowz;

	array_init(out);
	if (rows == NULL) { return; }
	while (corvid_rows_next(rows, &key, &klen, &doc, &score) == 1) {
		zval docz;

		object_init_ex(&rowz, corvid_row_ce);
		zend_update_property_stringl(corvid_row_ce, Z_OBJ_P(&rowz), "key", sizeof("key") - 1,
			(const char *)key, klen);
		if (doc != NULL) {
			php_corvid_decode_into((corvid_value *)doc, &docz);
		} else {
			ZVAL_NULL(&docz);
		}
		zend_update_property(corvid_row_ce, Z_OBJ_P(&rowz), "doc", sizeof("doc") - 1, &docz);
		zval_ptr_dtor(&docz);
		zend_update_property_double(corvid_row_ce, Z_OBJ_P(&rowz), "score", sizeof("score") - 1,
			(double)score);
		zend_hash_next_index_insert(Z_ARRVAL_P(out), &rowz);
	}
	corvid_rows_free(rows);
}

static void php_corvid_strs_to_array(corvid_strs *s, zval *out) /* consumes s */
{
	const char *str;
	size_t      len;

	array_init(out);
	if (s == NULL) { return; }
	while (corvid_strs_next(s, &str, &len) == 1) {
		zval zv;
		ZVAL_STR(&zv, zend_string_init(str, len, 0));
		zend_hash_next_index_insert(Z_ARRVAL_P(out), &zv);
	}
	corvid_strs_free(s);
}

static void php_corvid_groups_to_array(corvid_groupiter *it, zval *out) /* consumes it */
{
	const char *k;
	size_t      klen;
	double      value;
	zval        gz;

	array_init(out);
	if (it == NULL) { return; }
	while (corvid_groupiter_next(it, &k, &klen, &value) == 1) {
		object_init_ex(&gz, corvid_group_ce);
		zend_update_property_stringl(corvid_group_ce, Z_OBJ_P(&gz), "key", sizeof("key") - 1, k, klen);
		zend_update_property_double(corvid_group_ce, Z_OBJ_P(&gz), "value", sizeof("value") - 1, value);
		zend_hash_next_index_insert(Z_ARRVAL_P(out), &gz);
	}
	corvid_groupiter_free(it);
}

/* ------------------------------------------------------------------ */
/* §1.6 callbacks: C trampolines around PHP callables                   */
/* ------------------------------------------------------------------ */

typedef struct _php_corvid_cb {
	zend_fcall_info       fci;
	zend_fcall_info_cache fcc;
	zval                  exception;   /* stashed user exception (IS_UNDEF when none) */
} php_corvid_cb;

static zend_result php_corvid_cb_init(php_corvid_cb *cb, zval *callable)
{
	memset(cb, 0, sizeof(*cb));
	ZVAL_UNDEF(&cb->exception);
	if (zend_fcall_info_init(callable, 0, &cb->fci, &cb->fcc, NULL, NULL) != SUCCESS) {
		php_corvid_throw_arg("invalid callable");
		return FAILURE;
	}
	return SUCCESS;
}

static void php_corvid_cb_stash_exception(php_corvid_cb *cb)
{
	if (EG(exception)) {
		ZVAL_OBJ_COPY(&cb->exception, EG(exception));
		zend_clear_exception(); /* never unwind through C frames */
	}
}

static void php_corvid_cb_free(php_corvid_cb *cb)
{
	if (!Z_ISUNDEF(cb->exception)) {
		zval_ptr_dtor(&cb->exception);
		ZVAL_UNDEF(&cb->exception);
	}
}

/* Re-throw the stashed exception as-is (the update/scan contract). */
static void php_corvid_cb_rethrow(php_corvid_cb *cb)
{
	zval ex;
	ZVAL_COPY_VALUE(&ex, &cb->exception);
	ZVAL_UNDEF(&cb->exception);
	zend_throw_exception_object(&ex);
}

static corvid_status php_corvid_update_cb(void *ctx, const corvid_value *current, corvid_value **out)
{
	php_corvid_cb *cb  = (php_corvid_cb *)ctx;
	zval           args[1], retval;
	corvid_status  st  = CORVID_OK;

	if (current != NULL) {
		php_corvid_decode_into((corvid_value *)current, &args[0]);
	} else {
		ZVAL_NULL(&args[0]);
	}
	ZVAL_UNDEF(&retval);

	cb->fci.param_count  = 1;
	cb->fci.params       = args;
	cb->fci.named_params = NULL;
	cb->fci.retval       = &retval;

	if (zend_call_function(&cb->fci, &cb->fcc) != SUCCESS || EG(exception)) {
		php_corvid_cb_stash_exception(cb);
		st = CORVID_ERR; /* the §1.6 abort contract */
	} else if (Z_TYPE(retval) == IS_NULL) {
		*out = NULL;    /* null replacement = delete the key */
	} else {
		corvid_value *v = php_corvid_encode(&retval, 0);
		if (v == NULL) {
			php_corvid_cb_stash_exception(cb); /* encode threw */
			st = CORVID_ERR;
		} else {
			*out = v;  /* owned; consumed by the call */
		}
	}
	if (!Z_ISUNDEF(retval)) { zval_ptr_dtor(&retval); }
	zval_ptr_dtor(&args[0]);
	return st;
}

static int php_corvid_scan_cb(void *ctx, const uint8_t *key, size_t key_len, const corvid_value *doc)
{
	php_corvid_cb *cb  = (php_corvid_cb *)ctx;
	zval           args[2], retval;
	int            go   = 1;

	ZVAL_STRINGL(&args[0], (const char *)key, key_len);
	if (doc != NULL) {
		php_corvid_decode_into((corvid_value *)doc, &args[1]);
	} else {
		ZVAL_NULL(&args[1]);
	}
	ZVAL_UNDEF(&retval);

	cb->fci.param_count  = 2;
	cb->fci.params       = args;
	cb->fci.named_params = NULL;
	cb->fci.retval       = &retval;

	if (zend_call_function(&cb->fci, &cb->fcc) != SUCCESS || EG(exception)) {
		php_corvid_cb_stash_exception(cb);
		go = 0; /* stop the scan; re-throw below the ABI */
	} else {
		go = (Z_TYPE(retval) == IS_FALSE) ? 0 : 1;
	}
	if (!Z_ISUNDEF(retval)) { zval_ptr_dtor(&retval); }
	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);
	return go;
}

/* ------------------------------------------------------------------ */
/* object handlers                                                      */
/* ------------------------------------------------------------------ */

static void php_corvid_db_free(zend_object *obj)
{
	php_corvid_db *o = php_corvid_db_from(obj);
	if (o->handle != NULL) {
		corvid_close(o->handle);
		o->handle = NULL;
	}
	zend_object_std_dtor(obj);
}

static zend_object *php_corvid_db_create(zend_class_entry *ce)
{
	php_corvid_db *o = zend_object_alloc(sizeof(php_corvid_db), ce);

	zend_object_std_init(&o->std, ce);
	object_properties_init(&o->std, ce);
	o->std.handlers = &php_corvid_db_handlers;
	o->handle = NULL;
	return &o->std;
}

static void php_corvid_coll_free(zend_object *obj)
{
	php_corvid_coll *o = php_corvid_coll_from(obj);
	if (o->handle != NULL) {
		corvid_collection_free(o->handle);
		o->handle = NULL;
	}
	zend_object_std_dtor(obj);
}

static zend_object *php_corvid_coll_create(zend_class_entry *ce)
{
	php_corvid_coll *o = zend_object_alloc(sizeof(php_corvid_coll), ce);

	zend_object_std_init(&o->std, ce);
	object_properties_init(&o->std, ce);
	o->std.handlers = &php_corvid_coll_handlers;
	o->handle = NULL;
	return &o->std;
}

static void php_corvid_query_free(zend_object *obj)
{
	php_corvid_query *o = php_corvid_query_from(obj);
	if (o->handle != NULL) {
		corvid_query_free(o->handle); /* the abandoned-builder free path */
		o->handle = NULL;
	}
	zend_object_std_dtor(obj);
}

static zend_object *php_corvid_query_create(zend_class_entry *ce)
{
	php_corvid_query *o = zend_object_alloc(sizeof(php_corvid_query), ce);

	zend_object_std_init(&o->std, ce);
	object_properties_init(&o->std, ce);
	o->std.handlers = &php_corvid_query_handlers;
	o->handle = NULL;
	return &o->std;
}

static void php_corvid_pred_free(zend_object *obj)
{
	php_corvid_pred *o = php_corvid_pred_from(obj);
	if (o->handle != NULL) {
		corvid_pred_free(o->handle); /* the never-consumed-root free path */
		o->handle = NULL;
	}
	zend_object_std_dtor(obj);
}

static zend_object *php_corvid_pred_create(zend_class_entry *ce)
{
	php_corvid_pred *o = zend_object_alloc(sizeof(php_corvid_pred), ce);

	zend_object_std_init(&o->std, ce);
	object_properties_init(&o->std, ce);
	o->std.handlers = &php_corvid_pred_handlers;
	o->handle = NULL;
	return &o->std;
}

static void php_corvid_check_db(php_corvid_db *o)
{
	if (o->handle == NULL) {
		php_corvid_throw_arg("this Corvid\\Db is closed");
	}
}

static void php_corvid_check_coll(php_corvid_coll *o)
{
	if (o->handle == NULL) {
		php_corvid_throw_arg("this Corvid\\Collection is not usable (closed?)");
	}
}

static void php_corvid_check_query(php_corvid_query *o)
{
	if (o->handle == NULL) {
		php_corvid_throw_arg("this Corvid\\Query was already consumed by run()/an aggregate");
	}
}

static void php_corvid_check_pred(php_corvid_pred *o)
{
	if (o->handle == NULL) {
		php_corvid_throw_arg("this Corvid\\Predicate was already consumed (and()/or()/not()/filter()/deleteWhere())");
	}
}

/* ------------------------------------------------------------------ */
/* arginfo                                                              */
/* ------------------------------------------------------------------ */

ZEND_BEGIN_ARG_INFO_EX(arginfo_corvid_ffi_version, 0, 0, 0)
ZEND_END_ARG_INFO()

#define A0(name) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 0) ZEND_END_ARG_INFO()
#define A1(name, p1) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 1) ZEND_ARG_INFO(0, p1) ZEND_END_ARG_INFO()
#define A2(name, p1, p2) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 2) ZEND_ARG_INFO(0, p1) ZEND_ARG_INFO(0, p2) ZEND_END_ARG_INFO()
#define A3(name, p1, p2, p3) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 3) ZEND_ARG_INFO(0, p1) ZEND_ARG_INFO(0, p2) ZEND_ARG_INFO(0, p3) ZEND_END_ARG_INFO()
#define A4(name, p1, p2, p3, p4) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 4) ZEND_ARG_INFO(0, p1) ZEND_ARG_INFO(0, p2) ZEND_ARG_INFO(0, p3) ZEND_ARG_INFO(0, p4) ZEND_END_ARG_INFO()
#define A5(name, p1, p2, p3, p4, p5) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 5) ZEND_ARG_INFO(0, p1) ZEND_ARG_INFO(0, p2) ZEND_ARG_INFO(0, p3) ZEND_ARG_INFO(0, p4) ZEND_ARG_INFO(0, p5) ZEND_END_ARG_INFO()

#define AV1(name, p1) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 1) ZEND_ARG_INFO(0, p1) ZEND_ARG_VARIADIC_INFO(0, rest) ZEND_END_ARG_INFO()
#define AV2(name, p1, p2) ZEND_BEGIN_ARG_INFO_EX(arginfo_##name, 0, 0, 2) ZEND_ARG_INFO(0, p1) ZEND_ARG_INFO(0, p2) ZEND_ARG_VARIADIC_INFO(0, rest) ZEND_END_ARG_INFO()

A0(db_nop)
A1(db_open, path)
A0(db_open_memory)
A1(db_collection, name)
A0(db_collections)
A1(db_dump, path)
A1(db_load, path)
A2(db_load_with_renames, path, renames)
A1(db_backup, path)
A0(db_compact)
A0(db_close)

A0(coll_name)
A2(coll_insert, key, doc)
A1(coll_put_many, documents)
A1(coll_insert_auto, doc)
A2(coll_update, key, callback)
A2(coll_patch, key, patch)
A3(coll_compare_and_set, key, expected, replacement)
A1(coll_delete, key)
A1(coll_delete_where, predicate)
AV1(coll_delete_batch, keys)
A3(coll_insert_with_ttl, key, doc, expiresAt)
A2(coll_set_ttl, key, expiresAt)
A1(coll_get_ttl, key)
A1(coll_purge_expired, now)
A1(coll_get, key)
AV2(coll_get_fields, key, fields)
A1(coll_scan, callback)
A2(coll_page, after, limit)
A3(coll_phrase_search, field, phrase, k)
A3(coll_link, from, relation, to)
A4(coll_link_weighted, from, relation, to, weight)
A3(coll_unlink, from, relation, to)
A2(coll_neighbors, from, relation)
A2(coll_in_neighbors, to, relation)
A2(coll_neighbors_weighted, from, relation)
A3(coll_traverse, start, relation, hops)
A4(coll_geo_within_radius, field, lat, lon, radiusKm)
A5(coll_geo_within_bbox, field, minLat, minLon, maxLat, maxLon)
A4(coll_geo_nearest, field, lat, lon, k)
A1(coll_create_scalar_index, field)
AV1(coll_create_compound_index, fields)
A1(coll_create_text_index, field)
A1(coll_create_text_index_ondisk, field)
A1(coll_create_geo_index, field)
A2(coll_create_vector_index, field, metric)
A3(coll_create_vector_index_quantized, field, metric, quant)
A2(coll_create_vector_index_ondisk, field, metric)
A3(coll_create_vector_index_ondisk_quantized, field, metric, quant)
A4(coll_create_vector_index_pq, field, metric, m, k)
A4(coll_create_vector_index_ondisk_pq, field, metric, m, k)
AV1(coll_set_schema, definitions)
A0(coll_schema)
A0(coll_len)
A0(coll_query)

A1(query_filter, predicate)
A4(query_vector, field, query, k, metric)
A3(query_text, field, query, k)
A1(query_fuse_rrf, k)
A1(query_rerank_mmr, lambda)
A0(query_approx)
A1(query_limit, n)
A1(query_offset, n)
A2(query_order_by, field, descending)
AV1(query_select, fields)
A0(query_run)
A0(query_count)
A1(query_count_distinct, field)
A1(query_sum, field)
A1(query_avg, field)
A1(query_min, field)
A1(query_max, field)
A1(query_group_count, field)
A2(query_group_sum, groupField, valueField)
A2(query_group_avg, groupField, valueField)

A1(pred_and, other)
A1(pred_or, other)
A0(pred_not)
A1(field_cmp, value)
AV1(field_in, values)
A2(field_between, low, high)
A1(field_starts_with, prefix)
A1(field_contains, substr)
A3(field_geo_within, lat, lon, radiusKm)
A0(field_exists)
A1(field_construct, path)

A1(bytes_construct, bytes)
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bytes_to_string, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
A1(vector_construct, values)
A0(vector_values)
A4(fielddef_construct, name, type, required, unique)

A1(values_type, value)
A1(values_len, value)
A1(values_as_int, value)
A1(values_as_float, value)
A1(values_as_bool, value)
A1(values_as_text, value)
A1(values_as_bytes, value)
A1(values_as_vector, value)
A1(values_map_keys, value)
A1(values_clone, value)
A2(values_push, container, item)
A3(values_put, map, key, value)
A0(values_self_check)

/* ------------------------------------------------------------------ */
/* Corvid\Db                                                            */
/* ------------------------------------------------------------------ */

PHP_METHOD(Corvid_Db, __construct)
{
	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_throw_arg("use Corvid\\Db::open() / Corvid\\Db::openMemory()");
}

PHP_METHOD(Corvid_Db, open)
{
	zend_string   *path;
	corvid_db     *h;
	zval           out;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(path)
	ZEND_PARSE_PARAMETERS_END();

	h = corvid_open(ZSTR_VAL(path), ZSTR_LEN(path));
	if (h == NULL) {
		php_corvid_throw_last("corvid_open failed");
		RETURN_THROWS();
	}
	object_init_ex(&out, corvid_db_ce);
	Z_CORVID_DB_P(&out)->handle = h;
	RETURN_ZVAL(&out, 0, 0); /* transfer */
}

PHP_METHOD(Corvid_Db, openMemory)
{
	corvid_db *h;
	zval       out;

	ZEND_PARSE_PARAMETERS_NONE();
	h = corvid_open_memory();
	if (h == NULL) {
		php_corvid_throw_last("corvid_open_memory failed");
		RETURN_THROWS();
	}
	object_init_ex(&out, corvid_db_ce);
	Z_CORVID_DB_P(&out)->handle = h;
	RETURN_ZVAL(&out, 0, 0);
}

PHP_METHOD(Corvid_Db, collection)
{
	php_corvid_db   *dbo = Z_CORVID_DB_P(ZEND_THIS);
	zend_string     *name;
	corvid_coll     *h;
	zval             out;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(name)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_db(dbo);
	if (EG(exception)) { RETURN_THROWS(); }

	h = corvid_collection(dbo->handle, ZSTR_VAL(name), ZSTR_LEN(name));
	if (h == NULL) {
		php_corvid_throw_last("corvid_collection failed");
		RETURN_THROWS();
	}
	object_init_ex(&out, corvid_coll_ce);
	Z_CORVID_COLL_P(&out)->handle = h;
	RETURN_ZVAL(&out, 0, 0);
}

PHP_METHOD(Corvid_Db, collections)
{
	php_corvid_db *dbo = Z_CORVID_DB_P(ZEND_THIS);
	corvid_strs   *s;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_db(dbo);
	if (EG(exception)) { RETURN_THROWS(); }

	s = corvid_collections(dbo->handle);
	if (s == NULL) {
		php_corvid_throw_last("corvid_collections failed");
		RETURN_THROWS();
	}
	php_corvid_strs_to_array(s, return_value);
}

PHP_METHOD(Corvid_Db, dump)
{
	php_corvid_db *dbo = Z_CORVID_DB_P(ZEND_THIS);
	zend_string   *path;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(path)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_db(dbo);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_dump_to_path(dbo->handle, ZSTR_VAL(path), ZSTR_LEN(path)) != CORVID_OK) {
		php_corvid_throw_last("corvid_dump_to_path failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Db, load)
{
	php_corvid_db *dbo = Z_CORVID_DB_P(ZEND_THIS);
	zend_string   *path;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(path)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_db(dbo);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_load_from_path(dbo->handle, ZSTR_VAL(path), ZSTR_LEN(path)) != CORVID_OK) {
		php_corvid_throw_last("corvid_load_from_path failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Db, loadWithRenames)
{
	php_corvid_db *dbo = Z_CORVID_DB_P(ZEND_THIS);
	zend_string   *path;
	HashTable     *map;
	size_t         n, i = 0;
	const char   **olds, **news;
	size_t        *old_lens, *new_lens;
	zend_string  **news_z; /* keeps the coerced targets alive across the ABI call */
	zend_string   *k;
	zval          *v;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(path)
		Z_PARAM_ARRAY_HT(map)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_db(dbo);
	if (EG(exception)) { RETURN_THROWS(); }

	n       = zend_hash_num_elements(map);
	olds     = safe_emalloc(sizeof(char *), (n ? n : 1), 0);
	news     = safe_emalloc(sizeof(char *), (n ? n : 1), 0);
	old_lens = safe_emalloc(sizeof(size_t), (n ? n : 1), 0);
	new_lens = safe_emalloc(sizeof(size_t), (n ? n : 1), 0);
	news_z   = safe_emalloc(sizeof(zend_string *), (n ? n : 1), 0);

	ZEND_HASH_FOREACH_STR_KEY_VAL(map, k, v) {
		/* zval_get_string allocates a FRESH string for non-string
		 * values (int/float/...); its bytes must outlive this loop —
		 * release only after the engine call has returned. */
		zend_string *nv = zval_get_string(v);
		if (k == NULL) {
			php_corvid_throw_arg("renames map keys must be strings");
			zend_string_release(nv);
			goto cleanup;
		}
		olds[i]   = ZSTR_VAL(k);   old_lens[i] = ZSTR_LEN(k);
		news_z[i] = nv;
		news[i]   = ZSTR_VAL(nv);  new_lens[i] = ZSTR_LEN(nv);
		i++;
	} ZEND_HASH_FOREACH_END();

	if (corvid_load_from_path_with_renames(dbo->handle,
	        ZSTR_VAL(path), ZSTR_LEN(path),
	        (const char *const *)olds, (const char *const *)news,
	        old_lens, new_lens, n) != CORVID_OK) {
		php_corvid_throw_last("corvid_load_from_path_with_renames failed");
	}

cleanup:
	for (size_t j = 0; j < i; j++) { zend_string_release(news_z[j]); }
	efree(olds); efree(news); efree(old_lens); efree(new_lens); efree(news_z);
}

PHP_METHOD(Corvid_Db, backup)
{
	php_corvid_db *dbo = Z_CORVID_DB_P(ZEND_THIS);
	zend_string   *path;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(path)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_db(dbo);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_backup(dbo->handle, ZSTR_VAL(path), ZSTR_LEN(path)) != CORVID_OK) {
		php_corvid_throw_last("corvid_backup failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Db, compact)
{
	php_corvid_db *dbo = Z_CORVID_DB_P(ZEND_THIS);
	int            moved = 0;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_db(dbo);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_compact(dbo->handle, &moved) != CORVID_OK) {
		php_corvid_throw_last("corvid_compact failed (derived handles still open? CORVID_E_BUSY)");
		RETURN_THROWS();
	}
	RETURN_BOOL(moved != 0);
}

PHP_METHOD(Corvid_Db, close)
{
	php_corvid_db *dbo = Z_CORVID_DB_P(ZEND_THIS);

	ZEND_PARSE_PARAMETERS_NONE();
	if (dbo->handle != NULL) {
		corvid_close(dbo->handle);
		dbo->handle = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Corvid\Collection — writes                                           */
/* ------------------------------------------------------------------ */

PHP_METHOD(Corvid_Collection, name)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	const char      *n;
	size_t           len = 0;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	n = corvid_collection_name(co->handle, &len);
	if (n == NULL) {
		php_corvid_throw_last("corvid_collection_name failed");
		RETURN_THROWS();
	}
	RETURN_STRINGL(n, len);
}

PHP_METHOD(Corvid_Collection, insert)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	zval            *doc;
	corvid_value    *v;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(doc)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	v = php_corvid_encode(doc, 0);
	if (v == NULL) { RETURN_THROWS(); }
	if (corvid_insert(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), v) != CORVID_OK) {
		php_corvid_throw_last("corvid_insert failed");
	}
	corvid_value_free(v); /* the engine cloned it; ours is still ours */
	if (EG(exception)) { RETURN_THROWS(); }
}

PHP_METHOD(Corvid_Collection, putMany)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	HashTable       *docs;
	size_t           n, i = 0;
	corvid_kv       *items;
	zend_string     *k;
	zval            *v;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY_HT(docs)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }

	n     = zend_hash_num_elements(docs);
	items = safe_emalloc(sizeof(corvid_kv), (n ? n : 1), 0);

	ZEND_HASH_FOREACH_STR_KEY_VAL(docs, k, v) {
		corvid_value *cv = php_corvid_encode(v, 0);
		if (cv == NULL) { goto cleanup; }
		if (k == NULL) {
			corvid_value_free(cv); /* ours until consumed — not the leak's */
			php_corvid_throw_arg("putMany wants a key => document map (string keys)");
			goto cleanup;
		}
		items[i].key     = (const uint8_t *)ZSTR_VAL(k);
		items[i].key_len = ZSTR_LEN(k);
		items[i].val     = cv;
		i++;
	} ZEND_HASH_FOREACH_END();

	if (corvid_put_many(co->handle, items, n) != CORVID_OK) {
		php_corvid_throw_last("corvid_put_many failed");
	}

cleanup:
	while (i > 0) { corvid_value_free((corvid_value *)items[--i].val); }
	efree(items);
	if (EG(exception)) { RETURN_THROWS(); }
}

PHP_METHOD(Corvid_Collection, insertAuto)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zval            *doc;
	corvid_value    *v;
	size_t           klen = 0;
	uint8_t         *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(doc)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	v = php_corvid_encode(doc, 0);
	if (v == NULL) { RETURN_THROWS(); }
	key = corvid_insert_auto(co->handle, v, &klen);
	corvid_value_free(v);
	if (key == NULL) {
		php_corvid_throw_last("corvid_insert_auto failed");
		RETURN_THROWS();
	}
	RETVAL_STRINGL((const char *)key, klen);
	corvid_free(key); /* the ABI's buffer deallocator */
}

PHP_METHOD(Corvid_Collection, update)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	zval            *callback;
	php_corvid_cb    cb;
	corvid_status    st;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(callback)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (php_corvid_cb_init(&cb, callback) != SUCCESS) { RETURN_THROWS(); }

	st = corvid_update(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key),
		php_corvid_update_cb, &cb);

	if (!Z_ISUNDEF(cb.exception)) {
		/* The aborting-callback contract (§1.6), ruled symmetric with
		 * scan: the engine recorded CORVID_E_ARGUMENT (its abort
		 * status) and wrote nothing — what surfaces at the call site
		 * is the callback's OWN exception, verbatim. */
		php_corvid_cb_rethrow(&cb);
		php_corvid_cb_free(&cb);
		RETURN_THROWS();
	}
	php_corvid_cb_free(&cb);
	if (st != CORVID_OK) {
		php_corvid_throw_last("corvid_update failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Collection, patch)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	zval            *patch;
	corvid_value    *v;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(patch)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	v = php_corvid_encode(patch, 0);
	if (v == NULL) { RETURN_THROWS(); }
	if (corvid_patch(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), v) != CORVID_OK) {
		php_corvid_throw_last("corvid_patch failed");
	}
	corvid_value_free(v);
	if (EG(exception)) { RETURN_THROWS(); }
}

PHP_METHOD(Corvid_Collection, compareAndSet)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	zval            *expected, *replacement;
	corvid_value    *ex = NULL, *re = NULL;
	int32_t          applied = 0;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(expected)    /* null = "must be absent" */
		Z_PARAM_ZVAL(replacement) /* null = "delete on match" */
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (Z_TYPE_P(expected) != IS_NULL) {
		ex = php_corvid_encode(expected, 0);
		if (ex == NULL) { RETURN_THROWS(); }
	}
	if (Z_TYPE_P(replacement) != IS_NULL) {
		re = php_corvid_encode(replacement, 0);
		if (re == NULL) { if (ex) corvid_value_free(ex); RETURN_THROWS(); }
	}
	if (corvid_compare_and_set(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key),
	        ex, re, &applied) != CORVID_OK) {
		php_corvid_throw_last("corvid_compare_and_set failed");
		if (ex) corvid_value_free(ex);
		if (re) corvid_value_free(re);
		RETURN_THROWS();
	}
	if (ex) corvid_value_free(ex); /* cloned by the engine */
	if (re) corvid_value_free(re);
	RETURN_BOOL(applied != 0);
}

PHP_METHOD(Corvid_Collection, delete)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	int32_t          existed = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_delete(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), &existed) != CORVID_OK) {
		php_corvid_throw_last("corvid_delete failed");
		RETURN_THROWS();
	}
	RETURN_BOOL(existed != 0);
}

PHP_METHOD(Corvid_Collection, deleteWhere)
{
	php_corvid_coll  *co = Z_CORVID_COLL_P(ZEND_THIS);
	zval             *zp;
	php_corvid_pred  *p;
	size_t            removed = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(zp, corvid_pred_ce)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	p = Z_CORVID_PRED_P(zp);
	php_corvid_check_pred(p);
	if (EG(exception)) { RETURN_THROWS(); }

	/* §8: consumed unconditionally, whatever the status. */
	corvid_pred *h = p->handle;
	p->handle = NULL;
	if (corvid_delete_where(co->handle, h, &removed) != CORVID_OK) {
		php_corvid_throw_last("corvid_delete_where failed");
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)removed);
}

PHP_METHOD(Corvid_Collection, deleteBatch)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zval            *args;
	uint32_t         argc, i;
	size_t           removed = 0;
	const uint8_t  **keys;
	size_t          *key_lens;
	zend_string    **keys_z; /* keeps the coerced keys alive across the ABI call */

	ZEND_PARSE_PARAMETERS_START(1, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }

	keys     = safe_emalloc(sizeof(uint8_t *), (argc ? argc : 1), 0);
	key_lens = safe_emalloc(sizeof(size_t), (argc ? argc : 1), 0);
	keys_z   = safe_emalloc(sizeof(zend_string *), (argc ? argc : 1), 0);

	for (i = 0; i < argc; i++) {
		/* zval_get_string allocates a FRESH string for non-string args
		 * (int/float keys above 9 especially); release only after the
		 * engine call has returned — not inside this loop. */
		keys_z[i]   = zval_get_string(&args[i]);
		keys[i]     = (const uint8_t *)ZSTR_VAL(keys_z[i]);
		key_lens[i] = ZSTR_LEN(keys_z[i]);
	}
	if (corvid_delete_batch(co->handle, (const uint8_t *const *)keys, key_lens, argc, &removed) != CORVID_OK) {
		php_corvid_throw_last("corvid_delete_batch failed");
	}
	for (i = 0; i < argc; i++) { zend_string_release(keys_z[i]); }
	efree(keys); efree(key_lens); efree(keys_z);
	if (EG(exception)) { RETURN_THROWS(); }
	RETURN_LONG((zend_long)removed);
}

PHP_METHOD(Corvid_Collection, insertWithTtl)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	zval            *doc;
	zend_long        expiresAt;
	corvid_value    *v;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(doc)
		Z_PARAM_LONG(expiresAt)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	v = php_corvid_encode(doc, 0);
	if (v == NULL) { RETURN_THROWS(); }
	if (corvid_insert_with_ttl(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), v,
	        (int64_t)expiresAt) != CORVID_OK) {
		php_corvid_throw_last("corvid_insert_with_ttl failed");
	}
	corvid_value_free(v);
	if (EG(exception)) { RETURN_THROWS(); }
}

PHP_METHOD(Corvid_Collection, setTtl)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	zend_long        expiresAt;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(key)
		Z_PARAM_LONG(expiresAt)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_set_ttl(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), (int64_t)expiresAt) != CORVID_OK) {
		php_corvid_throw_last("corvid_set_ttl failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Collection, getTtl)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	int64_t          at = 0;
	int32_t          has = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_get_ttl(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), &at, &has) != CORVID_OK) {
		php_corvid_throw_last("corvid_get_ttl failed");
		RETURN_THROWS();
	}
	if (has) {
		RETURN_LONG((zend_long)at);
	}
	RETURN_NULL();
}

PHP_METHOD(Corvid_Collection, purgeExpired)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_long        now;
	size_t           purged = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(now)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_purge_expired(co->handle, (int64_t)now, &purged) != CORVID_OK) {
		php_corvid_throw_last("corvid_purge_expired failed");
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)purged);
}

/* ------------------------------------------------------------------ */
/* Corvid\Collection — reads                                            */
/* ------------------------------------------------------------------ */

PHP_METHOD(Corvid_Collection, get)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	corvid_value    *v = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_get(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), &v) != CORVID_OK) {
		php_corvid_throw_last("corvid_get failed");
		RETURN_THROWS();
	}
	if (v == NULL) { RETURN_NULL(); } /* absence is a success */
	php_corvid_decode_into(v, return_value);
	corvid_value_free(v);
}

PHP_METHOD(Corvid_Collection, getFields)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *key;
	zval            *args;
	uint32_t         argc, i;
	corvid_value    *v = NULL;

	ZEND_PARSE_PARAMETERS_START(2, -1)
		Z_PARAM_STR(key)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_get(co->handle, (const uint8_t *)ZSTR_VAL(key), ZSTR_LEN(key), &v) != CORVID_OK) {
		php_corvid_throw_last("corvid_get failed");
		RETURN_THROWS();
	}
	array_init(return_value);
	if (v == NULL) { return; } /* absent: no fields present */

	for (i = 0; i < argc; i++) {
		zend_string         *fname = zval_get_string(&args[i]);
		const corvid_value  *child = corvid_value_map_get(v, ZSTR_VAL(fname), ZSTR_LEN(fname));
		if (child != NULL) {
			zval iz;
			php_corvid_decode_into((corvid_value *)child, &iz);
			zend_symtable_update(Z_ARRVAL_P(return_value), fname, &iz);
		}
		zend_string_release(fname);
	}
	corvid_value_free(v);
}

PHP_METHOD(Corvid_Collection, scan)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zval            *callback;
	php_corvid_cb    cb;
	corvid_status    st;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(callback)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (php_corvid_cb_init(&cb, callback) != SUCCESS) { RETURN_THROWS(); }

	st = corvid_scan(co->handle, php_corvid_scan_cb, &cb);

	if (!Z_ISUNDEF(cb.exception)) {
		php_corvid_cb_rethrow(&cb); /* the original exception, as-is */
		php_corvid_cb_free(&cb);
		RETURN_THROWS();
	}
	php_corvid_cb_free(&cb);
	if (st != CORVID_OK) {
		php_corvid_throw_last("corvid_scan failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Collection, page)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *after = NULL;
	zend_long        limit;
	corvid_rows     *rows = NULL;
	uint8_t         *next = NULL;
	size_t           next_len = 0;
	zval             pagez, rowsz, nextz;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR_OR_NULL(after)
		Z_PARAM_LONG(limit)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }

	if (corvid_page(co->handle,
	        (const uint8_t *)(after ? ZSTR_VAL(after) : NULL),
	        (after ? ZSTR_LEN(after) : 0),
	        (size_t)limit, &rows, &next, &next_len) != CORVID_OK) {
		php_corvid_throw_last("corvid_page failed");
		RETURN_THROWS();
	}

	php_corvid_rows_to_array(rows, &rowsz);
	if (next != NULL) {
		ZVAL_STRINGL(&nextz, (const char *)next, next_len);
		corvid_free(next); /* the ABI's buffer deallocator */
	} else {
		ZVAL_NULL(&nextz);
	}

	object_init_ex(&pagez, corvid_page_ce);
	zend_update_property(corvid_page_ce, Z_OBJ_P(&pagez), "rows", sizeof("rows") - 1, &rowsz);
	zend_update_property(corvid_page_ce, Z_OBJ_P(&pagez), "next", sizeof("next") - 1, &nextz);
	zval_ptr_dtor(&rowsz);
	zval_ptr_dtor(&nextz);
	RETURN_ZVAL(&pagez, 0, 0);
}

PHP_METHOD(Corvid_Collection, len)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	size_t           n = 0;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_len(co->handle, &n) != CORVID_OK) {
		php_corvid_throw_last("corvid_len failed");
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)n);
}

PHP_METHOD(Corvid_Collection, phraseSearch)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *field, *phrase;
	zend_long        k;
	corvid_rows     *rows;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(field)
		Z_PARAM_STR(phrase)
		Z_PARAM_LONG(k)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	rows = corvid_phrase_search(co->handle, ZSTR_VAL(field), ZSTR_LEN(field),
		ZSTR_VAL(phrase), ZSTR_LEN(phrase), (size_t)k);
	if (rows == NULL) {
		php_corvid_throw_last("corvid_phrase_search failed");
		RETURN_THROWS();
	}
	php_corvid_rows_to_array(rows, return_value);
}

/* ------------------------------------------------------------------ */
/* Corvid\Collection — graph + geo                                      */
/* ------------------------------------------------------------------ */

PHP_METHOD(Corvid_Collection, link)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *from, *relation, *to;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(from)
		Z_PARAM_STR(relation)
		Z_PARAM_STR(to)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_link(co->handle, (const uint8_t *)ZSTR_VAL(from), ZSTR_LEN(from),
	        ZSTR_VAL(relation), ZSTR_LEN(relation),
	        (const uint8_t *)ZSTR_VAL(to), ZSTR_LEN(to)) != CORVID_OK) {
		php_corvid_throw_last("corvid_link failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Collection, linkWeighted)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *from, *relation, *to;
	double           weight;

	ZEND_PARSE_PARAMETERS_START(4, 4)
		Z_PARAM_STR(from)
		Z_PARAM_STR(relation)
		Z_PARAM_STR(to)
		Z_PARAM_DOUBLE(weight)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_link_weighted(co->handle, (const uint8_t *)ZSTR_VAL(from), ZSTR_LEN(from),
	        ZSTR_VAL(relation), ZSTR_LEN(relation),
	        (const uint8_t *)ZSTR_VAL(to), ZSTR_LEN(to), weight) != CORVID_OK) {
		php_corvid_throw_last("corvid_link_weighted failed");
		RETURN_THROWS();
	}
}

PHP_METHOD(Corvid_Collection, unlink)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *from, *relation, *to;
	int              removed = 0;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(from)
		Z_PARAM_STR(relation)
		Z_PARAM_STR(to)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_unlink(co->handle, (const uint8_t *)ZSTR_VAL(from), ZSTR_LEN(from),
	        ZSTR_VAL(relation), ZSTR_LEN(relation),
	        (const uint8_t *)ZSTR_VAL(to), ZSTR_LEN(to), &removed) != CORVID_OK) {
		php_corvid_throw_last("corvid_unlink failed");
		RETURN_THROWS();
	}
	RETURN_BOOL(removed != 0);
}

static void php_corvid_strs_call_2(corvid_strs *(*fn)(corvid_coll *, const uint8_t *, size_t, const char *, size_t),
	zval *return_value, php_corvid_coll *co, zend_string *a, zend_string *b)
{
	corvid_strs *s = fn(co->handle, (const uint8_t *)ZSTR_VAL(a), ZSTR_LEN(a), ZSTR_VAL(b), ZSTR_LEN(b));
	if (s == NULL) {
		php_corvid_throw_last("neighbors call failed");
		return;
	}
	php_corvid_strs_to_array(s, return_value);
}

PHP_METHOD(Corvid_Collection, neighbors)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *from, *relation;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(from)
		Z_PARAM_STR(relation)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	php_corvid_strs_call_2(corvid_neighbors, return_value, co, from, relation);
}

PHP_METHOD(Corvid_Collection, inNeighbors)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *to, *relation;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(to)
		Z_PARAM_STR(relation)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	php_corvid_strs_call_2(corvid_in_neighbors, return_value, co, to, relation);
}

PHP_METHOD(Corvid_Collection, neighborsWeighted)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *from, *relation;
	corvid_geohits  *h;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(from)
		Z_PARAM_STR(relation)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	h = corvid_neighbors_weighted(co->handle, (const uint8_t *)ZSTR_VAL(from), ZSTR_LEN(from),
		ZSTR_VAL(relation), ZSTR_LEN(relation));
	if (h == NULL) {
		php_corvid_throw_last("corvid_neighbors_weighted failed");
		RETURN_THROWS();
	}

	{
		struct corvid_geohit hit;
		const corvid_value  *doc;
		zval                 wz;

		array_init(return_value);
		while (corvid_geohits_next(h, &hit, &doc) == 1) {
			object_init_ex(&wz, corvid_whit_ce);
			zend_update_property_stringl(corvid_whit_ce, Z_OBJ_P(&wz), "key", sizeof("key") - 1,
				(const char *)hit.key, hit.key_len);
			zend_update_property_double(corvid_whit_ce, Z_OBJ_P(&wz), "weight", sizeof("weight") - 1,
				hit.distance_km); /* the geohits cursor reuses distance_km for the edge weight */
			zend_hash_next_index_insert(Z_ARRVAL_P(return_value), &wz);
		}
		corvid_geohits_free(h);
	}
}

PHP_METHOD(Corvid_Collection, traverse)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *start, *relation;
	zend_long        hops;
	corvid_strs     *s;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(start)
		Z_PARAM_STR(relation)
		Z_PARAM_LONG(hops)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	s = corvid_traverse(co->handle, (const uint8_t *)ZSTR_VAL(start), ZSTR_LEN(start),
		ZSTR_VAL(relation), ZSTR_LEN(relation), (size_t)hops);
	if (s == NULL) {
		php_corvid_throw_last("corvid_traverse failed");
		RETURN_THROWS();
	}
	php_corvid_strs_to_array(s, return_value);
}

static void php_corvid_geohits_to_array(corvid_geohits *h, zval *out)
{
	struct corvid_geohit hit;
	const corvid_value  *doc;
	zval                 gz;

	array_init(out);
	if (h == NULL) { return; }
	while (corvid_geohits_next(h, &hit, &doc) == 1) {
		zval docz;

		object_init_ex(&gz, corvid_geohit_ce);
		zend_update_property_stringl(corvid_geohit_ce, Z_OBJ_P(&gz), "key", sizeof("key") - 1,
			(const char *)hit.key, hit.key_len);
		zend_update_property_double(corvid_geohit_ce, Z_OBJ_P(&gz), "distanceKm", sizeof("distanceKm") - 1,
			hit.distance_km);
		if (doc != NULL) {
			php_corvid_decode_into((corvid_value *)doc, &docz);
		} else {
			ZVAL_NULL(&docz);
		}
		zend_update_property(corvid_geohit_ce, Z_OBJ_P(&gz), "doc", sizeof("doc") - 1, &docz);
		zval_ptr_dtor(&docz);
		zend_hash_next_index_insert(Z_ARRVAL_P(out), &gz);
	}
	corvid_geohits_free(h);
}

PHP_METHOD(Corvid_Collection, geoWithinRadius)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *field;
	double           lat, lon, radiusKm;
	corvid_geohits  *h;

	ZEND_PARSE_PARAMETERS_START(4, 4)
		Z_PARAM_STR(field)
		Z_PARAM_DOUBLE(lat)
		Z_PARAM_DOUBLE(lon)
		Z_PARAM_DOUBLE(radiusKm)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	h = corvid_geo_within_radius(co->handle, ZSTR_VAL(field), ZSTR_LEN(field), lat, lon, radiusKm);
	if (h == NULL) {
		php_corvid_throw_last("corvid_geo_within_radius failed");
		RETURN_THROWS();
	}
	php_corvid_geohits_to_array(h, return_value);
}

PHP_METHOD(Corvid_Collection, geoWithinBBox)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *field;
	double           minLat, minLon, maxLat, maxLon;
	corvid_geohits  *h;

	ZEND_PARSE_PARAMETERS_START(5, 5)
		Z_PARAM_STR(field)
		Z_PARAM_DOUBLE(minLat)
		Z_PARAM_DOUBLE(minLon)
		Z_PARAM_DOUBLE(maxLat)
		Z_PARAM_DOUBLE(maxLon)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	h = corvid_geo_within_bbox(co->handle, ZSTR_VAL(field), ZSTR_LEN(field), minLat, minLon, maxLat, maxLon);
	if (h == NULL) {
		php_corvid_throw_last("corvid_geo_within_bbox failed");
		RETURN_THROWS();
	}
	php_corvid_geohits_to_array(h, return_value);
}

PHP_METHOD(Corvid_Collection, geoNearest)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zend_string     *field;
	double           lat, lon;
	zend_long        k;
	corvid_geohits  *h;

	ZEND_PARSE_PARAMETERS_START(4, 4)
		Z_PARAM_STR(field)
		Z_PARAM_DOUBLE(lat)
		Z_PARAM_DOUBLE(lon)
		Z_PARAM_LONG(k)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	h = corvid_geo_nearest(co->handle, ZSTR_VAL(field), ZSTR_LEN(field), lat, lon, (size_t)k);
	if (h == NULL) {
		php_corvid_throw_last("corvid_geo_nearest failed");
		RETURN_THROWS();
	}
	php_corvid_geohits_to_array(h, return_value);
}

/* ------------------------------------------------------------------ */
/* Corvid\Collection — indexes + schema                                 */
/* ------------------------------------------------------------------ */

#define PHP_CORVID_IDX_1(fn) do { \
	zend_string *field; \
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(field) ZEND_PARSE_PARAMETERS_END(); \
	php_corvid_check_coll(co); \
	if (EG(exception)) { RETURN_THROWS(); } \
	if (fn(co->handle, ZSTR_VAL(field), ZSTR_LEN(field)) != CORVID_OK) { \
		php_corvid_throw_last(#fn " failed"); RETURN_THROWS(); \
	} \
} while (0)

#define PHP_CORVID_IDX_2_METRIC(fn) do { \
	zend_string *field; zend_long metric; \
	ZEND_PARSE_PARAMETERS_START(2, 2) Z_PARAM_STR(field) Z_PARAM_LONG(metric) ZEND_PARSE_PARAMETERS_END(); \
	php_corvid_check_coll(co); \
	if (EG(exception)) { RETURN_THROWS(); } \
	if (metric < 0 || metric > 2) { php_corvid_throw_arg("metric must be Corvid\\Metric::COSINE/DOT/L2"); RETURN_THROWS(); } \
	if (fn(co->handle, ZSTR_VAL(field), ZSTR_LEN(field), (corvid_metric)metric) != CORVID_OK) { \
		php_corvid_throw_last(#fn " failed"); RETURN_THROWS(); \
	} \
} while (0)

#define PHP_CORVID_IDX_3_QUANT(fn) do { \
	zend_string *field; zend_long metric, quant; \
	ZEND_PARSE_PARAMETERS_START(3, 3) Z_PARAM_STR(field) Z_PARAM_LONG(metric) Z_PARAM_LONG(quant) ZEND_PARSE_PARAMETERS_END(); \
	php_corvid_check_coll(co); \
	if (EG(exception)) { RETURN_THROWS(); } \
	if (metric < 0 || metric > 2) { php_corvid_throw_arg("metric must be Corvid\\Metric::COSINE/DOT/L2"); RETURN_THROWS(); } \
	if (quant < 0 || quant > 2) { php_corvid_throw_arg("quant must be Corvid\\Quant::NONE/BINARY/SCALAR"); RETURN_THROWS(); } \
	if (fn(co->handle, ZSTR_VAL(field), ZSTR_LEN(field), (corvid_metric)metric, (corvid_quant)quant) != CORVID_OK) { \
		php_corvid_throw_last(#fn " failed"); RETURN_THROWS(); \
	} \
} while (0)

#define PHP_CORVID_IDX_PQ(fn) do { \
	zend_string *field; zend_long metric, m, k; \
	ZEND_PARSE_PARAMETERS_START(4, 4) Z_PARAM_STR(field) Z_PARAM_LONG(metric) Z_PARAM_LONG(m) Z_PARAM_LONG(k) ZEND_PARSE_PARAMETERS_END(); \
	php_corvid_check_coll(co); \
	if (EG(exception)) { RETURN_THROWS(); } \
	if (metric < 0 || metric > 2) { php_corvid_throw_arg("metric must be Corvid\\Metric::COSINE/DOT/L2"); RETURN_THROWS(); } \
	if (fn(co->handle, ZSTR_VAL(field), ZSTR_LEN(field), (corvid_metric)metric, (size_t)m, (size_t)k) != CORVID_OK) { \
		php_corvid_throw_last(#fn " failed"); RETURN_THROWS(); \
	} \
} while (0)

PHP_METHOD(Corvid_Collection, createScalarIndex)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_1(corvid_create_scalar_index);
}

PHP_METHOD(Corvid_Collection, createCompoundIndex)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	zval            *args;
	uint32_t         argc, i;
	const char     **fields;
	size_t          *lens;
	zend_string    **fields_z; /* keeps the coerced names alive across the ABI call */

	ZEND_PARSE_PARAMETERS_START(1, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }

	fields   = safe_emalloc(sizeof(char *), (argc ? argc : 1), 0);
	lens     = safe_emalloc(sizeof(size_t), (argc ? argc : 1), 0);
	fields_z = safe_emalloc(sizeof(zend_string *), (argc ? argc : 1), 0);
	for (i = 0; i < argc; i++) {
		/* fresh zend_strings for non-string args — released after the call */
		fields_z[i] = zval_get_string(&args[i]);
		fields[i]   = ZSTR_VAL(fields_z[i]);
		lens[i]     = ZSTR_LEN(fields_z[i]);
	}
	if (corvid_create_compound_index(co->handle, (const char *const *)fields, lens, argc) != CORVID_OK) {
		php_corvid_throw_last("corvid_create_compound_index failed");
	}
	for (i = 0; i < argc; i++) { zend_string_release(fields_z[i]); }
	efree(fields); efree(lens); efree(fields_z);
	if (EG(exception)) { RETURN_THROWS(); }
}

PHP_METHOD(Corvid_Collection, createTextIndex)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_1(corvid_create_text_index);
}

PHP_METHOD(Corvid_Collection, createTextIndexOnDisk)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_1(corvid_create_text_index_ondisk);
}

PHP_METHOD(Corvid_Collection, createGeoIndex)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_1(corvid_create_geo_index);
}

PHP_METHOD(Corvid_Collection, createVectorIndex)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_2_METRIC(corvid_create_vector_index);
}

PHP_METHOD(Corvid_Collection, createVectorIndexQuantized)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_3_QUANT(corvid_create_vector_index_quantized);
}

PHP_METHOD(Corvid_Collection, createVectorIndexOnDisk)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_2_METRIC(corvid_create_vector_index_ondisk);
}

PHP_METHOD(Corvid_Collection, createVectorIndexOnDiskQuantized)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_3_QUANT(corvid_create_vector_index_ondisk_quantized);
}

PHP_METHOD(Corvid_Collection, createVectorIndexPQ)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_PQ(corvid_create_vector_index_pq);
}

PHP_METHOD(Corvid_Collection, createVectorIndexOnDiskPQ)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	PHP_CORVID_IDX_PQ(corvid_create_vector_index_ondisk_pq);
}

PHP_METHOD(Corvid_Collection, setSchema)
{
	php_corvid_coll  *co = Z_CORVID_COLL_P(ZEND_THIS);
	zval             *args, tmp;
	uint32_t          argc, i;
	corvid_field_def *defs;
	zend_string     **names; /* keeps the field names alive across the call */

	ZEND_PARSE_PARAMETERS_START(1, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }

	defs  = safe_emalloc(sizeof(corvid_field_def), (argc ? argc : 1), 0);
	names = safe_emalloc(sizeof(zend_string *), (argc ? argc : 1), 0);
	for (i = 0; i < argc; i++) {
		zval *pz;
		if (Z_TYPE(args[i]) != IS_OBJECT || !instanceof_function(Z_OBJCE(args[i]), corvid_fielddef_ce)) {
			php_corvid_throw_arg("setSchema wants Corvid\\FieldDef instances");
			goto cleanup;
		}
		pz = zend_read_property(corvid_fielddef_ce, Z_OBJ(args[i]), "name", sizeof("name") - 1, 0, &tmp);
		names[i] = zval_get_string(pz);
		pz = zend_read_property(corvid_fielddef_ce, Z_OBJ(args[i]), "type", sizeof("type") - 1, 0, &tmp);
		{
			zend_long t = zval_get_long(pz);
			zend_long r, u;

			if (t < 0 || t > 8) {
				zend_string_release(names[i]); /* captured this iteration; i was not yet bumped */
				php_corvid_throw_arg("field type must be one of Corvid\\FieldDef::TYPE_* (0..8)");
				goto cleanup;
			}
			pz = zend_read_property(corvid_fielddef_ce, Z_OBJ(args[i]), "required", sizeof("required") - 1, 0, &tmp);
			r = zval_get_long(pz);
			pz = zend_read_property(corvid_fielddef_ce, Z_OBJ(args[i]), "unique", sizeof("unique") - 1, 0, &tmp);
			u = zval_get_long(pz);

			defs[i].name     = ZSTR_VAL(names[i]);
			defs[i].name_len = ZSTR_LEN(names[i]);
			defs[i].type     = (corvid_field_type)t;
			defs[i].required = r ? 1 : 0;
			defs[i].unique   = u ? 1 : 0;
		}
	}

	if (corvid_set_schema(co->handle, defs, argc) != CORVID_OK) {
		php_corvid_throw_last("corvid_set_schema failed");
	}

cleanup:
	for (uint32_t j = 0; j < i; j++) { zend_string_release(names[j]); }
	efree(defs);
	efree(names);
	if (EG(exception)) { RETURN_THROWS(); }
}

PHP_METHOD(Corvid_Collection, schema)
{
	php_corvid_coll   *co = Z_CORVID_COLL_P(ZEND_THIS);
	corvid_schemaiter *it = NULL;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }

	if (corvid_schema(co->handle, &it) != CORVID_OK) {
		php_corvid_throw_last("corvid_schema failed");
		RETURN_THROWS();
	}
	if (it == NULL) { RETURN_NULL(); } /* absence is a success */

	array_init(return_value);
	{
		struct corvid_field_def fd;
		while (corvid_schemaiter_next(it, &fd) == 1) {
			zval fz, tv, rv, uv;
			object_init_ex(&fz, corvid_fielddef_ce);
			zend_update_property_stringl(corvid_fielddef_ce, Z_OBJ_P(&fz), "name",
				sizeof("name") - 1, fd.name, fd.name_len);
			ZVAL_LONG(&tv, (zend_long)fd.type);
			zend_update_property(corvid_fielddef_ce, Z_OBJ_P(&fz), "type", sizeof("type") - 1, &tv);
			ZVAL_BOOL(&rv, fd.required != 0);
			zend_update_property(corvid_fielddef_ce, Z_OBJ_P(&fz), "required", sizeof("required") - 1, &rv);
			ZVAL_BOOL(&uv, fd.unique != 0);
			zend_update_property(corvid_fielddef_ce, Z_OBJ_P(&fz), "unique", sizeof("unique") - 1, &uv);
			zend_hash_next_index_insert(Z_ARRVAL_P(return_value), &fz);
		}
	}
	corvid_schemaiter_free(it);
}

PHP_METHOD(Corvid_Collection, query)
{
	php_corvid_coll *co = Z_CORVID_COLL_P(ZEND_THIS);
	corvid_query    *h;
	zval             out;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_coll(co);
	if (EG(exception)) { RETURN_THROWS(); }
	h = corvid_query_new(co->handle);
	if (h == NULL) {
		php_corvid_throw_last("corvid_query_new failed");
		RETURN_THROWS();
	}
	object_init_ex(&out, corvid_query_ce);
	Z_CORVID_QUERY_P(&out)->handle = h;
	RETURN_ZVAL(&out, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Corvid\Query — the fluent builder (consumed by its terminals, §8)    */
/* ------------------------------------------------------------------ */

PHP_METHOD(Corvid_Query, filter)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zval             *zp;
	php_corvid_pred  *p;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(zp, corvid_pred_ce)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	p = Z_CORVID_PRED_P(zp);
	php_corvid_check_pred(p);
	if (EG(exception)) { RETURN_THROWS(); }

	{
		corvid_pred *ph = p->handle;
		p->handle = NULL; /* consumed unconditionally (§8) */
		if (corvid_query_filter(q->handle, ph) != CORVID_OK) {
			php_corvid_throw_last("corvid_query_filter failed");
			RETURN_THROWS();
		}
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, vector)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_string     *field;
	zval            *query;
	zend_long        k, metric = 0;
	size_t           dim = 0;
	float           *buf;

	ZEND_PARSE_PARAMETERS_START(3, 4)
		Z_PARAM_STR(field)
		Z_PARAM_ZVAL(query) /* Corvid\Vector or array of numbers */
		Z_PARAM_LONG(k)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(metric)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (metric < 0 || metric > 2) {
		php_corvid_throw_arg("metric must be Corvid\\Metric::COSINE/DOT/L2");
		RETURN_THROWS();
	}

	if (Z_TYPE_P(query) == IS_OBJECT && instanceof_function(Z_OBJCE_P(query), corvid_vector_ce)) {
		zval tmp, *pz = zend_read_property(corvid_vector_ce, Z_OBJ_P(query), "values", sizeof("values") - 1, 0, &tmp);
		buf = php_corvid_floats_from_array(pz, &dim);
	} else if (Z_TYPE_P(query) == IS_ARRAY) {
		buf = php_corvid_floats_from_array(query, &dim);
	} else {
		php_corvid_throw_arg("vector query must be a Corvid\\Vector or an array of numbers");
		RETURN_THROWS();
	}
	if (buf == NULL) { RETURN_THROWS(); }

	if (corvid_query_vector(q->handle, ZSTR_VAL(field), ZSTR_LEN(field), buf, dim, (size_t)k,
	        (corvid_metric)metric) != CORVID_OK) {
		efree(buf);
		php_corvid_throw_last("corvid_query_vector failed");
		RETURN_THROWS();
	}
	efree(buf);
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, text)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_string     *field, *query;
	zend_long        k;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(field)
		Z_PARAM_STR(query)
		Z_PARAM_LONG(k)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_query_text(q->handle, ZSTR_VAL(field), ZSTR_LEN(field),
	        ZSTR_VAL(query), ZSTR_LEN(query), (size_t)k) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_text failed");
		RETURN_THROWS();
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, fuseRrf)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	double            k;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_DOUBLE(k)
	ZEND_PARSE_PARAMETERS_END();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_query_fuse_rrf(q->handle, (float)k) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_fuse_rrf failed");
		RETURN_THROWS();
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, rerankMmr)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	double            lambda;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_DOUBLE(lambda)
	ZEND_PARSE_PARAMETERS_END();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_query_rerank_mmr(q->handle, (float)lambda) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_rerank_mmr failed");
		RETURN_THROWS();
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, approx)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_query_approx(q->handle) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_approx failed");
		RETURN_THROWS();
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, limit)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_long        n;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_query_limit(q->handle, (size_t)n) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_limit failed");
		RETURN_THROWS();
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, offset)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_long        n;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_query_offset(q->handle, (size_t)n) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_offset failed");
		RETURN_THROWS();
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, orderBy)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_string     *field;
	bool             descending = false;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_STR(field)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(descending)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	if (corvid_query_order_by(q->handle, ZSTR_VAL(field), ZSTR_LEN(field), descending ? 1 : 0) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_order_by failed");
		RETURN_THROWS();
	}
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

PHP_METHOD(Corvid_Query, select)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zval             *args;
	uint32_t          argc, i;
	const char      **fields;
	size_t           *lens;
	zend_string     **fields_z; /* keeps the coerced names alive across the ABI call */

	ZEND_PARSE_PARAMETERS_START(1, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }

	fields   = safe_emalloc(sizeof(char *), (argc ? argc : 1), 0);
	lens     = safe_emalloc(sizeof(size_t), (argc ? argc : 1), 0);
	fields_z = safe_emalloc(sizeof(zend_string *), (argc ? argc : 1), 0);
	for (i = 0; i < argc; i++) {
		/* fresh zend_strings for non-string args — released after the call */
		fields_z[i] = zval_get_string(&args[i]);
		fields[i]   = ZSTR_VAL(fields_z[i]);
		lens[i]     = ZSTR_LEN(fields_z[i]);
	}
	if (corvid_query_select(q->handle, (const char *const *)fields, lens, argc) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_select failed");
	}
	for (i = 0; i < argc; i++) { zend_string_release(fields_z[i]); }
	efree(fields); efree(lens); efree(fields_z);
	if (EG(exception)) { RETURN_THROWS(); }
	RETURN_ZVAL(ZEND_THIS, 1, 0);
}

/* The §8 unconditional-consumption discipline for the terminals: the
 * handle is detached BEFORE the call, whatever its outcome. */
static corvid_query *php_corvid_query_take(zval *zthis)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(zthis);
	corvid_query     *h = q->handle;
	q->handle = NULL;
	return h;
}

PHP_METHOD(Corvid_Query, run)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	corvid_query     *h;
	corvid_rows      *rows;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }

	h = php_corvid_query_take(ZEND_THIS);
	rows = corvid_query_run(h); /* consumes h */
	if (rows == NULL) {
		php_corvid_throw_last("corvid_query_run failed (ranking params validate here — audit C6)");
		RETURN_THROWS();
	}
	php_corvid_rows_to_array(rows, return_value);
}

PHP_METHOD(Corvid_Query, count)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	corvid_query     *h;
	size_t            n = 0;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	h = php_corvid_query_take(ZEND_THIS);
	if (corvid_query_count(h, &n) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_count failed");
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)n);
}

PHP_METHOD(Corvid_Query, countDistinct)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_string     *field;
	corvid_query     *h;
	size_t            n = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(field)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	h = php_corvid_query_take(ZEND_THIS);
	if (corvid_query_count_distinct(h, ZSTR_VAL(field), ZSTR_LEN(field), &n) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_count_distinct failed");
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)n);
}

PHP_METHOD(Corvid_Query, sum)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_string     *field;
	corvid_query     *h;
	double            d = 0.0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(field)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	h = php_corvid_query_take(ZEND_THIS);
	if (corvid_query_sum(h, ZSTR_VAL(field), ZSTR_LEN(field), &d) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_sum failed");
		RETURN_THROWS();
	}
	RETURN_DOUBLE(d);
}

PHP_METHOD(Corvid_Query, avg)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_string     *field;
	corvid_query     *h;
	double            d = 0.0;
	int               has = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(field)
	ZEND_PARSE_PARAMETERS_END();

	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	h = php_corvid_query_take(ZEND_THIS);
	if (corvid_query_avg(h, ZSTR_VAL(field), ZSTR_LEN(field), &d, &has) != CORVID_OK) {
		php_corvid_throw_last("corvid_query_avg failed");
		RETURN_THROWS();
	}
	if (!has) { RETURN_NULL(); } /* absence is a success */
	RETURN_DOUBLE(d);
}

#define PHP_CORVID_QUERY_MINMAX(mname, fn) \
PHP_METHOD(Corvid_Query, mname) \
{ \
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS); \
	zend_string     *field; \
	corvid_query     *h; \
	corvid_value    *out = NULL; \
	\
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(field) ZEND_PARSE_PARAMETERS_END(); \
	php_corvid_check_query(q); \
	if (EG(exception)) { RETURN_THROWS(); } \
	h = php_corvid_query_take(ZEND_THIS); \
	if (fn(h, ZSTR_VAL(field), ZSTR_LEN(field), &out) != CORVID_OK) { \
		php_corvid_throw_last(#fn " failed"); \
		RETURN_THROWS(); \
	} \
	if (out == NULL) { RETURN_NULL(); } \
	php_corvid_decode_into(out, return_value); \
	corvid_value_free(out); \
}

PHP_CORVID_QUERY_MINMAX(min, corvid_query_min)
PHP_CORVID_QUERY_MINMAX(max, corvid_query_max)

#define PHP_CORVID_QUERY_GROUP(mname, fn) \
PHP_METHOD(Corvid_Query, mname) \
{ \
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS); \
	zend_string     *groupField, *valueField; \
	corvid_query     *h; \
	corvid_groupiter *it; \
	\
	ZEND_PARSE_PARAMETERS_START(2, 2) \
		Z_PARAM_STR(groupField) \
		Z_PARAM_STR(valueField) \
	ZEND_PARSE_PARAMETERS_END(); \
	php_corvid_check_query(q); \
	if (EG(exception)) { RETURN_THROWS(); } \
	h = php_corvid_query_take(ZEND_THIS); \
	it = fn(h, ZSTR_VAL(groupField), ZSTR_LEN(groupField), ZSTR_VAL(valueField), ZSTR_LEN(valueField)); \
	if (it == NULL) { \
		php_corvid_throw_last(#fn " failed"); \
		RETURN_THROWS(); \
	} \
	php_corvid_groups_to_array(it, return_value); \
}

PHP_METHOD(Corvid_Query, groupCount)
{
	php_corvid_query *q = Z_CORVID_QUERY_P(ZEND_THIS);
	zend_string     *field;
	corvid_query     *h;
	corvid_groupiter *it;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(field)
	ZEND_PARSE_PARAMETERS_END();
	php_corvid_check_query(q);
	if (EG(exception)) { RETURN_THROWS(); }
	h = php_corvid_query_take(ZEND_THIS);
	it = corvid_query_group_count(h, ZSTR_VAL(field), ZSTR_LEN(field));
	if (it == NULL) {
		php_corvid_throw_last("corvid_query_group_count failed");
		RETURN_THROWS();
	}
	php_corvid_groups_to_array(it, return_value);
}

PHP_CORVID_QUERY_GROUP(groupSum, corvid_query_group_sum)
PHP_CORVID_QUERY_GROUP(groupAvg, corvid_query_group_avg)

/* ------------------------------------------------------------------ */
/* Corvid\Field + Corvid\Predicate                                      */
/* ------------------------------------------------------------------ */

static zend_string *php_corvid_field_path(zval *zthis)
{
	zval tmp, *pz = zend_read_property(corvid_field_ce, Z_OBJ_P(zthis), "path", sizeof("path") - 1, 0, &tmp);
	return zval_get_string(pz);
}

static void php_corvid_pred_new(corvid_pred *h, zval *out)
{
	object_init_ex(out, corvid_pred_ce);
	Z_CORVID_PRED_P(out)->handle = h;
}

#define PHP_CORVID_FIELD_CMP(mname, op) \
PHP_METHOD(Corvid_Field, mname) \
{ \
	zval        *value; \
	zend_string *path; \
	corvid_value *v; \
	corvid_pred *p; \
	\
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_ZVAL(value) ZEND_PARSE_PARAMETERS_END(); \
	path = php_corvid_field_path(ZEND_THIS); \
	v = php_corvid_encode(value, 0); \
	if (v == NULL) { zend_string_release(path); RETURN_THROWS(); } \
	p = corvid_pred_compare(ZSTR_VAL(path), ZSTR_LEN(path), op, v); \
	corvid_value_free(v); /* cloned into the tree */ \
	zend_string_release(path); \
	if (p == NULL) { php_corvid_throw_last("corvid_pred_compare failed"); RETURN_THROWS(); } \
	php_corvid_pred_new(p, return_value); \
}

PHP_CORVID_FIELD_CMP(eq, CORVID_CMP_EQ)
PHP_CORVID_FIELD_CMP(ne, CORVID_CMP_NE)
PHP_CORVID_FIELD_CMP(lt, CORVID_CMP_LT)
PHP_CORVID_FIELD_CMP(le, CORVID_CMP_LE)
PHP_CORVID_FIELD_CMP(gt, CORVID_CMP_GT)
PHP_CORVID_FIELD_CMP(ge, CORVID_CMP_GE)

PHP_METHOD(Corvid_Field, __construct)
{
	zend_string *path;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(path)
	ZEND_PARSE_PARAMETERS_END();
	zend_update_property_str(corvid_field_ce, Z_OBJ_P(ZEND_THIS), "path", sizeof("path") - 1, path);
}

PHP_METHOD(Corvid_Field, in)
{
	zval        *args;
	uint32_t     argc, i;
	zend_string *path;
	corvid_value **vals;
	corvid_pred  *p;

	ZEND_PARSE_PARAMETERS_START(1, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	path = php_corvid_field_path(ZEND_THIS);
	vals = safe_emalloc(sizeof(corvid_value *), (argc ? argc : 1), 0);
	for (i = 0; i < argc; i++) {
		vals[i] = php_corvid_encode(&args[i], 0);
		if (vals[i] == NULL) {
			while (i > 0) corvid_value_free(vals[--i]);
			efree(vals); zend_string_release(path);
			RETURN_THROWS();
		}
	}
	p = corvid_pred_in(ZSTR_VAL(path), ZSTR_LEN(path), (const corvid_value *const *)vals, argc);
	for (i = 0; i < argc; i++) { corvid_value_free(vals[i]); } /* cloned */
	efree(vals);
	zend_string_release(path);
	if (p == NULL) { php_corvid_throw_last("corvid_pred_in failed"); RETURN_THROWS(); }
	php_corvid_pred_new(p, return_value);
}

PHP_METHOD(Corvid_Field, between)
{
	zval         *low, *high;
	zend_string  *path;
	corvid_value *lo, *hi;
	corvid_pred  *p;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_ZVAL(low)
		Z_PARAM_ZVAL(high)
	ZEND_PARSE_PARAMETERS_END();

	path = php_corvid_field_path(ZEND_THIS);
	lo = php_corvid_encode(low, 0);
	if (lo == NULL) { zend_string_release(path); RETURN_THROWS(); }
	hi = php_corvid_encode(high, 0);
	if (hi == NULL) { corvid_value_free(lo); zend_string_release(path); RETURN_THROWS(); }
	p = corvid_pred_between(ZSTR_VAL(path), ZSTR_LEN(path), lo, hi);
	corvid_value_free(lo); corvid_value_free(hi); /* cloned */
	zend_string_release(path);
	if (p == NULL) { php_corvid_throw_last("corvid_pred_between failed"); RETURN_THROWS(); }
	php_corvid_pred_new(p, return_value);
}

#define PHP_CORVID_FIELD_TEXT(mname, fn) \
PHP_METHOD(Corvid_Field, mname) \
{ \
	zend_string *needle, *path; \
	corvid_pred *p; \
	\
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(needle) ZEND_PARSE_PARAMETERS_END(); \
	path = php_corvid_field_path(ZEND_THIS); \
	p = fn(ZSTR_VAL(path), ZSTR_LEN(path), ZSTR_VAL(needle), ZSTR_LEN(needle)); \
	zend_string_release(path); \
	if (p == NULL) { php_corvid_throw_last(#fn " failed"); RETURN_THROWS(); } \
	php_corvid_pred_new(p, return_value); \
}

PHP_CORVID_FIELD_TEXT(startsWith, corvid_pred_starts_with)
PHP_CORVID_FIELD_TEXT(contains, corvid_pred_contains)

PHP_METHOD(Corvid_Field, geoWithin)
{
	zend_string *path;
	double       lat, lon, radiusKm;
	corvid_pred *p;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_DOUBLE(lat)
		Z_PARAM_DOUBLE(lon)
		Z_PARAM_DOUBLE(radiusKm)
	ZEND_PARSE_PARAMETERS_END();

	path = php_corvid_field_path(ZEND_THIS);
	p = corvid_pred_geo_within(ZSTR_VAL(path), ZSTR_LEN(path), lat, lon, radiusKm);
	zend_string_release(path);
	if (p == NULL) { php_corvid_throw_last("corvid_pred_geo_within failed"); RETURN_THROWS(); }
	php_corvid_pred_new(p, return_value);
}

PHP_METHOD(Corvid_Field, exists)
{
	zend_string *path;
	corvid_pred *p;

	ZEND_PARSE_PARAMETERS_NONE();
	path = php_corvid_field_path(ZEND_THIS);
	p = corvid_pred_exists(ZSTR_VAL(path), ZSTR_LEN(path));
	zend_string_release(path);
	if (p == NULL) { php_corvid_throw_last("corvid_pred_exists failed"); RETURN_THROWS(); }
	php_corvid_pred_new(p, return_value);
}

/* Consuming combines: §8 — the children are consumed even when the
 * combine fails. Marking both consumed before the call makes the
 * double-free UB class unreachable through PHP. */
#define PHP_CORVID_PRED_COMBINE(mname, fn) \
PHP_METHOD(Corvid_Predicate, mname) \
{ \
	php_corvid_pred *self = Z_CORVID_PRED_P(ZEND_THIS); \
	zval            *zo; \
	php_corvid_pred *other; \
	corvid_pred     *a, *b, *p; \
	\
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_OBJECT_OF_CLASS(zo, corvid_pred_ce) ZEND_PARSE_PARAMETERS_END(); \
	php_corvid_check_pred(self); \
	if (EG(exception)) { RETURN_THROWS(); } \
	other = Z_CORVID_PRED_P(zo); \
	php_corvid_check_pred(other); \
	if (EG(exception)) { RETURN_THROWS(); } \
	a = self->handle;  self->handle  = NULL; \
	b = other->handle; other->handle = NULL; \
	p = fn(a, b); \
	if (p == NULL) { php_corvid_throw_last(#fn " failed"); RETURN_THROWS(); } \
	php_corvid_pred_new(p, return_value); \
}

PHP_CORVID_PRED_COMBINE(and, corvid_pred_and)
PHP_CORVID_PRED_COMBINE(or, corvid_pred_or)

PHP_METHOD(Corvid_Predicate, not)
{
	php_corvid_pred *self = Z_CORVID_PRED_P(ZEND_THIS);
	corvid_pred     *a, *p;

	ZEND_PARSE_PARAMETERS_NONE();
	php_corvid_check_pred(self);
	if (EG(exception)) { RETURN_THROWS(); }
	a = self->handle;
	self->handle = NULL;
	p = corvid_pred_not(a);
	if (p == NULL) { php_corvid_throw_last("corvid_pred_not failed"); RETURN_THROWS(); }
	php_corvid_pred_new(p, return_value);
}

/* ------------------------------------------------------------------ */
/* Corvid\Bytes / Corvid\Vector / Corvid\FieldDef                       */
/* ------------------------------------------------------------------ */

PHP_METHOD(Corvid_Bytes, __construct)
{
	zend_string *bytes;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(bytes)
	ZEND_PARSE_PARAMETERS_END();
	zend_update_property_str(corvid_bytes_ce, Z_OBJ_P(ZEND_THIS), "bytes", sizeof("bytes") - 1, bytes);
}

PHP_METHOD(Corvid_Bytes, __toString)
{
	zval tmp, *pz;

	ZEND_PARSE_PARAMETERS_NONE();
	pz = zend_read_property(corvid_bytes_ce, Z_OBJ_P(ZEND_THIS), "bytes", sizeof("bytes") - 1, 0, &tmp);
	RETURN_ZVAL(pz, 1, 0);
}

PHP_METHOD(Corvid_Vector, __construct)
{
	HashTable *values;
	zval       arr, *item;
	zend_long  i = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY_HT(values)
	ZEND_PARSE_PARAMETERS_END();

	array_init_size(&arr, zend_hash_num_elements(values));
	ZEND_HASH_FOREACH_VAL(values, item) {
		zval fz;
		switch (Z_TYPE_P(item)) {
			case IS_LONG:   ZVAL_DOUBLE(&fz, (double)Z_LVAL_P(item)); break;
			case IS_DOUBLE: ZVAL_DOUBLE(&fz, Z_DVAL_P(item));         break;
			default:
				zval_ptr_dtor(&arr);
				php_corvid_throw_arg("Corvid\\Vector wants an array of numbers");
				RETURN_THROWS();
		}
		zend_hash_next_index_insert(Z_ARRVAL(arr), &fz);
		i++;
	} ZEND_HASH_FOREACH_END();
	zend_update_property(corvid_vector_ce, Z_OBJ_P(ZEND_THIS), "values", sizeof("values") - 1, &arr);
	zval_ptr_dtor(&arr);
}

PHP_METHOD(Corvid_Vector, values)
{
	zval tmp, *pz;

	ZEND_PARSE_PARAMETERS_NONE();
	pz = zend_read_property(corvid_vector_ce, Z_OBJ_P(ZEND_THIS), "values", sizeof("values") - 1, 0, &tmp);
	RETURN_ZVAL(pz, 1, 0);
}

PHP_METHOD(Corvid_FieldDef, __construct)
{
	zend_string *name;
	zend_long    type;
	bool         required = false, unique = false;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STR(name)
		Z_PARAM_LONG(type)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(required)
		Z_PARAM_BOOL(unique)
	ZEND_PARSE_PARAMETERS_END();

	if (type < 0 || type > 8) {
		php_corvid_throw_arg("field type must be one of Corvid\\FieldDef::TYPE_* (0..8)");
		RETURN_THROWS();
	}
	zend_update_property_str(corvid_fielddef_ce, Z_OBJ_P(ZEND_THIS), "name", sizeof("name") - 1, name);
	zend_update_property_long(corvid_fielddef_ce, Z_OBJ_P(ZEND_THIS), "type", sizeof("type") - 1, type);
	zend_update_property_bool(corvid_fielddef_ce, Z_OBJ_P(ZEND_THIS), "required", sizeof("required") - 1, required);
	zend_update_property_bool(corvid_fielddef_ce, Z_OBJ_P(ZEND_THIS), "unique", sizeof("unique") - 1, unique);
}

/* ------------------------------------------------------------------ */
/* Corvid\Values — the value-mapping surface (harness + userland)       */
/* ------------------------------------------------------------------ */

static const char *php_corvid_type_names[9] = {
	"null", "bool", "int", "float", "text", "bytes", "array", "map", "vector",
};

PHP_METHOD(Corvid_Values, type)
{
	zval         *value;
	corvid_value *v;
	uint32_t      t;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	t = corvid_value_type(v);
	corvid_value_free(v);
	if (t > 8) {
		php_corvid_throw_arg("corvid_value_type returned an out-of-range tag %u", (unsigned)t);
		RETURN_THROWS();
	}
	RETURN_STRING(php_corvid_type_names[t]);
}

PHP_METHOD(Corvid_Values, len)
{
	zval         *value;
	corvid_value *v;
	size_t        n;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	n = corvid_value_len(v);
	corvid_value_free(v);
	RETURN_LONG((zend_long)n);
}

PHP_METHOD(Corvid_Values, asInt)
{
	zval         *value;
	corvid_value *v;
	int           ok = 0;
	int64_t       got;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	got = corvid_value_as_int(v, &ok);
	corvid_value_free(v);
	if (!ok) { RETURN_NULL(); }
	RETURN_LONG((zend_long)got);
}

PHP_METHOD(Corvid_Values, asFloat)
{
	zval         *value;
	corvid_value *v;
	int           ok = 0;
	double        got;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	got = corvid_value_as_float(v, &ok);
	corvid_value_free(v);
	if (!ok) { RETURN_NULL(); }
	RETURN_DOUBLE(got);
}

PHP_METHOD(Corvid_Values, asBool)
{
	zval         *value;
	corvid_value *v;
	int           ok = 0, got;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	got = corvid_value_as_bool(v, &ok);
	corvid_value_free(v);
	if (!ok) { RETURN_NULL(); }
	RETURN_BOOL(got != 0);
}

PHP_METHOD(Corvid_Values, asText)
{
	zval         *value;
	corvid_value *v;
	size_t        len = 0;
	const char   *s;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	s = corvid_value_text_ref(v, &len);
	if (s == NULL) {
		corvid_value_free(v);
		RETURN_NULL(); /* wrong type: the as_* Option convention */
	}
	RETVAL_STRINGL(s, len);
	corvid_value_free(v);
}

PHP_METHOD(Corvid_Values, asBytes)
{
	zval            *value;
	corvid_value    *v;
	size_t           len = 0;
	const uint8_t   *b;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	b = corvid_value_bytes_ref(v, &len);
	if (b == NULL) {
		corvid_value_free(v);
		RETURN_NULL();
	}
	{
		zval bv;
		object_init_ex(return_value, corvid_bytes_ce);
		ZVAL_STRINGL(&bv, (const char *)b, len);
		zend_update_property(corvid_bytes_ce, Z_OBJ_P(return_value), "bytes", sizeof("bytes") - 1, &bv);
		zval_ptr_dtor(&bv);
	}
	corvid_value_free(v);
}

PHP_METHOD(Corvid_Values, asVector)
{
	zval            *value;
	corvid_value    *v;
	size_t           dim = 0;
	const float     *f;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	f = corvid_value_vector_ref(v, &dim);
	if (f == NULL) {
		corvid_value_free(v);
		RETURN_NULL();
	}
	{
		zval arr, zv;
		size_t i;
		object_init_ex(return_value, corvid_vector_ce);
		array_init_size(&arr, (uint32_t)dim);
		for (i = 0; i < dim; i++) {
			ZVAL_DOUBLE(&zv, (double)f[i]);
			zend_hash_next_index_insert(Z_ARRVAL(arr), &zv);
		}
		zend_update_property(corvid_vector_ce, Z_OBJ_P(return_value), "values", sizeof("values") - 1, &arr);
		zval_ptr_dtor(&arr);
	}
	corvid_value_free(v);
}

PHP_METHOD(Corvid_Values, mapKeys)
{
	zval         *value;
	corvid_value *v;
	corvid_strs  *keys;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	keys = corvid_value_map_keys(v);
	corvid_value_free(v);
	if (keys == NULL) {
		php_corvid_throw_last("corvid_value_map_keys failed");
		RETURN_THROWS();
	}
	php_corvid_strs_to_array(keys, return_value);
}

PHP_METHOD(Corvid_Values, clone)
{
	zval         *value;
	corvid_value *v, *c;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	v = php_corvid_encode(value, 0);
	if (v == NULL) { RETURN_THROWS(); }
	c = corvid_value_clone(v);
	corvid_value_free(v);
	if (c == NULL) {
		php_corvid_throw_last("corvid_value_clone failed");
		RETURN_THROWS();
	}
	php_corvid_decode_into(c, return_value);
	corvid_value_free(c);
}

PHP_METHOD(Corvid_Values, push)
{
	zval         *container, *item;
	corvid_value *arr, *it;
	size_t        n;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_ZVAL(container)
		Z_PARAM_ZVAL(item)
	ZEND_PARSE_PARAMETERS_END();

	arr = php_corvid_encode(container, 0);
	if (arr == NULL) { RETURN_THROWS(); }
	it = php_corvid_encode(item, 0);
	if (it == NULL) { corvid_value_free(arr); RETURN_THROWS(); }
	if (corvid_value_array_push(arr, it) != CORVID_OK) { /* consumes it (§8) */
		php_corvid_throw_last("corvid_value_array_push failed (not an array value?)");
		corvid_value_free(arr);
		RETURN_THROWS();
	}
	n = corvid_value_len(arr);
	corvid_value_free(arr);
	RETURN_LONG((zend_long)n);
}

PHP_METHOD(Corvid_Values, put)
{
	zval         *map, *value;
	zend_string  *key;
	corvid_value *m, *v;
	size_t        n;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_ZVAL(map)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	m = php_corvid_encode(map, 0);
	if (m == NULL) { RETURN_THROWS(); }
	v = php_corvid_encode(value, 0);
	if (v == NULL) { corvid_value_free(m); RETURN_THROWS(); }
	if (corvid_value_map_put(m, ZSTR_VAL(key), ZSTR_LEN(key), v) != CORVID_OK) { /* consumes v (§8) */
		php_corvid_throw_last("corvid_value_map_put failed (not a map value?)");
		corvid_value_free(m);
		RETURN_THROWS();
	}
	n = corvid_value_len(m);
	corvid_value_free(m);
	RETURN_LONG((zend_long)n);
}

PHP_METHOD(Corvid_Values, selfCheck)
{
	/* The §7 inert shapes + the ABI version gate, exercised from PHP:
	 * every _free(NULL) is a documented no-op, NULL-cursor next answers
	 * its inert value, corvid_value_type(NULL) answers TYPE_NULL. */
	ZEND_PARSE_PARAMETERS_NONE();

	if (corvid_ffi_version() != 1) {
		php_corvid_throw_arg("corvid_ffi_version() != 1");
		RETURN_THROWS();
	}
	corvid_collection_free(NULL);
	corvid_query_free(NULL);
	corvid_pred_free(NULL);
	corvid_rows_free(NULL);
	corvid_strs_free(NULL);
	corvid_geohits_free(NULL);
	corvid_groupiter_free(NULL);
	corvid_schemaiter_free(NULL);
	corvid_value_free(NULL);
	corvid_free(NULL);
	if (corvid_groupiter_next(NULL, NULL, NULL, NULL) != 0) {
		php_corvid_throw_arg("groupiter_next(NULL) must answer 0");
		RETURN_THROWS();
	}
	if (corvid_value_type(NULL) != CORVID_TYPE_NULL) {
		php_corvid_throw_arg("value_type(NULL) must answer TYPE_NULL");
		RETURN_THROWS();
	}
	RETURN_TRUE;
}

/* ------------------------------------------------------------------ */
/* Corvid\ffiVersion()                                                  */
/* ------------------------------------------------------------------ */

PHP_FUNCTION(corvid_ffi_version)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long)corvid_ffi_version());
}

/* ------------------------------------------------------------------ */
/* class tables + MINIT                                                 */
/* ------------------------------------------------------------------ */

static const zend_function_entry corvid_functions[] = {
	/* ZEND_RAW_FENTRY grew two attribute-hook arguments in PHP 8.4 —
	 * the floor is 8.3, so the arity is version-gated, not assumed. */
#if PHP_VERSION_ID >= 80400
	ZEND_RAW_FENTRY("Corvid\\ffiVersion", PHP_FN(corvid_ffi_version), arginfo_corvid_ffi_version, 0, NULL, NULL)
#else
	ZEND_RAW_FENTRY("Corvid\\ffiVersion", PHP_FN(corvid_ffi_version), arginfo_corvid_ffi_version, 0)
#endif
	PHP_FE_END
};

static const zend_function_entry corvid_db_methods[] = {
	PHP_ME(Corvid_Db, __construct,       arginfo_db_nop,                ZEND_ACC_PRIVATE)
	PHP_ME(Corvid_Db, open,              arginfo_db_open,               ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Db, openMemory,        arginfo_db_open_memory,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Db, collection,        arginfo_db_collection,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Db, collections,       arginfo_db_collections,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Db, dump,              arginfo_db_dump,               ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Db, load,              arginfo_db_load,               ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Db, loadWithRenames,   arginfo_db_load_with_renames,  ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Db, backup,            arginfo_db_backup,             ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Db, compact,           arginfo_db_compact,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Db, close,             arginfo_db_close,              ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_coll_methods[] = {
	PHP_ME(Corvid_Collection, name,                       arginfo_coll_name,                          ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, insert,                     arginfo_coll_insert,                        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, putMany,                    arginfo_coll_put_many,                      ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, insertAuto,                 arginfo_coll_insert_auto,                   ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, update,                     arginfo_coll_update,                        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, patch,                      arginfo_coll_patch,                         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, compareAndSet,              arginfo_coll_compare_and_set,               ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, delete,                     arginfo_coll_delete,                        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, deleteWhere,                arginfo_coll_delete_where,                  ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, deleteBatch,                arginfo_coll_delete_batch,                  ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, insertWithTtl,              arginfo_coll_insert_with_ttl,               ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, setTtl,                     arginfo_coll_set_ttl,                       ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, getTtl,                     arginfo_coll_get_ttl,                       ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, purgeExpired,               arginfo_coll_purge_expired,                 ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, get,                        arginfo_coll_get,                           ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, getFields,                  arginfo_coll_get_fields,                    ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, scan,                       arginfo_coll_scan,                          ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, page,                       arginfo_coll_page,                          ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, len,                        arginfo_coll_len,                           ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, phraseSearch,               arginfo_coll_phrase_search,                 ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, link,                       arginfo_coll_link,                          ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, linkWeighted,               arginfo_coll_link_weighted,                 ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, unlink,                     arginfo_coll_unlink,                        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, neighbors,                  arginfo_coll_neighbors,                     ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, inNeighbors,                arginfo_coll_in_neighbors,                  ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, neighborsWeighted,          arginfo_coll_neighbors_weighted,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, traverse,                   arginfo_coll_traverse,                      ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, geoWithinRadius,            arginfo_coll_geo_within_radius,             ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, geoWithinBBox,              arginfo_coll_geo_within_bbox,               ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, geoNearest,                 arginfo_coll_geo_nearest,                   ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createScalarIndex,          arginfo_coll_create_scalar_index,           ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createCompoundIndex,        arginfo_coll_create_compound_index,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createTextIndex,            arginfo_coll_create_text_index,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createTextIndexOnDisk,      arginfo_coll_create_text_index_ondisk,      ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createGeoIndex,             arginfo_coll_create_geo_index,             ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createVectorIndex,          arginfo_coll_create_vector_index,           ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createVectorIndexQuantized, arginfo_coll_create_vector_index_quantized, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createVectorIndexOnDisk,    arginfo_coll_create_vector_index_ondisk,    ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createVectorIndexOnDiskQuantized, arginfo_coll_create_vector_index_ondisk_quantized, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createVectorIndexPQ,        arginfo_coll_create_vector_index_pq,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, createVectorIndexOnDiskPQ,  arginfo_coll_create_vector_index_ondisk_pq, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, setSchema,                  arginfo_coll_set_schema,                    ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, schema,                     arginfo_coll_schema,                        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Collection, query,                      arginfo_coll_query,                         ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_query_methods[] = {
	PHP_ME(Corvid_Query, filter,        arginfo_query_filter,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, vector,        arginfo_query_vector,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, text,          arginfo_query_text,           ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, fuseRrf,       arginfo_query_fuse_rrf,       ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, rerankMmr,     arginfo_query_rerank_mmr,     ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, approx,        arginfo_query_approx,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, limit,         arginfo_query_limit,          ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, offset,        arginfo_query_offset,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, orderBy,       arginfo_query_order_by,       ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, select,        arginfo_query_select,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, run,           arginfo_query_run,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, count,         arginfo_query_count,          ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, countDistinct, arginfo_query_count_distinct, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, sum,           arginfo_query_sum,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, avg,           arginfo_query_avg,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, min,           arginfo_query_min,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, max,           arginfo_query_max,            ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, groupCount,    arginfo_query_group_count,    ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, groupSum,      arginfo_query_group_sum,      ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Query, groupAvg,      arginfo_query_group_avg,      ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_pred_methods[] = {
	PHP_ME(Corvid_Predicate, and, arginfo_pred_and, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Predicate, or,  arginfo_pred_or,  ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Predicate, not, arginfo_pred_not, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_field_methods[] = {
	PHP_ME(Corvid_Field, __construct, arginfo_field_construct,  ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, eq,          arginfo_field_cmp,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, ne,          arginfo_field_cmp,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, lt,          arginfo_field_cmp,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, le,          arginfo_field_cmp,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, gt,          arginfo_field_cmp,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, ge,          arginfo_field_cmp,        ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, in,          arginfo_field_in,         ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, between,     arginfo_field_between,    ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, startsWith,  arginfo_field_starts_with, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, contains,    arginfo_field_contains,   ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, geoWithin,   arginfo_field_geo_within, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Field, exists,      arginfo_field_exists,     ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_bytes_methods[] = {
	PHP_ME(Corvid_Bytes, __construct, arginfo_bytes_construct,  ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Bytes, __toString,  arginfo_bytes_to_string,  ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_vector_methods[] = {
	PHP_ME(Corvid_Vector, __construct, arginfo_vector_construct, ZEND_ACC_PUBLIC)
	PHP_ME(Corvid_Vector, values,      arginfo_vector_values,   ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_fielddef_methods[] = {
	PHP_ME(Corvid_FieldDef, __construct, arginfo_fielddef_construct, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry corvid_values_methods[] = {
	PHP_ME(Corvid_Values, type,      arginfo_values_type,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, len,       arginfo_values_len,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, asInt,     arginfo_values_as_int,     ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, asFloat,   arginfo_values_as_float,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, asBool,    arginfo_values_as_bool,    ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, asText,    arginfo_values_as_text,    ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, asBytes,   arginfo_values_as_bytes,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, asVector,  arginfo_values_as_vector,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, mapKeys,   arginfo_values_map_keys,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, clone,     arginfo_values_clone,      ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, push,      arginfo_values_push,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, put,       arginfo_values_put,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Corvid_Values, selfCheck, arginfo_values_self_check, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_FE_END
};

#define PHP_CORVID_PROP(cvar, pname) \
	zend_declare_property_null(cvar, pname, strlen(pname), ZEND_ACC_PUBLIC)

#define PHP_CORVID_CONST_LONG(cvar, cname, value) \
	zend_declare_class_constant_long(cvar, cname, strlen(cname), (zend_long)value)

PHP_MINIT_FUNCTION(corvid)
{
	zend_class_entry ce;

	/* The ABI version gate — bindings verify before anything else
	 * (FFI.md §4.1). A mismatched cdylib fails the load, loudly. */
	if (corvid_ffi_version() != 1) {
		zend_error(E_ERROR, "corvid: libcorvid FFI version %u != 1 — the extension and the cdylib disagree",
			(unsigned)corvid_ffi_version());
		return FAILURE;
	}

	/* Corvid\Exception — getCode() carries the corvid_err table. */
	INIT_CLASS_ENTRY(ce, "Corvid\\Exception", NULL);
	corvid_exception_ce = zend_register_internal_class_ex(&ce, zend_ce_exception);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_OK",                   CORVID_E_OK);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_DATABASE",             CORVID_E_DATABASE);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_TRANSACTION",          CORVID_E_TRANSACTION);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_TABLE",                CORVID_E_TABLE);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_STORAGE",              CORVID_E_STORAGE);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_COMMIT",               CORVID_E_COMMIT);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_SET_DURABILITY",       CORVID_E_SET_DURABILITY);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_COMPACTION",           CORVID_E_COMPACTION);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_DECODE",               CORVID_E_DECODE);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_CORRUPT_INDEX",        CORVID_E_CORRUPT_INDEX);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_RESERVED_COLLECTION",  CORVID_E_RESERVED_COLLECTION);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_INVALID_NAME",         CORVID_E_INVALID_NAME);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_ARGUMENT",             CORVID_E_ARGUMENT);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_INCOMPATIBLE_FORMAT",  CORVID_E_INCOMPATIBLE_FORMAT);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_EMPTY_INDEX_TRAINING", CORVID_E_EMPTY_INDEX_TRAINING);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_SCHEMA_VIOLATION",     CORVID_E_SCHEMA_VIOLATION);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_INVALID_DUMP",         CORVID_E_INVALID_DUMP);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_BACKUP_TARGET_EXISTS", CORVID_E_BACKUP_TARGET_EXISTS);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_IO",                   CORVID_E_IO);
	PHP_CORVID_CONST_LONG(corvid_exception_ce, "CODE_BUSY",                 CORVID_E_BUSY);

	/* Corvid\Db */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Db", strlen("Corvid\\Db"), corvid_db_methods);
	corvid_db_ce = zend_register_internal_class_ex(&ce, NULL);
	corvid_db_ce->create_object = php_corvid_db_create;
	memcpy(&php_corvid_db_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_corvid_db_handlers.offset          = XtOffsetOf(php_corvid_db, std);
	php_corvid_db_handlers.free_obj        = php_corvid_db_free;
	php_corvid_db_handlers.clone_obj       = NULL; /* handles are not cloneable */

	/* Corvid\Collection */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Collection", strlen("Corvid\\Collection"), corvid_coll_methods);
	corvid_coll_ce = zend_register_internal_class_ex(&ce, NULL);
	corvid_coll_ce->create_object = php_corvid_coll_create;
	memcpy(&php_corvid_coll_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_corvid_coll_handlers.offset   = XtOffsetOf(php_corvid_coll, std);
	php_corvid_coll_handlers.free_obj  = php_corvid_coll_free;
	php_corvid_coll_handlers.clone_obj = NULL;

	/* Corvid\Query — fluent; consumed by run()/aggregates */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Query", strlen("Corvid\\Query"), corvid_query_methods);
	corvid_query_ce = zend_register_internal_class_ex(&ce, NULL);
	corvid_query_ce->create_object = php_corvid_query_create;
	memcpy(&php_corvid_query_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_corvid_query_handlers.offset   = XtOffsetOf(php_corvid_query, std);
	php_corvid_query_handlers.free_obj  = php_corvid_query_free;
	php_corvid_query_handlers.clone_obj = NULL;

	/* Corvid\Predicate — consumed by and/or/not/filter/deleteWhere */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Predicate", strlen("Corvid\\Predicate"), corvid_pred_methods);
	corvid_pred_ce = zend_register_internal_class_ex(&ce, NULL);
	corvid_pred_ce->create_object = php_corvid_pred_create;
	memcpy(&php_corvid_pred_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_corvid_pred_handlers.offset   = XtOffsetOf(php_corvid_pred, std);
	php_corvid_pred_handlers.free_obj  = php_corvid_pred_free;
	php_corvid_pred_handlers.clone_obj = NULL;

	/* Corvid\Field — the predicate factory */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Field", strlen("Corvid\\Field"), corvid_field_methods);
	corvid_field_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_field_ce, "path");

	/* Corvid\Row / Corvid\Page / Corvid\GeoHit / Corvid\WeightedHit / Corvid\Group */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Row", strlen("Corvid\\Row"), NULL);
	corvid_row_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_row_ce, "key");
	PHP_CORVID_PROP(corvid_row_ce, "doc");
	PHP_CORVID_PROP(corvid_row_ce, "score");

	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Page", strlen("Corvid\\Page"), NULL);
	corvid_page_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_page_ce, "rows");
	PHP_CORVID_PROP(corvid_page_ce, "next");

	INIT_CLASS_ENTRY_EX(ce, "Corvid\\GeoHit", strlen("Corvid\\GeoHit"), NULL);
	corvid_geohit_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_geohit_ce, "key");
	PHP_CORVID_PROP(corvid_geohit_ce, "distanceKm");
	PHP_CORVID_PROP(corvid_geohit_ce, "doc");

	INIT_CLASS_ENTRY_EX(ce, "Corvid\\WeightedHit", strlen("Corvid\\WeightedHit"), NULL);
	corvid_whit_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_whit_ce, "key");
	PHP_CORVID_PROP(corvid_whit_ce, "weight");

	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Group", strlen("Corvid\\Group"), NULL);
	corvid_group_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_group_ce, "key");
	PHP_CORVID_PROP(corvid_group_ce, "value");

	/* Corvid\Bytes — the byte-typed string (binary-safe) */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Bytes", strlen("Corvid\\Bytes"), corvid_bytes_methods);
	corvid_bytes_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_bytes_ce, "bytes");

	/* Corvid\Vector — the f32 embedding (an array of floats under a tag) */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Vector", strlen("Corvid\\Vector"), corvid_vector_methods);
	corvid_vector_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_vector_ce, "values");

	/* Corvid\FieldDef + the frozen field-type discriminants (§1.4) */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\FieldDef", strlen("Corvid\\FieldDef"), corvid_fielddef_methods);
	corvid_fielddef_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_PROP(corvid_fielddef_ce, "name");
	PHP_CORVID_PROP(corvid_fielddef_ce, "type");
	PHP_CORVID_PROP(corvid_fielddef_ce, "required");
	PHP_CORVID_PROP(corvid_fielddef_ce, "unique");
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_ANY",    CORVID_FIELD_ANY);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_BOOL",   CORVID_FIELD_BOOL);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_INT",    CORVID_FIELD_INT);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_FLOAT",  CORVID_FIELD_FLOAT);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_TEXT",   CORVID_FIELD_TEXT);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_BYTES",  CORVID_FIELD_BYTES);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_VECTOR", CORVID_FIELD_VECTOR);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_ARRAY",  CORVID_FIELD_ARRAY);
	PHP_CORVID_CONST_LONG(corvid_fielddef_ce, "TYPE_MAP",    CORVID_FIELD_MAP);

	/* Corvid\Metric / Corvid\Quant — frozen discriminants (§1.4) */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Metric", strlen("Corvid\\Metric"), NULL);
	corvid_metric_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_CONST_LONG(corvid_metric_ce, "COSINE", CORVID_METRIC_COSINE);
	PHP_CORVID_CONST_LONG(corvid_metric_ce, "DOT",    CORVID_METRIC_DOT);
	PHP_CORVID_CONST_LONG(corvid_metric_ce, "L2",     CORVID_METRIC_L2);

	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Quant", strlen("Corvid\\Quant"), NULL);
	corvid_quant_ce = zend_register_internal_class_ex(&ce, NULL);
	PHP_CORVID_CONST_LONG(corvid_quant_ce, "NONE",   CORVID_QUANT_NONE);
	PHP_CORVID_CONST_LONG(corvid_quant_ce, "BINARY", CORVID_QUANT_BINARY);
	PHP_CORVID_CONST_LONG(corvid_quant_ce, "SCALAR", CORVID_QUANT_SCALAR);

	/* Corvid\Values — the value-mapping surface */
	INIT_CLASS_ENTRY_EX(ce, "Corvid\\Values", strlen("Corvid\\Values"), corvid_values_methods);
	corvid_values_ce = zend_register_internal_class_ex(&ce, NULL);

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(corvid)
{
	/* Nothing engine-side to tear down: the corvid ABI has no global
	 * init/teardown; every handle died with its PHP object. */
	return SUCCESS;
}

PHP_MINFO_FUNCTION(corvid)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "corvid support", "enabled");
	php_info_print_table_row(2, "extension version", PHP_CORVID_VERSION);
	php_info_print_table_row(2, "libcorvid FFI version", "1 (corvid engine v0.3.0 pin)");
	php_info_print_table_end();
}

zend_module_entry corvid_module_entry = {
	STANDARD_MODULE_HEADER,
	PHP_CORVID_EXTNAME,
	corvid_functions,          /* Corvid\ffiVersion() */
	PHP_MINIT(corvid),         /* per process: classes + the ABI version gate */
	PHP_MSHUTDOWN(corvid),     /* per process: nothing to do */
	NULL,                      /* RINIT: nothing request-scoped exists */
	NULL,                      /* RSHUTDOWN: ditto */
	PHP_MINFO(corvid),
	PHP_CORVID_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_CORVID
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(corvid)
#endif
