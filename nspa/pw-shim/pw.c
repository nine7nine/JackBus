/*
 * jackbus-pw: library setup, logging, versions and pw_properties.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>

/* compile the header's inline helpers as exported functions, as
 * libpipewire itself does, for callers that link them by name */
#define PW_API_PROPERTIES SHIM_EXPORT
#define SHIM_EXPORT __attribute__((visibility("default")))
#include <pipewire/properties.h>

#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include "shim.h"

/* ------------------------------------------------------------------ log */

int shim_verbose;

SHIM_EXPORT enum spa_log_level pw_log_level = SPA_LOG_LEVEL_WARN;
SHIM_EXPORT struct spa_log_topic * const PW_LOG_TOPIC_DEFAULT = NULL;

static const char level_char[] = "-EWIDT";

void shim_log(const char *fmt, ...)
{
	va_list ap;
	if (!shim_verbose)
		return;
	va_start(ap, fmt);
	fputs("jackbus-pw: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void shim_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("jackbus-pw: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

SHIM_EXPORT void pw_log_logtv(enum spa_log_level level,
		const struct spa_log_topic *topic, const char *file, int line,
		const char *func, const char *fmt, va_list args)
{
	if (level > pw_log_level)
		return;
	fprintf(stderr, "[%c][%s] ", level_char[(unsigned)level <= 5 ? (unsigned)level : 5u],
			topic ? topic->topic : "app");
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
}

SHIM_EXPORT void pw_log_logt(enum spa_log_level level,
		const struct spa_log_topic *topic, const char *file, int line,
		const char *func, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	pw_log_logtv(level, topic, file, line, func, fmt, ap);
	va_end(ap);
}

SHIM_EXPORT void pw_log_logv(enum spa_log_level level, const char *file, int line,
		const char *func, const char *fmt, va_list args)
{
	pw_log_logtv(level, NULL, file, line, func, fmt, args);
}

SHIM_EXPORT void pw_log_log(enum spa_log_level level, const char *file, int line,
		const char *func, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	pw_log_logtv(level, NULL, file, line, func, fmt, ap);
	va_end(ap);
}

SHIM_EXPORT void pw_log_set_level(enum spa_log_level level) { pw_log_level = level; }
SHIM_EXPORT void pw_log_set(struct spa_log *log) { (void)log; }
SHIM_EXPORT struct spa_log *pw_log_get(void) { return NULL; }
SHIM_EXPORT void pw_log_topic_register(struct spa_log_topic *t) { (void)t; }
SHIM_EXPORT void pw_log_topic_unregister(struct spa_log_topic *t) { (void)t; }

SHIM_EXPORT int pw_log_set_level_string(const char *str)
{
	/* "3", "I", or "*:3,..." — the global level is enough here */
	const char *p = strrchr(str, ':');
	char c = p ? p[1] : str[0];
	const char *l = strchr(level_char, c);
	if (c >= '0' && c <= '5')
		pw_log_level = c - '0';
	else if (l != NULL && c != '-')
		pw_log_level = l - level_char;
	else
		return -EINVAL;
	return 0;
}

SHIM_EXPORT bool pw_debug_is_category_enabled(const char *name) { (void)name; return false; }

/* ----------------------------------------------------------- init, misc */

static int init_count;
static char *prgname;

SHIM_EXPORT void pw_init(int *argc, char **argv[])
{
	const char *str;

	if (init_count++ > 0)
		return;
	if ((str = getenv("JACKBUS_PW_VERBOSE")) != NULL)
		shim_verbose = atoi(str);
	if ((str = getenv("PIPEWIRE_DEBUG")) != NULL)
		pw_log_set_level_string(str);
	if (argc && argv && *argc > 0 && (*argv)[0])
		prgname = strdup(basename((*argv)[0]));
	shim_log("pw_init: libpipewire-0.3 shim on JackBus (API of PipeWire %s)",
			pw_get_headers_version());
}

SHIM_EXPORT void pw_deinit(void)
{
	if (init_count > 0)
		init_count--;
}

SHIM_EXPORT const char *pw_get_library_version(void)
{
	/* the PipeWire version whose headers (and so struct layouts) we serve */
	return pw_get_headers_version();
}

SHIM_EXPORT bool pw_check_library_version(int major, int minor, int micro)
{
	return PW_CHECK_VERSION(major, minor, micro);
}

SHIM_EXPORT const char *pw_get_prgname(void)
{
	if (prgname == NULL) {
		char buf[256];
		ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = 0;
			prgname = strdup(basename(buf));
		}
	}
	return prgname;
}

SHIM_EXPORT const char *pw_get_application_name(void) { return NULL; }

SHIM_EXPORT const char *pw_get_user_name(void)
{
	struct passwd *pw = getpwuid(getuid());
	return pw ? pw->pw_name : "";
}

SHIM_EXPORT const char *pw_get_host_name(void)
{
	static char host[256];
	if (host[0] == 0 && gethostname(host, sizeof(host) - 1) < 0)
		return NULL;
	return host;
}

SHIM_EXPORT const char *pw_get_client_name(void)
{
	return pw_get_prgname();
}

SHIM_EXPORT bool pw_check_option(const char *option, const char *value)
{
	(void)option; (void)value;
	return false;
}

SHIM_EXPORT enum pw_direction pw_direction_reverse(enum pw_direction direction)
{
	return direction == PW_DIRECTION_INPUT ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
}

static char *domain;
SHIM_EXPORT int pw_set_domain(const char *d) { free(domain); domain = d ? strdup(d) : NULL; return 0; }
SHIM_EXPORT const char *pw_get_domain(void) { return domain; }

/* ---------------------------------------------------------- properties */

struct props_impl {
	struct pw_properties this;
	uint32_t alloc;
};

static struct props_impl *props_alloc(void)
{
	struct props_impl *impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;
	impl->alloc = 16;
	impl->this.dict.items = calloc(impl->alloc, sizeof(struct spa_dict_item));
	if (impl->this.dict.items == NULL) {
		free(impl);
		return NULL;
	}
	return impl;
}

static int props_find(const struct pw_properties *p, const char *key)
{
	for (uint32_t i = 0; i < p->dict.n_items; i++)
		if (spa_streq(p->dict.items[i].key, key))
			return (int)i;
	return -1;
}

/* takes ownership of key and value */
static int props_set_owned(struct pw_properties *p, char *key, char *value)
{
	struct props_impl *impl = SPA_CONTAINER_OF(p, struct props_impl, this);
	struct spa_dict_item *items = (struct spa_dict_item *)p->dict.items;
	int i = props_find(p, key);

	if (i >= 0) {
		if (value == NULL) {
			free((char *)items[i].key);
			free((char *)items[i].value);
			items[i] = items[--p->dict.n_items];
			free(key);
			return 1;
		}
		if (spa_streq(items[i].value, value)) {
			free(key);
			free(value);
			return 0;
		}
		free((char *)items[i].value);
		items[i].value = value;
		free(key);
		return 1;
	}
	if (value == NULL) {
		free(key);
		return 0;
	}
	if (p->dict.n_items == impl->alloc) {
		void *n = realloc(items, impl->alloc * 2 * sizeof(*items));
		if (n == NULL) {
			free(key);
			free(value);
			return -errno;
		}
		impl->alloc *= 2;
		p->dict.items = items = n;
	}
	items[p->dict.n_items++] = SPA_DICT_ITEM_INIT(key, value);
	return 1;
}

SHIM_EXPORT int pw_properties_set(struct pw_properties *p, const char *key, const char *value)
{
	if (p == NULL || key == NULL)
		return -EINVAL;
	return props_set_owned(p, strdup(key), value ? strdup(value) : NULL);
}

SHIM_EXPORT int pw_properties_setva(struct pw_properties *p, const char *key,
		const char *format, va_list args)
{
	char *value = NULL;
	if (format != NULL && vasprintf(&value, format, args) < 0)
		return -errno;
	return props_set_owned(p, strdup(key), value);
}

SHIM_EXPORT int pw_properties_setf(struct pw_properties *p, const char *key,
		const char *format, ...)
{
	va_list ap;
	int res;
	va_start(ap, format);
	res = pw_properties_setva(p, key, format, ap);
	va_end(ap);
	return res;
}

SHIM_EXPORT const char *pw_properties_get(const struct pw_properties *p, const char *key)
{
	int i;
	if (p == NULL || key == NULL)
		return NULL;
	i = props_find(p, key);
	return i < 0 ? NULL : p->dict.items[i].value;
}

SHIM_EXPORT struct pw_properties *pw_properties_new(const char *key, ...)
{
	struct props_impl *impl = props_alloc();
	va_list ap;
	if (impl == NULL)
		return NULL;
	va_start(ap, key);
	while (key != NULL) {
		const char *value = va_arg(ap, char *);
		if (value && key[0])
			pw_properties_set(&impl->this, key, value);
		key = va_arg(ap, char *);
	}
	va_end(ap);
	return &impl->this;
}

SHIM_EXPORT int pw_properties_update(struct pw_properties *p, const struct spa_dict *dict)
{
	int changed = 0;
	const struct spa_dict_item *it;
	if (dict == NULL)
		return 0;
	spa_dict_for_each(it, dict)
		changed += pw_properties_set(p, it->key, it->value) > 0;
	return changed;
}

SHIM_EXPORT int pw_properties_update_keys(struct pw_properties *p,
		const struct spa_dict *dict, const char * const keys[])
{
	int changed = 0;
	for (int i = 0; keys[i]; i++) {
		const char *v = spa_dict_lookup(dict, keys[i]);
		if (v)
			changed += pw_properties_set(p, keys[i], v) > 0;
	}
	return changed;
}

SHIM_EXPORT int pw_properties_update_ignore(struct pw_properties *p,
		const struct spa_dict *dict, const char * const ignore[])
{
	int changed = 0;
	const struct spa_dict_item *it;
	spa_dict_for_each(it, dict) {
		bool skip = false;
		for (int i = 0; ignore && ignore[i]; i++)
			if (spa_streq(ignore[i], it->key))
				skip = true;
		if (!skip)
			changed += pw_properties_set(p, it->key, it->value) > 0;
	}
	return changed;
}

SHIM_EXPORT int pw_properties_add(struct pw_properties *p, const struct spa_dict *dict)
{
	int added = 0;
	const struct spa_dict_item *it;
	spa_dict_for_each(it, dict)
		if (pw_properties_get(p, it->key) == NULL)
			added += pw_properties_set(p, it->key, it->value) > 0;
	return added;
}

SHIM_EXPORT int pw_properties_add_keys(struct pw_properties *p,
		const struct spa_dict *dict, const char * const keys[])
{
	int added = 0;
	for (int i = 0; keys[i]; i++) {
		const char *v = spa_dict_lookup(dict, keys[i]);
		if (v && pw_properties_get(p, keys[i]) == NULL)
			added += pw_properties_set(p, keys[i], v) > 0;
	}
	return added;
}

/* the "key=value key2=value2" / SPA-JSON object syntax of PIPEWIRE_PROPS */
SHIM_EXPORT int pw_properties_update_string_checked(struct pw_properties *p,
		const char *str, size_t size, struct spa_error_location *loc)
{
	struct spa_json it[1];
	char key[1024];
	const char *value;
	int len, changed = 0;

	(void)loc;
	if (spa_json_begin_object_relax(&it[0], str, size) <= 0)
		return 0;
	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &value)) > 0) {
		char *val;
		if (spa_json_is_container(value, len))
			len = spa_json_container_len(&it[0], value, len);
		if (len <= 0)
			break;
		if (spa_json_is_null(value, len)) {
			changed += props_set_owned(p, strdup(key), NULL) > 0;
			continue;
		}
		if ((val = malloc(len + 1)) == NULL)
			return -errno;
		if (spa_json_parse_stringn(value, len, val, len + 1) <= 0) {
			memcpy(val, value, len);
			val[len] = 0;
		}
		changed += props_set_owned(p, strdup(key), val) > 0;
	}
	return changed;
}

