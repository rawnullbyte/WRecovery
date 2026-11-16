/*
 * winreg - minimal read-only parser for Windows registry hive files (regf)
 *
 * Reads SYSTEM / SOFTWARE / NTUSER.DAT style hives straight off a mounted
 * (or raw) volume. No Windows, no libs, no dependencies beyond libc.
 *
 * Build:  cc -O2 -static -o winreg winreg.c
 *
 * Usage:
 *   winreg keys    <hive> [path]                 list subkey names
 *   winreg values  <hive> [path]                 list values as TSV (name<TAB>type<TAB>value)
 *   winreg get     <hive> <path> <value> [--raw] print a single value
 *   winreg ls      <hive> [path]                 subkeys + values, human readable
 *   winreg dump    <hive> [path] [maxdepth]      regedit-style recursive dump
 *   winreg exists  <hive> [path]                 exit 0 if the key exists
 *
 * Paths are case-insensitive, relative to the hive root, using \ or /.
 * Values are escaped in TSV mode: \\ \t \r \n
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- raw hive */

static const uint8_t *H;      /* mapped hive */
static size_t HLEN;           /* mapped length */
static uint32_t ROOT_CELL;    /* root cell index (relative to hive bins data) */

#define BASE_BLOCK_SIZE 0x1000
#define CELL_HDR 4
#define BIG_DATA_SEG 16344   /* max bytes per db segment */
#define INLINE_DATA 0x80000000u

static uint16_t rd16(size_t o) { return (uint16_t)(H[o] | (H[o + 1] << 8)); }
static uint32_t rd32(size_t o)
{
	return (uint32_t)H[o] | ((uint32_t)H[o + 1] << 8) |
	       ((uint32_t)H[o + 2] << 16) | ((uint32_t)H[o + 3] << 24);
}

static int in_bounds(size_t off, size_t len)
{
	return off <= HLEN && len <= HLEN - off;
}

/* Return the data offset of the cell at absolute file offset @off, or 0. */
static size_t cell_at(size_t off)
{
	int32_t sz;
	uint32_t asz;

	if (!in_bounds(off, CELL_HDR))
		return 0;
	sz = (int32_t)rd32(off);
	if (sz == 0)
		return 0;
	asz = sz < 0 ? (uint32_t)(-sz) : (uint32_t)sz;
	if (asz < CELL_HDR || !in_bounds(off, asz))
		return 0;
	return off + CELL_HDR;
}

/* Cell index (as stored inside the hive) -> data offset. */
static size_t cell_idx(uint32_t idx)
{
	size_t off = (size_t)BASE_BLOCK_SIZE + idx;

	if (idx == 0xffffffffu || off < (size_t)BASE_BLOCK_SIZE)
		return 0;
	return cell_at(off);
}

static int has_sig(size_t off, const char *sig)
{
	return off && in_bounds(off, 2) && H[off] == (uint8_t)sig[0] &&
	       H[off + 1] == (uint8_t)sig[1];
}

/* ------------------------------------------------------------ name decoding */