SHIM_EXPORT int pw_properties_update_string(struct pw_properties *p, const char *str, size_t size)
{
	return pw_properties_update_string_checked(p, str, size, NULL);
}

SHIM_EXPORT struct pw_properties *pw_properties_new_string_checked(const char *args,
		size_t size, struct spa_error_location *loc)
{
	struct pw_properties *p = pw_properties_new(NULL, NULL);
	if (p && pw_properties_update_string_checked(p, args, size, loc) < 0) {
		pw_properties_free(p);
		return NULL;
	}
	return p;
}

SHIM_EXPORT struct pw_properties *pw_properties_new_string(const char *args)
{
	return pw_properties_new_string_checked(args, strlen(args), NULL);
}

SHIM_EXPORT struct pw_properties *pw_properties_new_dict(const struct spa_dict *dict)
{
	struct pw_properties *p = pw_properties_new(NULL, NULL);
	if (p && dict)
		pw_properties_update(p, dict);
	return p;
}

SHIM_EXPORT struct pw_properties *pw_properties_copy(const struct pw_properties *props)
{
	return pw_properties_new_dict(props ? &props->dict : NULL);
}

SHIM_EXPORT void pw_properties_clear(struct pw_properties *p)
{
	struct spa_dict_item *items = (struct spa_dict_item *)p->dict.items;
	for (uint32_t i = 0; i < p->dict.n_items; i++) {
		free((char *)items[i].key);
		free((char *)items[i].value);
	}
	p->dict.n_items = 0;
}

SHIM_EXPORT void pw_properties_free(struct pw_properties *p)
{
	struct props_impl *impl;
	if (p == NULL)
		return;
	impl = SPA_CONTAINER_OF(p, struct props_impl, this);
	pw_properties_clear(p);
	free((void *)p->dict.items);
	free(impl);
}

SHIM_EXPORT const char *pw_properties_iterate(const struct pw_properties *p, void **state)
{
	uintptr_t i = (uintptr_t)*state;
	if (i >= p->dict.n_items)
		return NULL;
	*state = (void *)(i + 1);
	return p->dict.items[i].key;
}

SHIM_EXPORT int pw_properties_fetch_uint32(const struct pw_properties *p, const char *key, uint32_t *v)
{
	const char *s = pw_properties_get(p, key);
	return s && spa_atou32(s, v, 0) ? 0 : -EINVAL;
}

SHIM_EXPORT int pw_properties_fetch_int32(const struct pw_properties *p, const char *key, int32_t *v)
{
	const char *s = pw_properties_get(p, key);
	return s && spa_atoi32(s, v, 0) ? 0 : -EINVAL;
}

SHIM_EXPORT int pw_properties_fetch_uint64(const struct pw_properties *p, const char *key, uint64_t *v)
{
	const char *s = pw_properties_get(p, key);
	return s && spa_atou64(s, v, 0) ? 0 : -EINVAL;
}

SHIM_EXPORT int pw_properties_fetch_int64(const struct pw_properties *p, const char *key, int64_t *v)
{
	const char *s = pw_properties_get(p, key);
	return s && spa_atoi64(s, v, 0) ? 0 : -EINVAL;
}

SHIM_EXPORT int pw_properties_fetch_bool(const struct pw_properties *p, const char *key, bool *v)
{
	const char *s = pw_properties_get(p, key);
	if (s == NULL)
		return -EINVAL;
	*v = spa_atob(s);
	return 0;
}

SHIM_EXPORT int pw_properties_serialize_dict(FILE *f, const struct spa_dict *dict, uint32_t flags)
{
	const struct spa_dict_item *it;
	int count = 0;
	(void)flags;
	fputc('{', f);
	spa_dict_for_each(it, dict) {
		fprintf(f, "%s \"%s\": \"%s\"", count++ ? "," : "", it->key, it->value ? it->value : "");
	}
	fputs(" }", f);
	return count;
}