static size_t emit_utf8(uint32_t c, char *out)
{
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (char)(0xc0 | (c >> 6));
		out[1] = (char)(0x80 | (c & 0x3f));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (char)(0xe0 | (c >> 12));
		out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
		out[2] = (char)(0x80 | (c & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (c >> 18));
	out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
	out[3] = (char)(0x80 | (c & 0x3f));
	return 4;
}

static size_t utf16_to_utf8(const uint8_t *p, size_t n, char *out, size_t outsz)
{
	size_t i, o = 0;

	for (i = 0; i + 1 < n; i += 2) {
		uint32_t c = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8);

		if (c >= 0xd800 && c <= 0xdbff && i + 3 < n) {
			uint32_t lo = (uint32_t)p[i + 2] | ((uint32_t)p[i + 3] << 8);
			if (lo >= 0xdc00 && lo <= 0xdfff) {
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
		}
		if (o + 4 >= outsz)
			break;
		o += emit_utf8(c, out + o);
	}
	out[o] = 0;
	return o;
}

/* comp == 1 -> single byte (latin-1 / "compressed" name), else UTF-16LE */
static void decode_name(const uint8_t *p, size_t nbytes, int comp, char *out,
			size_t outsz)
{
	size_t i, o = 0;

	if (outsz == 0)
		return;
	if (!comp) {
		utf16_to_utf8(p, nbytes, out, outsz);
		return;
	}
	for (i = 0; i < nbytes && o + 1 < outsz; i++) {
		uint8_t c = p[i];
		if (c < 0x80) {
			out[o++] = (char)c;
		} else {
			if (o + 2 >= outsz)
				break;
			out[o++] = (char)(0xc0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[o] = 0;
}

/* ------------------------------------------------------------------- keys */

#define NK_NAME_OFF 0x4c

typedef struct {
	size_t off;            /* file offset of the "nk" record */
	uint32_t flags;
	uint32_t subkeys;
	uint32_t subkey_list;
	uint32_t values;
	uint32_t value_list;
	uint16_t name_len;
} NK;

static int nk_load(size_t off, NK *k)
{
	if (!has_sig(off, "nk") || !in_bounds(off, NK_NAME_OFF + 2))
		return -1;
	memset(k, 0, sizeof(*k));
	k->off = off;
	k->flags = rd16(off + 2);
	k->subkeys = rd32(off + 0x14);
	k->subkey_list = rd32(off + 0x1c);
	k->values = rd32(off + 0x24);
	k->value_list = rd32(off + 0x28);
	k->name_len = rd16(off + 0x48);
	return 0;
}

static void nk_name(const NK *k, char *out, size_t outsz)
{
	size_t n = k->name_len;

	if (!in_bounds(k->off + NK_NAME_OFF, n))
		n = 0;
	decode_name(H + k->off + NK_NAME_OFF, n, (k->flags & 0x20) != 0, out,
		    outsz);
}

typedef int (*nk_cb)(size_t nk_off, void *ud);

/* lf/lh/li/ri list cell -> callback for every referenced nk cell */
static int walk_list(size_t list, int depth, nk_cb cb, void *ud)
{
	uint16_t n, i;
	int rc = 0;

	if (!list || depth > 8 || !in_bounds(list, 4))
		return -1;

	n = rd16(list + 2);
	if (has_sig(list, "lf") || has_sig(list, "lh")) {
		for (i = 0; i < n; i++) {
			size_t e = list + 4 + (size_t)i * 8;
			size_t nk;

			if (!in_bounds(e, 8))
				break;
			nk = cell_idx(rd32(e));
			if (!nk)
				continue;
			if ((rc = cb(nk, ud)) != 0)
				return rc;
		}
		return 0;
	}
	if (has_sig(list, "li")) {
		for (i = 0; i < n; i++) {
			size_t e = list + 4 + (size_t)i * 4;
			size_t nk;

			if (!in_bounds(e, 4))
				break;
			nk = cell_idx(rd32(e));
			if (!nk)
				continue;
			if ((rc = cb(nk, ud)) != 0)
				return rc;
		}
		return 0;
	}
	if (has_sig(list, "ri")) {
		for (i = 0; i < n; i++) {
			size_t e = list + 4 + (size_t)i * 4;
			size_t sub;

			if (!in_bounds(e, 4))
				break;
			sub = cell_idx(rd32(e));
			if (!sub)
				continue;
			if ((rc = walk_list(sub, depth + 1, cb, ud)) != 0)
				return rc;
		}
		return 0;
	}
	return -1;
}

static int walk_subkeys(const NK *k, nk_cb cb, void *ud)
{
	if (k->subkeys == 0)
		return 0;
	return walk_list(cell_idx(k->subkey_list), 0, cb, ud);
}

/* --------------------------------------------------------------- values */

static const char *type_name(uint32_t t)
{
	switch (t) {
	case 0: return "REG_NONE";
	case 1: return "REG_SZ";
	case 2: return "REG_EXPAND_SZ";
	case 3: return "REG_BINARY";
	case 4: return "REG_DWORD";
	case 5: return "REG_DWORD_BIG_ENDIAN";
	case 6: return "REG_LINK";
	case 7: return "REG_MULTI_SZ";
	case 8: return "REG_RESOURCE_LIST";
	case 9: return "REG_FULL_RESOURCE_DESCRIPTOR";
	case 10: return "REG_RESOURCE_REQUIREMENTS_LIST";
	case 11: return "REG_QWORD";
	default: return "REG_UNKNOWN";
	}
}

typedef struct {
	char name[1024];
	uint32_t type;
	uint8_t *data;
	size_t len;
} Value;

static void value_free(Value *v)
{
	free(v->data);
	v->data = NULL;
	v->len = 0;
}

/* Load value at nk/ vk cell offset. Returns 0 on success. */
static int value_load(size_t vk, Value *v)
{
	uint32_t dsize, doff, type;
	uint16_t flags, nlen;
	size_t data;
	int comp;

	if (!has_sig(vk, "vk") || !in_bounds(vk, 0x14))
		return -1;
	nlen = rd16(vk + 2);
	dsize = rd32(vk + 4);
	doff = rd32(vk + 8);
	type = rd32(vk + 0xc);
	flags = rd16(vk + 0x10);
	comp = (flags & 0x0001) != 0;

	memset(v, 0, sizeof(*v));
	/*
	 * Real-world hives contain value types with high bits set (0x20004,
	 * 0x40007, ...). Windows and other parsers mask them, so the low 16
	 * bits are what the value actually means.
	 */
	v->type = type & 0xFFFFu;

	if (!in_bounds(vk + 0x14, nlen))
		return -1;
	decode_name(H + vk + 0x14, nlen, comp, v->name, sizeof(v->name));

	if (dsize & INLINE_DATA) {
		size_t n = dsize & ~INLINE_DATA;

		if (n > 4)
			n = 4;
		if (!in_bounds(vk + 8, n))
			return -1;
		v->data = malloc(n ? n : 1);
		if (!v->data)
			return -1;
		memcpy(v->data, H + vk + 8, n);
		v->len = n;
		return 0;
	}

	if (dsize == 0) {
		v->data = malloc(1);
		v->len = 0;
		return v->data ? 0 : -1;
	}

	data = cell_idx(doff);
	if (!data)
		return -1;

	if (dsize > BIG_DATA_SEG && has_sig(data, "db")) {
		/* segmented big data: db -> list of cells, 16344 bytes each */
		size_t list = cell_idx(rd32(data + 4));
		uint16_t segs = rd16(data + 2);
		uint8_t *buf = malloc(dsize ? dsize : 1);
		size_t got = 0;
		uint16_t i;

		if (!buf || !list)
			goto fail;
		for (i = 0; i < segs && got < dsize; i++) {
			size_t e = list + (size_t)i * 4;
			size_t seg;
			size_t want = dsize - got;

			if (!in_bounds(e, 4))
				break;
			seg = cell_idx(rd32(e));
			if (!seg)
				break;
			if (want > BIG_DATA_SEG)
				want = BIG_DATA_SEG;
			if (!in_bounds(seg, want))
				break;
			memcpy(buf + got, H + seg, want);
			got += want;
		}
		v->data = buf;
		v->len = got;
		return 0;
fail:
		free(buf);
		return -1;
	}

	if (!in_bounds(data, dsize))
		return -1;
	v->data = malloc(dsize);
	if (!v->data)
		return -1;
	memcpy(v->data, H + data, dsize);
	v->len = dsize;
	return 0;
}

typedef int (*val_cb)(const Value *v, void *ud);

static int walk_values(const NK *k, val_cb cb, void *ud)
{
	uint32_t i;
	size_t list = cell_idx(k->value_list);
	int rc;

	if (k->values == 0 || !list)
		return 0;
	for (i = 0; i < k->values; i++) {
		size_t e = list + (size_t)i * 4;
		size_t vk;
		Value v;

		if (!in_bounds(e, 4))
			break;
		vk = cell_idx(rd32(e));
		if (!vk)
			continue;
		if (value_load(vk, &v) != 0)
			continue;
		rc = cb(&v, ud);
		value_free(&v);
		if (rc)
			return rc;
	}
	return 0;
}

/* ------------------------------------------------------------ path lookup */

static int find_subkey(const NK *k, const char *name, NK *out)
{
	struct ctx {
		const char *name;
		NK *out;
		int found;
	} c;
	int cb(size_t off, void *ud)
	{
		struct ctx *x = ud;
		NK cand;
		char n[1024];

		if (nk_load(off, &cand) != 0)
			return 0;
		nk_name(&cand, n, sizeof(n));
		if (strcasecmp(n, x->name) == 0) {
			*x->out = cand;
			x->found = 1;
			return 1;
		}
		return 0;
	}

	c.name = name;
	c.out = out;
	c.found = 0;
	walk_subkeys(k, cb, &c);
	return c.found ? 0 : -1;
}

/*
 * Split @path on @sep and walk down the tree. Registry key names may contain
 * '/' (e.g. "Standard TCP/IP Port"), so the separator is chosen by the caller
 * and only used when it actually resolves.
 */
static int resolve_sep(const char *path, char sep, NK *out)
{
	NK cur;
	char buf[4096];
	char *tok;

	if (nk_load(cell_idx(ROOT_CELL), &cur) != 0)
		return -1;
	if (!path || !*path) {
		*out = cur;
		return 0;
	}
	if (strlen(path) >= sizeof(buf))
		return -1;
	strcpy(buf, path);
	tok = buf;
	while (*tok == sep)
		tok++;
	/* accept "SAM\Domains\..." as well as "Domains\..." */
	{
		char rootname[1024];

		nk_name(&cur, rootname, sizeof(rootname));
		if (*rootname) {
			char *next = strchr(tok, sep);
			size_t n = next ? (size_t)(next - tok) : strlen(tok);

			if (n == strlen(rootname) &&
			    strncasecmp(tok, rootname, n) == 0)
				tok = next ? next + 1 : tok + n;
		}
	}
	while (*tok) {
		char *next = strchr(tok, sep);

		if (next)
			*next = 0;
		if (*tok && find_subkey(&cur, tok, &cur) != 0)
			return -1;
		if (!next)
			break;
		tok = next + 1;
		while (*tok == sep)
			tok++;
	}
	*out = cur;
	return 0;
}

static int resolve(const char *path, NK *out)
{
	if (resolve_sep(path, '\\', out) == 0)
		return 0;
	if (path && strchr(path, '/'))
		return resolve_sep(path, '/', out);
	return -1;
}

/* --------------------------------------------------------------- printing */

/*
 * Registry string values (REG_SZ, REG_EXPAND_SZ, REG_LINK, REG_MULTI_SZ) are
 * stored as UTF-16LE. A fair number of real-world hives contain ANSI data in
 * those slots, so sniff the encoding instead of trusting it blindly.
 */
static int looks_utf16(const uint8_t *p, size_t n)
{
	size_t i, odd_nuls = 0;

	if (n < 2 || (n & 1))
		return 0;
	if (p[n - 1] == 0 && p[n - 2] == 0)
		return 1;
	for (i = 1; i < n && i < 64; i += 2)
		if (p[i] == 0)
			odd_nuls++;
	return odd_nuls > 0;
}

/* Returns a malloc'd UTF-8 string (NUL terminated, NULs inside for MULTI_SZ). */
static char *utf16le_to_utf8(const uint8_t *p, size_t n, size_t *outlen)
{
	char *out = malloc(n * 3 / 2 + 8);
	size_t i, o = 0;

	if (!out)
		return NULL;
	for (i = 0; i + 1 < n; i += 2) {
		uint32_t c = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8);

		if (c >= 0xd800 && c <= 0xdbff && i + 3 < n) {
			uint32_t lo = (uint32_t)p[i + 2] | ((uint32_t)p[i + 3] << 8);
			if (lo >= 0xdc00 && lo <= 0xdfff) {
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
		}
		if (c == 0)
			out[o++] = 0;
		else
			o += emit_utf8(c, out + o);
	}
	out[o] = 0;
	*outlen = o;
	return out;
}

static char *value_text(const Value *v, size_t *len)
{
	char *s;

	if (!looks_utf16(v->data, v->len)) {
		s = malloc(v->len + 1);
		if (!s)
			return NULL;
		memcpy(s, v->data, v->len);
		s[v->len] = 0;
		*len = v->len;
	} else {
		s = utf16le_to_utf8(v->data, v->len, len);
		if (!s)
			return NULL;
	}
	while (*len && s[*len - 1] == 0)
		(*len)--;
	if (v->type != 7) {
		/* REG_SZ / REG_EXPAND_SZ / REG_LINK: C string, ends at first NUL */
		size_t i;

		for (i = 0; i < *len; i++) {
			if (s[i] == 0) {
				*len = i;
				break;
			}
		}
	}
	return s;
}

static int is_string_type(uint32_t t)
{
	return t == 1 || t == 2 || t == 6 || t == 7;
}

static void print_escaped(const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		uint8_t c = p[i];

		switch (c) {
		case '\\': fputs("\\\\", stdout); break;
		case '\t': fputs("\\t", stdout); break;
		case '\r': fputs("\\r", stdout); break;
		case '\n': fputs("\\n", stdout); break;
		default:
			if (c < 0x20 || c == 0x7f)
				printf("\\x%02x", c);
			else
				putchar(c);
		}
	}
}

static void print_multisz(const char *s, size_t len)
{
	size_t i = 0;
	int first = 1;

	while (i < len) {
		size_t start = i, n;

		while (i < len && s[i])
			i++;
		n = i - start;
		if (!first)
			putchar('|');
		print_escaped((const uint8_t *)s + start, n);
		first = 0;
		if (i < len)
			i++;
	}
}

static uint32_t be32(uint32_t v)
{
	return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
	       ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}

/* Human readable one-line rendering of a value (no trailing newline). */
static void print_value_human(const Value *v)
{
	if (is_string_type(v->type)) {
		size_t len = 0;
		char *s = value_text(v, &len);

		if (!s)
			return;
		if (v->type == 7)
			print_multisz(s, len);
		else
			print_escaped((const uint8_t *)s, len);
		free(s);
		return;
	}

	switch (v->type) {
	case 4:
		if (v->len >= 4)
			printf("%u", (unsigned)(
				(uint32_t)v->data[0] | ((uint32_t)v->data[1] << 8) |
				((uint32_t)v->data[2] << 16) |
				((uint32_t)v->data[3] << 24)));
		break;
	case 5:
		if (v->len >= 4)
			printf("%u", be32((uint32_t)v->data[0] |
					  ((uint32_t)v->data[1] << 8) |
					  ((uint32_t)v->data[2] << 16) |
					  ((uint32_t)v->data[3] << 24)));
		break;
	case 11:
		if (v->len >= 8)
			printf("%llu", (unsigned long long)(
				(uint64_t)v->data[0] | ((uint64_t)v->data[1] << 8) |
				((uint64_t)v->data[2] << 16) |
				((uint64_t)v->data[3] << 24) |
				((uint64_t)v->data[4] << 32) |
				((uint64_t)v->data[5] << 40) |
				((uint64_t)v->data[6] << 48) |
				((uint64_t)v->data[7] << 56)));
		break;
	default:
		if (v->len == 0) {
			fputs("(empty)", stdout);
			break;
		}
		if (v->len <= 8) {
			size_t i;

			for (i = 0; i < v->len; i++)
				printf("%s%02x", i ? " " : "", v->data[i]);
			break;
		}
		printf("<%zu bytes>", v->len);
		break;
	}
}

static void print_value_tsv(const Value *v)
{
	printf("%s\t%s\t", v->name, type_name(v->type));
	print_value_human(v);
	putchar('\n');
}

static int cb_tsv(const Value *v, void *ud)
{
	(void)ud;
	print_value_tsv(v);
	return 0;
}



struct name_only {
	int first;
};

static int cb_print_key(size_t off, void *ud)
{
	struct name_only *x = ud;
	NK k;
	char n[1024];

	if (nk_load(off, &k) != 0)
		return 0;
	nk_name(&k, n, sizeof(n));
	printf("%s%s", x->first ? "" : "\n", n);
	x->first = 0;
	return 0;
}

static int cb_ls(size_t off, void *ud)
{
	NK k;
	char n[1024];

	(void)ud;
	if (nk_load(off, &k) != 0)
		return 0;
	nk_name(&k, n, sizeof(n));
	printf("  %s\\\n", n);
	return 0;
}

/* ------------------------------------------------------------- dump (reg) */

static void dump_hex(FILE *f, const uint8_t *p, size_t n)
{
	size_t i;

	fputs("hex:", f);
	for (i = 0; i < n; i++) {
		if (i && i % 25 == 0)
			fputs(" \\\n  ", f);
		fprintf(f, "%02x", p[i]);
		if (i + 1 < n)
			fputc(',', f);
	}
}

static int dump_value(const Value *v, void *ud)
{
	FILE *f = ud;
	size_t len = 0;
	char *s;

	fprintf(f, "\"%s\"=", v->name);

	if (is_string_type(v->type)) {
		if (!(s = value_text(v, &len)))
			return 0;
		fputc('"', f);
		if (v->type == 7) {
			size_t i;

			for (i = 0; i < len; i++) {
				if (s[i] == 0) {
					fputs("\\0", f);
					continue;
				}
				if (s[i] == '"' || s[i] == '\\')
					fputc('\\', f);
				fputc(s[i], f);
			}
		} else {
			size_t i;
			for (i = 0; i < len; i++) {
				if (s[i] == '"' || s[i] == '\\')
					fputc('\\', f);
				fputc(s[i], f);
			}
		}
		fputs("\"\n", f);
		free(s);
		return 0;
	}

	switch (v->type) {
	case 4:
		if (v->len >= 4)
			fprintf(f, "dword:%08x\n", (unsigned)(
				(uint32_t)v->data[0] | ((uint32_t)v->data[1] << 8) |
				((uint32_t)v->data[2] << 16) |
				((uint32_t)v->data[3] << 24)));
		else
			fputs("\"\"\n", f);
		break;
	default:
		dump_hex(f, v->data, v->len);
		fputc('\n', f);
		break;
	}
	return 0;
}

struct dump_item {
	char name[1024];
	size_t off;
};

struct dump_ctx {
	struct dump_item *items;
	size_t n, cap;
};

static int cb_collect(size_t off, void *ud)
{
	struct dump_ctx *c = ud;
	NK k;

	if (nk_load(off, &k) != 0)
		return 0;
	if (c->n == c->cap) {
		size_t cap = c->cap ? c->cap * 2 : 16;
		struct dump_item *p = realloc(c->items, cap * sizeof(*p));

		if (!p)
			return 1;
		c->items = p;
		c->cap = cap;
	}
	nk_name(&k, c->items[c->n].name, sizeof(c->items[c->n].name));
	c->items[c->n].off = off;
	c->n++;
	return 0;
}

/* @path is the full path of @k (used for the [HKEY...] header line) */
static void dump_key(FILE *f, const NK *k, const char *path, int depth,
		     int maxdepth)
{
	struct dump_ctx c;
	size_t i;

	fprintf(f, "\n[%s]\n", path);
	walk_values(k, dump_value, f);

	if (maxdepth >= 0 && depth >= maxdepth)
		return;

	memset(&c, 0, sizeof(c));
	walk_subkeys(k, cb_collect, &c);
	for (i = 0; i < c.n; i++) {
		NK child;
		char cpath[8192];

		if (nk_load(c.items[i].off, &child) != 0)
			continue;
		snprintf(cpath, sizeof(cpath), "%s\\%s", path, c.items[i].name);
		dump_key(f, &child, cpath, depth + 1, maxdepth);
	}
	free(c.items);
}

/* ------------------------------------------------------------- get command */

struct get_ctx {
	const char *want;
	int found;
};

static int cb_get(const Value *v, void *ud)
{
	struct get_ctx *g = ud;
	size_t len = 0;
	char *s;

	if (strcasecmp(v->name, g->want) != 0)
		return 0;
	g->found = 1;

	if (is_string_type(v->type)) {
		s = value_text(v, &len);
		if (!s)
			return 1;
		if (v->type == 7) {
			size_t i;

			for (i = 0; i < len; i++)
				putchar(s[i] ? s[i] : '\n');
		} else {
			fwrite(s, 1, len, stdout);
		}
		free(s);
		return 1;
	}

	switch (v->type) {
	case 4:
		if (v->len >= 4)
			printf("%u", (unsigned)(
				(uint32_t)v->data[0] | ((uint32_t)v->data[1] << 8) |
				((uint32_t)v->data[2] << 16) |
				((uint32_t)v->data[3] << 24)));
		break;
	case 5:
		if (v->len >= 4)
			printf("%u", be32((uint32_t)v->data[0] |
					  ((uint32_t)v->data[1] << 8) |
					  ((uint32_t)v->data[2] << 16) |
					  ((uint32_t)v->data[3] << 24)));
		break;
	case 11:
		if (v->len >= 8)
			printf("%llu", (unsigned long long)(
				(uint64_t)v->data[0] | ((uint64_t)v->data[1] << 8) |
				((uint64_t)v->data[2] << 16) |
				((uint64_t)v->data[3] << 24) |
				((uint64_t)v->data[4] << 32) |
				((uint64_t)v->data[5] << 40) |
				((uint64_t)v->data[6] << 48) |
				((uint64_t)v->data[7] << 56)));
		break;
	default: {
		size_t i;

		for (i = 0; i < v->len; i++)
			printf("%02x", v->data[i]);
		break;
	}
	}
	return 1;
}

/* ---------------------------------------------------- SHA-512 and $6$ crypt */

/* ---- SHA-512 (FIPS 180-4) ---- */
typedef struct {
	uint64_t h[8];
	uint64_t len_lo, len_hi;
	uint8_t buf[128];
	size_t buflen;
} sha512_ctx;

static const uint64_t sha512_k[80] = {
	0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
	0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
	0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
	0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(sha512_ctx *c, const uint8_t *p)
{
	uint64_t w[80], a, b, cc, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint64_t)p[i * 8] << 56) | ((uint64_t)p[i * 8 + 1] << 48) |
		       ((uint64_t)p[i * 8 + 2] << 40) | ((uint64_t)p[i * 8 + 3] << 32) |
		       ((uint64_t)p[i * 8 + 4] << 24) | ((uint64_t)p[i * 8 + 5] << 16) |
		       ((uint64_t)p[i * 8 + 6] << 8) | (uint64_t)p[i * 8 + 7];
	}
	for (i = 16; i < 80; i++) {
		uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
		uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (i = 0; i < 80; i++) {
		uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
		uint64_t ch = (e & f) ^ (~e & g);
		uint64_t t1 = h + S1 + ch + sha512_k[i] + w[i];
		uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
		uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		uint64_t t2 = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha512_init(sha512_ctx *c)
{
	c->h[0] = 0x6a09e667f3bcc908ULL; c->h[1] = 0xbb67ae8584caa73bULL;
	c->h[2] = 0x3c6ef372fe94f82bULL; c->h[3] = 0xa54ff53a5f1d36f1ULL;
	c->h[4] = 0x510e527fade682d1ULL; c->h[5] = 0x9b05688c2b3e6c1fULL;
	c->h[6] = 0x1f83d9abfb41bd6bULL; c->h[7] = 0x5be0cd19137e2179ULL;
	c->len_lo = c->len_hi = 0;
	c->buflen = 0;
}

static void sha512_update(sha512_ctx *c, const void *data, size_t len)
{
	const uint8_t *p = data;

	c->len_lo += (uint64_t)len << 3;
	if (c->len_lo < ((uint64_t)len << 3))
		c->len_hi++;
	c->len_hi += (uint64_t)len >> 61;
	while (len) {
		size_t n = 128 - c->buflen;

		if (n > len)
			n = len;
		memcpy(c->buf + c->buflen, p, n);
		c->buflen += n;
		p += n;
		len -= n;
		if (c->buflen == 128) {
			sha512_block(c, c->buf);
			c->buflen = 0;
		}
	}
}

static void sha512_final(sha512_ctx *c, uint8_t out[64])
{
	uint64_t lo = c->len_lo, hi = c->len_hi;   /* message length in bits */
	size_t i;

	if (c->buflen == 128) {
		sha512_block(c, c->buf);
		c->buflen = 0;
	}
	c->buf[c->buflen++] = 0x80;
	if (c->buflen > 112) {
		memset(c->buf + c->buflen, 0, 128 - c->buflen);
		sha512_block(c, c->buf);
		c->buflen = 0;
	}
	memset(c->buf + c->buflen, 0, 112 - c->buflen);
	for (i = 0; i < 8; i++) {
		c->buf[112 + i] = (uint8_t)(hi >> (56 - i * 8));
		c->buf[120 + i] = (uint8_t)(lo >> (56 - i * 8));
	}
	sha512_block(c, c->buf);
	for (i = 0; i < 8; i++) {
		uint64_t v = c->h[i];
		int j;

		for (j = 0; j < 8; j++)
			out[i * 8 + j] = (uint8_t)(v >> (56 - j * 8));
	}
}

/* ---- sha512-crypt ($6$) ---- */
static void sha512crypt_perm(const uint8_t dig[64], char *out)
{
	/* byte order of the transposed digest, 3 bytes per 4 output chars */
	static const uint8_t order[21][3] = {
		{42, 21, 0}, {1, 43, 22}, {23, 2, 44}, {45, 24, 3}, {4, 46, 25},
		{26, 5, 47}, {48, 27, 6}, {7, 49, 28}, {29, 8, 50}, {51, 30, 9},
		{10, 52, 31}, {32, 11, 53}, {54, 33, 12}, {13, 55, 34},
		{35, 14, 56}, {57, 36, 15}, {16, 58, 37}, {38, 17, 59},
		{60, 39, 18}, {19, 61, 40}, {41, 20, 62}
	};
	static const char *b64 =
		"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	int i;

	/* crypt's own base64: little endian, six bits at a time */
	for (i = 0; i < 21; i++) {
		uint8_t v1 = dig[order[i][0]];
		uint8_t v2 = dig[order[i][1]];
		uint8_t v3 = dig[order[i][2]];

		*out++ = b64[v1 & 0x3f];
		*out++ = b64[((v2 & 0x0f) << 2) | (v1 >> 6)];
		*out++ = b64[((v3 & 0x03) << 4) | (v2 >> 4)];
		*out++ = b64[v3 >> 2];
	}
	*out++ = b64[dig[63] & 0x3f];
	*out++ = b64[dig[63] >> 6];
	*out = 0;
}

/* out must hold at least 128 bytes; salt is up to 16 chars */
static void sha512crypt(const char *pw, const char *salt, char *out)
{
	sha512_ctx a, b;
	uint8_t dig[64], dp[64], ds[64];
	uint8_t pseq[256], sseq[256];
	size_t plen = strlen(pw), slen = strlen(salt), i;
	uint32_t cnt;

	if (plen > 255)
		plen = 255;
	if (slen > 16)
		slen = 16;

	sha512_init(&b);
	sha512_update(&b, pw, plen);
	sha512_update(&b, salt, slen);
	sha512_update(&b, pw, plen);
	sha512_final(&b, dig);

	sha512_init(&a);
	sha512_update(&a, pw, plen);
	sha512_update(&a, salt, slen);
	/* plen bytes of digest B, repeating B as often as needed */
	{
		size_t left = plen;

		while (left) {
			size_t n = left < 64 ? left : 64;

			sha512_update(&a, dig, n);
			left -= n;
		}
	}
	for (cnt = (uint32_t)plen; cnt; cnt >>= 1) {
		if (cnt & 1)
			sha512_update(&a, dig, 64);
		else
			sha512_update(&a, pw, plen);
	}
	sha512_final(&a, dig);

	/* P */
	sha512_init(&b);
	for (i = 0; i < plen; i++)
		sha512_update(&b, pw, plen);
	sha512_final(&b, dp);
	for (i = 0; i < plen; i++)
		pseq[i] = dp[i % 64];

	/* S */
	sha512_init(&b);
	for (i = 0; i < (size_t)(16 + dig[0]); i++)
		sha512_update(&b, salt, slen);
	sha512_final(&b, ds);
	for (i = 0; i < slen; i++)
		sseq[i] = ds[i % 64];

	for (i = 0; i < 5000; i++) {
		sha512_init(&b);
		if (i & 1)
			sha512_update(&b, pseq, plen);
		else
			sha512_update(&b, dig, 64);
		if (i % 3)
			sha512_update(&b, sseq, slen);
		if (i % 7)
			sha512_update(&b, pseq, plen);
		if (i & 1)
			sha512_update(&b, dig, 64);
		else
			sha512_update(&b, pseq, plen);
		sha512_final(&b, dig);
	}

	{
		char perm[87];

		sha512crypt_perm(dig, perm);
		snprintf(out, 128, "$6$%.16s$%.86s", salt, perm);
	}
}

/* --------------------------------------------------------------- SAM hives */

/*
 * Layout constants for the SAM hive, matching the long-standing
 * chntpw/ntpasswd reference implementation:
 *
 *   Users\<RID>\F   user account record, 0x50 bytes
 *                    0x38 u16 ACB flags, 0x40 u16 failed logins
 *   Users\<RID>\V   user record, >= 0xCC bytes
 *                    0x0C/0x10 username offset/length (relative to 0xCC)
 *                    0xA0      LM hash length
 *                    0xAC      NT hash length
 *
 * Clearing both hash lengths makes Windows treat the account as having a
 * blank password. That is exactly what ntpasswd's "clear password" does, and
 * it needs no boot key, no RC4 and no SAM crypto.
 */

#define SAM_ACB_DISABLED 0x0001
#define SAM_ACB_PWNOEXP  0x0200
#define SAM_ACB_AUTOLOCK 0x0400
#define SAM_USERS_PATH   "Domains\\Account\\Users"

typedef struct {
	uint32_t rid;
	char name[256];
	char fullname[256];
	uint16_t acb;
	uint16_t failed;
	int nt_len;
	int lm_len;
	int admin;
} SamUser;

static int get_value_named(const char *path, const char *want, Value *out)
{
	struct ctx {
		const char *want;
		Value *out;
		int found;
	} c;
	int cb(const Value *v, void *ud)
	{
		struct ctx *x = ud;

		if (strcasecmp(v->name, x->want) != 0)
			return 0;
		memset(x->out, 0, sizeof(*x->out));
		snprintf(x->out->name, sizeof(x->out->name), "%s", v->name);
		x->out->type = v->type;
		x->out->len = v->len;
		x->out->data = malloc(v->len ? v->len : 1);
		if (!x->out->data)
			return 1;
		memcpy(x->out->data, v->data, v->len);
		x->found = 1;
		return 1;
	}
	NK k;

	c.want = want;
	c.out = out;
	c.found = 0;
	if (resolve(path, &k) != 0)
		return -1;
	walk_values(&k, cb, &c);
	return c.found ? 0 : -1;
}

/* File offset and length of a value's data (only for plain cell data). */
static int value_span(const char *path, const char *want, size_t *off,
		      size_t *len)
{
	NK k;
	uint32_t i;
	size_t list;
	int found = 0;

	if (resolve(path, &k) != 0)
		return -1;
	list = cell_idx(k.value_list);
	if (!list)
		return -1;
	for (i = 0; i < k.values && !found; i++) {
		size_t e = list + (size_t)i * 4;
		size_t vk;
		uint16_t nlen, flags;
		char name[1024];
		uint32_t dsize, doff;

		if (!in_bounds(e, 4))
			break;
		vk = cell_idx(rd32(e));
		if (!vk || !has_sig(vk, "vk"))
			continue;
		nlen = rd16(vk + 2);
		flags = rd16(vk + 0x10);
		if (!in_bounds(vk + 0x14, nlen))
			continue;
		decode_name(H + vk + 0x14, nlen, flags & 1, name, sizeof(name));
		if (strcasecmp(name, want) != 0)
			continue;
		dsize = rd32(vk + 4);
		doff = rd32(vk + 8);
		if (dsize & INLINE_DATA || dsize > BIG_DATA_SEG)
			return -1;              /* not patchable in place */
		*off = cell_idx(doff);
		*len = dsize;
		if (!*off)
			return -1;
		found = 1;
	}
	return found ? 0 : -1;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* Read the Administrators alias member list (RIDs). */
static int sam_admin_rids(uint32_t *rids, int max)
{
	Value c;
	const uint8_t *p;
	size_t pos, end;
	int n = 0;

	if (get_value_named("Domains\\Builtin\\Aliases\\00000220", "C", &c) != 0)
		return 0;
	if (c.len < 0x34) {
		value_free(&c);
		return 0;
	}
	p = c.data;
	pos = 0x34 + get32(p + 0x28);
	end = 0x34 + c.len;
	while (pos + 8 <= end && n < max) {
		size_t sid_len = 8 + (size_t)p[pos + 1] * 4;

		if (pos + sid_len > end)
			break;
		rids[n++] = get32(p + pos + sid_len - 4);
		pos += sid_len;
	}
	value_free(&c);
	return n;
}

static int sam_read_user(uint32_t rid, SamUser *u)
{
	char path[256];
	Value f, v;

	memset(u, 0, sizeof(*u));
	u->rid = rid;
	snprintf(path, sizeof(path), SAM_USERS_PATH "\\%08X", rid);

	if (get_value_named(path, "F", &f) == 0) {
		if (f.len >= 0x44) {
			u->acb = get16(f.data + 0x38);
			u->failed = get16(f.data + 0x40);
		}
		value_free(&f);
	}
	if (get_value_named(path, "V", &v) != 0) {
		fprintf(stderr, "winreg: RID %08X: no readable V value\n", rid);
		return -1;
	}
	if (v.len >= 0xCC) {
		uint32_t nofs = get32(v.data + 0x0c), nlen = get32(v.data + 0x10);
		uint32_t fofs = get32(v.data + 0x18), flen = get32(v.data + 0x1c);

		if (nofs + nlen <= v.len - 0xCC)
			utf16_to_utf8(v.data + 0xCC + nofs, nlen, u->name,
				      sizeof(u->name));
		if (flen && fofs + flen <= v.len - 0xCC)
			utf16_to_utf8(v.data + 0xCC + fofs, flen, u->fullname,
				      sizeof(u->fullname));
		u->lm_len = (int)get32(v.data + 0xa0);
		u->nt_len = (int)get32(v.data + 0xac);
	}
	value_free(&v);
	return 0;
}

struct sam_list_ctx {
	uint32_t admins[64];
	int nadmins;
	int seen;
	int printed;
};

static int cb_sam_user(size_t nk_off, void *ud)
{
	struct sam_list_ctx *x = ud;
	NK k;
	char name[64];
	SamUser u;
	int i;

	if (nk_load(nk_off, &k) != 0)
		return 0;
	nk_name(&k, name, sizeof(name));
	if (strcasecmp(name, "Names") == 0)
		return 0;

	u.rid = (uint32_t)strtoul(name, NULL, 16);
	x->seen++;
	if (sam_read_user(u.rid, &u) != 0)
		return 0;
	x->printed++;

	for (i = 0; i < x->nadmins; i++)
		if (x->admins[i] == u.rid)
			u.admin = 1;

	printf("%-10u %-24s %-24s %s%s%s%s\n", u.rid,
	       u.name[0] ? u.name : "?",
	       u.fullname,
	       (u.acb & SAM_ACB_DISABLED) ? "disabled " : "",
	       (u.acb & SAM_ACB_AUTOLOCK) ? "locked " : "",
	       u.nt_len ? "" : "blank-pw ",
	       u.admin ? "admin" : "");
	return 0;
}

static int cmd_sam_users(void)
{
	NK users;
	struct sam_list_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.nadmins = sam_admin_rids(ctx.admins,
				     (int)(sizeof(ctx.admins) / sizeof(uint32_t)));

	if (resolve(SAM_USERS_PATH, &users) != 0) {
		fprintf(stderr, "winreg: this does not look like a SAM hive "
			"(no %s key)\n", SAM_USERS_PATH);
		return 1;
	}
	printf("%-10s %-24s %-24s %s\n", "RID", "USER", "FULL NAME", "STATE");
	walk_subkeys(&users, cb_sam_user, &ctx);

	if (ctx.seen == 0)
		fprintf(stderr, "winreg: 'Users' has no subkeys - is this really the "
			"SAM hive of a Windows install?\n");
	else if (ctx.printed == 0)
		fprintf(stderr, "winreg: %d user record(s) found but none could be "
			"read\n", ctx.seen);
	return 0;
}

/* ----------------------------------------------------- base block checksum */

static uint32_t calc_regf_checksum(const uint8_t *hdr)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i < 0x1fc; i += 4)
		sum ^= get32(hdr + i);
	return sum;
}

static int fix_checksum(int fd)
{
	uint8_t hdr[0x200];
	uint32_t sum;

	if (pread(fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr))
		return -1;
	sum = calc_regf_checksum(hdr);
	hdr[0x1fc] = (uint8_t)(sum);
	hdr[0x1fd] = (uint8_t)(sum >> 8);
	hdr[0x1fe] = (uint8_t)(sum >> 16);
	hdr[0x1ff] = (uint8_t)(sum >> 24);
	if (pwrite(fd, hdr + 0x1fc, 4, 0x1fc) != 4)
		return -1;
	return 0;
}

static int cmd_sam_reset(const char *who, int dry_run, int unlock, int fd)
{
	SamUser u;
	char path[256];
	size_t voff, vlen, foff, flen;
	uint16_t acb, failed;
	uint32_t patch;
	int rc = 1;
	int rid;

	if (!who || !*who) {
		fprintf(stderr, "winreg: sam-reset needs a user name or RID\n");
		return 2;
	}

	/* resolve a user name through Users\Names\<name>, or accept a RID */
	rid = (int)strtoul(who, NULL, 0);
	if (rid <= 0 || rid > 0xffff) {
		Value nv;
		char npath[300];

		snprintf(npath, sizeof(npath), SAM_USERS_PATH "\\Names\\%s", who);
		if (get_value_named(npath, who, &nv) != 0) {
			fprintf(stderr, "winreg: no such local user: %s\n", who);
			return 1;
		}
		if (nv.len < 12) {
			fprintf(stderr, "winreg: cannot read RID for %s\n", who);
			value_free(&nv);
			return 1;
		}
		rid = (int)get32(nv.data + 8);
		value_free(&nv);
	}

	if (sam_read_user((uint32_t)rid, &u) != 0) {
		fprintf(stderr, "winreg: cannot read user RID %d\n", rid);
		return 1;
	}

	snprintf(path, sizeof(path), SAM_USERS_PATH "\\%08X", rid);

	if (value_span(path, "V", &voff, &vlen) != 0 || vlen < 0xCC) {
		fprintf(stderr, "winreg: cannot patch the V record of %s\n", u.name);
		return 1;
	}
	if (value_span(path, "F", &foff, &flen) != 0 || flen < 0x44) {
		fprintf(stderr, "winreg: cannot patch the F record of %s\n", u.name);
		return 1;
	}

	acb = u.acb;
	if (unlock) {
		acb |= SAM_ACB_PWNOEXP;
		acb &= (uint16_t)~SAM_ACB_DISABLED;
		acb &= (uint16_t)~SAM_ACB_AUTOLOCK;
	}
	failed = 0;

	printf("user      : %s (RID %u)\n", u.name, u.rid);
	printf("password  : %s\n", u.nt_len ? "set (will be cleared)" : "already blank");
	if (unlock) {
		printf("account   : %s%s%s\n",
		       (u.acb & SAM_ACB_DISABLED) ? "disabled " : "",
		       (u.acb & SAM_ACB_AUTOLOCK) ? "locked " : "",
		       (failed || (u.acb & (SAM_ACB_DISABLED | SAM_ACB_AUTOLOCK)))
			       ? "-> enabled/unlocked" : "already enabled");
	}

	if (dry_run) {
		printf("dry run: nothing written\n");
		return 0;
	}

	/* 1. blank both hash lengths so Windows treats the password as empty */
	patch = 0;
	if (pwrite(fd, &patch, 4, (off_t)(voff + 0xa0)) != 4 ||
	    pwrite(fd, &patch, 4, (off_t)(voff + 0xac)) != 4) {
		fprintf(stderr, "winreg: write failed (V record)\n");
		return 1;
	}

	/* 2. account flags */
	{
		uint8_t buf[2];

		buf[0] = (uint8_t)acb;
		buf[1] = (uint8_t)(acb >> 8);
		if (pwrite(fd, buf, 2, (off_t)(foff + 0x38)) != 2) {
			fprintf(stderr, "winreg: write failed (ACB flags)\n");
			return 1;
		}
		buf[0] = (uint8_t)failed;
		buf[1] = (uint8_t)(failed >> 8);
		if (pwrite(fd, buf, 2, (off_t)(foff + 0x40)) != 2) {
			fprintf(stderr, "winreg: write failed (logon counter)\n");
			return 1;
		}
	}

	if (fix_checksum(fd) != 0) {
		fprintf(stderr, "winreg: warning: could not update the hive "
			"checksum\n");
	}

	if (fsync(fd) != 0)
		fprintf(stderr, "winreg: warning: fsync failed: %s\n",
			strerror(errno));

	printf("done: password cleared%s\n", unlock ? ", account enabled" : "");
	rc = 0;
	return rc;
}

/* -------------------------------------------------------------------- main */

static void usage(void)
{
	fputs("usage: winreg <cmd> <hive> [args]\n"
	      "  keys    <hive> [path]\n"
	      "  values  <hive> [path]\n"
	      "  get     <hive> <path> <value> [--raw]\n"
	      "  ls      <hive> [path]\n"
	      "  dump    <hive> [path] [maxdepth]\n"
	      "  exists  <hive> [path]\n"
	      "\n"
	      "SAM password recovery (writes to the hive!):\n"
	      "  sam-users <SAM>                       list local accounts\n"
	      "  sam-reset <SAM> <user|RID> [--dry-run] [--no-unlock]\n"
	      "  cryptpw <password> [salt]             make a $6$ password hash\n"
	      "                                        clear the password, enable\n"
	      "                                        the account (ntpasswd style)\n",
	      stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *cmd, *file, *path = "";
	int fd;
	struct stat st;

	if (argc < 3)
		usage();
	cmd = argv[1];
	file = argv[2];

	if (!strcmp(cmd, "cryptpw")) {
		char salt[24], hash[160];
		const char *pw = argc >= 3 ? argv[2] : "";
		int i;

		if (argc >= 4) {
			snprintf(salt, sizeof(salt), "%.16s", argv[3]);
		} else {
			FILE *r = fopen("/dev/urandom", "rb");
			static const char *b64 =
				"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				"abcdefghijklmnopqrstuvwxyz";
			unsigned char rnd[16];

			if (!r || fread(rnd, 1, sizeof(rnd), r) != sizeof(rnd)) {
				fprintf(stderr, "winreg: no entropy for a salt\n");
				return 1;
			}
			fclose(r);
			for (i = 0; i < 16; i++)
				salt[i] = b64[rnd[i] & 0x3f];
			salt[16] = 0;
		}
		sha512crypt(pw, salt, hash);
		printf("%s\n", hash);
		return 0;
	}

	{
		int needs_write = !strcmp(cmd, "sam-reset");
		int i;

		for (i = 3; i < argc; i++)
			if (!strcmp(argv[i], "--dry-run"))
				needs_write = 0;
		fd = open(file, needs_write ? O_RDWR : O_RDONLY);
	}
	if (fd < 0) {
		fprintf(stderr, "winreg: %s: %s\n", file, strerror(errno));
		return 2;
	}
	if (fstat(fd, &st) != 0 || st.st_size < BASE_BLOCK_SIZE) {
		fprintf(stderr, "winreg: %s: too small to be a hive\n", file);
		return 2;
	}
	HLEN = (size_t)st.st_size;
	H = mmap(NULL, HLEN, PROT_READ, MAP_PRIVATE, fd, 0);
	if (H == MAP_FAILED) {
		fprintf(stderr, "winreg: mmap %s: %s\n", file, strerror(errno));
		return 2;
	}
	if (memcmp(H, "regf", 4) != 0) {
		fprintf(stderr, "winreg: %s: not a registry hive (no regf signature)\n",
			file);
		return 2;
	}
	ROOT_CELL = rd32(0x24);
	if (rd32(0x04) != rd32(0x08))
		fprintf(stderr, "winreg: warning: %s is dirty (unflushed log), "
			"recent changes may be missing\n", file);

	if (!strcmp(cmd, "sam-users"))
		return cmd_sam_users();
	if (!strcmp(cmd, "sam-reset")) {
		const char *who = NULL;
		int dry_run = 0, unlock = 1;

		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--dry-run"))
				dry_run = 1;
			else if (!strcmp(argv[i], "--no-unlock"))
				unlock = 0;
			else if (!who)
				who = argv[i];
		}
		return cmd_sam_reset(who, dry_run, unlock, fd);
	}


	if (!strcmp(cmd, "keys") || !strcmp(cmd, "ls") || !strcmp(cmd, "values") ||
	    !strcmp(cmd, "dump") || !strcmp(cmd, "exists") || !strcmp(cmd, "get")) {
		if (argc >= 4)
			path = argv[3];
	}

	NK k;
	if (resolve(path, &k) != 0) {
		fprintf(stderr, "winreg: key not found: %s\n", path);
		return 1;
	}

	if (!strcmp(cmd, "exists")) {
		return 0;
	} else if (!strcmp(cmd, "keys")) {
		struct name_only x = { 1 };
		walk_subkeys(&k, cb_print_key, &x);
		if (!x.first)
			putchar('\n');
		return 0;
	} else if (!strcmp(cmd, "values")) {
		walk_values(&k, cb_tsv, NULL);
		return 0;
	} else if (!strcmp(cmd, "ls")) {
		char n[1024];
		nk_name(&k, n, sizeof(n));
		printf("%s\\\n", n);
		walk_subkeys(&k, cb_ls, NULL);
		walk_values(&k, cb_tsv, NULL);
		return 0;
	} else if (!strcmp(cmd, "dump")) {
		int maxdepth = -1;
		if (argc >= 5)
			maxdepth = atoi(argv[4]);
		char dpath[8192];

		fputs("Windows Registry Editor Version 5.00\n", stdout);
		if (!*path)
			nk_name(&k, dpath, sizeof(dpath));
		else
			snprintf(dpath, sizeof(dpath), "%s", path);
		dump_key(stdout, &k, dpath, 0, maxdepth);
		return 0;
	} else if (!strcmp(cmd, "get")) {
		struct get_ctx g;

		if (argc < 5)
			usage();
		g.want = argv[4];
		g.found = 0;
		walk_values(&k, cb_get, &g);
		if (!g.found) {
			fprintf(stderr, "winreg: value not found: %s\n", g.want);
			return 1;
		}
		putchar('\n');
		return 0;
	}
	usage();
	return 2;
}
