/*
 * wretui - full screen menu for the WRecovery live environment
 *
 * No ncurses, no terminfo: plain ANSI escapes and termios, so it works on the
 * VGA console, on a serial terminal and over IPMI. Every action is the same
 * shell command you could type yourself, so the menu can never do anything
 * the CLI cannot.
 *
 * Keys: arrows / j k move, Enter selects, q or Esc leaves the menu.
 * Choosing "Shell" replaces the menu with a login shell; exiting the shell
 * brings the menu back (init respawns it).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <sys/wait.h>

#ifndef WRE_VERSION
#define WRE_VERSION "dev"
#endif

#define KEY_UP    1000
#define KEY_DOWN  1001
#define KEY_PGUP  1002
#define KEY_PGDN  1003
#define KEY_HOME  1004
#define KEY_END   1005
#define KEY_ESC   1006
#define KEY_NONE  1007

#define A_BOLD "\033[1m"
#define A_REV  "\033[7m"
#define A_OFF  "\033[0m"

static int rows = 24, cols = 80;
static struct termios saved_term;
static int raw_mode = 0;

/* --------------------------------------------------------------- terminal -- */

static void term_restore(void)
{
	if (raw_mode) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_term);
		raw_mode = 0;
	}
}

static void term_raw(void)
{
	struct termios t;

	if (raw_mode)
		return;
	if (tcgetattr(STDIN_FILENO, &saved_term) != 0)
		return;
	t = saved_term;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_iflag &= ~(IXON | ICRNL);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0)
		raw_mode = 1;
}

static void term_size(void)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
	    ws.ws_col > 0) {
		rows = ws.ws_row;
		cols = ws.ws_col;
	}
	if (rows < 10)
		rows = 24;
	if (cols < 40)
		cols = 80;
}

static void clear_screen(void) { fputs("\033[H\033[2J", stdout); }
static void goto_xy(int r, int c) { printf("\033[%d;%dH", r, c); }

/* Print at most cols-1 characters so nothing wraps around. */
static void put_line(const char *text)
{
	int n = (int)strlen(text);

	if (n > cols - 1)
		n = cols - 1;
	fwrite(text, 1, (size_t)n, stdout);
	fputc('\n', stdout);
}

static void put_row(int row, const char *text)
{
	goto_xy(row, 1);
	fputs("\033[2K", stdout);
	put_line(text);
}

/* --------------------------------------------------------------- input ----- */

static int read_key(void)
{
	unsigned char c, s1, s2;

	if (read(STDIN_FILENO, &c, 1) != 1)
		return 'q';          /* EOF: leave the menu instead of spinning */
	if (c == 27) {
		struct pollfd p = { STDIN_FILENO, POLLIN, 0 };

		if (poll(&p, 1, 40) <= 0)
			return KEY_ESC;
		if (read(STDIN_FILENO, &s1, 1) != 1 || (s1 != '[' && s1 != 'O'))
			return KEY_ESC;
		if (read(STDIN_FILENO, &s2, 1) != 1)
			return KEY_ESC;
		switch (s2) {
		case 'A': return KEY_UP;
		case 'B': return KEY_DOWN;
		case 'C': return KEY_DOWN;
		case 'D': return KEY_UP;
		case 'H': return KEY_HOME;
		case 'F': return KEY_END;
		case '5': read(STDIN_FILENO, &s2, 1); return KEY_PGUP;
		case '6': read(STDIN_FILENO, &s2, 1); return KEY_PGDN;
		}
		return KEY_NONE;
	}
	if (c == '\r' || c == '\n')
		return '\n';
	return c;
}

/* --------------------------------------------------------------- output ---- */

struct buf {
	char *data;
	size_t len, cap;
};

static void buf_add(struct buf *b, const void *p, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 4096;

		while (b->len + n + 1 > cap)
			cap *= 2;
		b->data = realloc(b->data, cap);
		if (!b->data)
			exit(1);
		b->cap = cap;
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
	b->data[b->len] = 0;
}

/* Run a command, capture stdout+stderr. */
static char *run_capture(const char *cmd, size_t *outlen)
{
	struct buf b = { 0 };
	FILE *f;
	char tmp[4096];
	size_t n;

	f = popen(cmd, "r");
	if (!f)
		return NULL;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
		buf_add(&b, tmp, n);
	pclose(f);
	if (!b.data)
		buf_add(&b, "", 0);
	*outlen = b.len;
	return b.data;
}

/* ---------------------------------------------------------------- pager ---- */

static void page(const char *title, const char *text)
{
	char *copy = strdup(text ? text : "");
	char **lines = NULL;
	int nlines = 0, cap = 0, top = 0, key;
	char *p, *nl;
	int body = rows - 4;

	p = copy;
	while (p && *p) {
		nl = strchr(p, '\n');
		if (nl)
			*nl = 0;
		if (nlines == cap) {
			cap = cap ? cap * 2 : 128;
			lines = realloc(lines, (size_t)cap * sizeof(*lines));
		}
		lines[nlines++] = p;
		if (!nl)
			break;
		p = nl + 1;
	}

	term_raw();
	for (;;) {
		int i;

		term_size();
		body = rows - 4;
		if (body < 3)
			body = 3;
		if (top > nlines - body)
			top = nlines - body;
		if (top < 0)
			top = 0;

		clear_screen();
		goto_xy(1, 1);
		printf(A_BOLD "[ %s ]" A_OFF, title);
		if (nlines > body) {
			goto_xy(1, cols - 12);
			printf(" %d%%", top * 100 / (nlines > body ? nlines - body : 1));
		}
		put_line("");
		for (i = 0; i < body; i++) {
			if (top + i < nlines)
				put_row(2 + i, lines[top + i]);
			else
				put_row(2 + i, "");
		}
		goto_xy(rows, 1);
		printf(A_REV " up/down scroll   space page   q back " A_OFF);
		fflush(stdout);

		key = read_key();
		if (key == 'q' || key == KEY_ESC || key == '\n' || key == 'Q')
			break;
		if (key == KEY_UP || key == 'k')
			top--;
		else if (key == KEY_DOWN || key == 'j')
			top++;
		else if (key == ' ' || key == KEY_PGDN || key == 'f')
			top += body;
		else if (key == KEY_PGUP || key == 'b')
			top -= body;
		else if (key == KEY_HOME || key == 'g')
			top = 0;
		else if (key == KEY_END || key == 'G')
			top = nlines;
		if (top < 0)
			top = 0;
	}
	free(lines);
	free(copy);
}

static void message(const char *title, const char *text)
{
	page(title, text);
}

static void action(const char *title, const char *cmd)
{
	size_t n = 0;
	char *out = run_capture(cmd, &n);

	if (!out) {
		message(title, "(failed to run the command)");
		return;
	}
	page(title, out);
	free(out);
}

/* ---------------------------------------------------------------- menus ---- */

static int pick(const char *title, char **items, int n, const char *help)
{
	int sel = 0, top = 0, key, body;

	term_raw();
	for (;;) {
		int i;

		term_size();
		body = rows - 5;
		if (body < 3)
			body = 3;
		if (sel < top)
			top = sel;
		if (sel >= top + body)
			top = sel - body + 1;

		clear_screen();
		goto_xy(1, 1);
		printf(A_BOLD "%s" A_OFF, title);
		put_line("");
		for (i = 0; i < body; i++) {
			int idx = top + i;

			if (idx >= n)
				break;
			goto_xy(3 + i, 1);
			fputs("\033[2K", stdout);
			if (idx == sel) {
				fputs(A_REV " > " A_OFF, stdout);
				fputs(A_REV, stdout);
			} else {
				fputs("   ", stdout);
			}
			put_line(items[idx]);
			if (idx == sel)
				fputs(A_OFF, stdout);
		}
		goto_xy(rows - 1, 1);
		if (help && *help)
			put_line(help);
		goto_xy(rows, 1);
		printf(A_REV " up/down move   enter select   q cancel " A_OFF);
		fflush(stdout);

		key = read_key();
		if (key == 'q' || key == KEY_ESC)
			return -1;
		if (key == '\n')
			return sel;
		if (key == KEY_UP || key == 'k')
			sel = (sel + n - 1) % n;
		else if (key == KEY_DOWN || key == 'j')
			sel = (sel + 1) % n;
		else if (key == KEY_PGUP)
			sel = sel - body > 0 ? sel - body : 0;
		else if (key == KEY_PGDN)
			sel = sel + body < n ? sel + body : n - 1;
		else if (key == KEY_HOME)
			sel = 0;
		else if (key == KEY_END)
			sel = n - 1;
	}
}

/* ------------------------------------------------------ password reset ----- */

static int count_mounted(void)
{
	FILE *f = fopen("/run/state/mounts.db", "r");
	char line[512];
	int n = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, "|mounted|"))
			n++;
	fclose(f);
	return n;
}

static int count_installs(void)
{
	size_t n = 0;
	char *out = run_capture("winstall", &n);
	int count = 0;
	char *p;

	if (!out)
		return 0;
	for (p = out; *p; p++)
		if (*p == '\n')
			count++;
	free(out);
	return count;
}

static void ui_reset_password(void)
{
	char *out, *p, *nl;
	size_t n = 0;
	char *vols[32], *sams[32], *roots[32];
	int ninst = 0, i, picki = 0;
	char cmd[1024];
	char *names[128], *labels[128];
	int nusers = 0;

	out = run_capture("winstall", &n);
	if (!out || !*out) {
		free(out);
		message("Reset a Windows password",
			"No Windows installation found.\n\n"
			"Mount the volumes first (menu: Mount all volumes).");
		return;
	}
	/* installs are "volume|windows_root|sam_hive" lines */
	for (p = out; *p && ninst < 32; p = nl ? nl + 1 : p + strlen(p)) {
		char vol[64] = "", root[512] = "", sam[512] = "";

		nl = strchr(p, '\n');
		if (nl)
			*nl = 0;
		if (sscanf(p, "%63[^|]|%511[^|]|%511[^|]", vol, root, sam) != 3)
			break;
		if (!*sam)
			continue;
		vols[ninst] = strdup(vol);
		roots[ninst] = strdup(root);
		sams[ninst] = strdup(sam);
		ninst++;
	}
	free(out);
	if (ninst == 0) {
		message("Reset a Windows password",
			"Found a Windows directory but no SAM hive.");
		return;
	}

	if (ninst > 1) {
		char *items[32];

		for (i = 0; i < ninst; i++) {
			char *s = malloc(600);

			snprintf(s, 600, "%s   %s", vols[i], roots[i]);
			items[i] = s;
		}
		picki = pick("Which Windows installation?", items, ninst, NULL);
		for (i = 0; i < ninst; i++)
			free(items[i]);
		if (picki < 0)
			return;
	}

	snprintf(cmd, sizeof(cmd), "winreg sam-users \"%s\"", sams[picki]);
	out = run_capture(cmd, &n);
	if (!out) {
		message("Reset a Windows password", "Could not read the SAM hive.");
		return;
	}
	for (p = out; *p && nusers < 128; p = nl ? nl + 1 : p + strlen(p)) {
		char rid[32] = "", name[128] = "", flags[128] = "", line[512];

		nl = strchr(p, '\n');
		if (nl)
			*nl = 0;
		if (strncmp(p, "RID", 3) == 0 || !*p)
			continue;
		snprintf(line, sizeof(line), "%s", p);
		if (sscanf(p, "%31s %127s %*s", rid, name) != 2)
			continue;
		if (strstr(line, "disabled"))
			snprintf(flags, sizeof(flags), "  [disabled]");
		labels[nusers] = malloc(300);
		snprintf(labels[nusers], 300, "%-20s RID %-6s%s", name, rid, flags);
		names[nusers] = strdup(name);
		nusers++;
	}

	if (nusers == 0) {
		size_t mlen = strlen(out) + 512;
		char *msg = malloc(mlen);

		snprintf(msg, mlen,
			 "No local accounts could be read from:\n  %s\n\n"
			 "winreg sam-users said:\n%.900s", sams[picki], out);
		free(out);
		message("Reset a Windows password", msg);
		free(msg);
		return;
	}
	free(out);
	i = pick("Which account?", labels, nusers, "The chosen account gets an empty password.");
	if (i < 0)
		return;

	{
		char *items[2] = { "No, go back", "Yes, clear the password" };
		char help[512];

		snprintf(help, sizeof(help),
			 "Clear the password of %s on %s? A backup of the SAM is written first.",
			 names[i], vols[picki]);
		if (pick("Reset a Windows password", items, 2, help) != 1) {
			for (i = 0; i < nusers; i++) {
				free(labels[i]);
				free(names[i]);
			}
			return;
		}
	}

	snprintf(cmd, sizeof(cmd), "wpasswd -y %s %s", vols[picki], names[i]);
	for (i = 0; i < nusers; i++) {
		free(labels[i]);
		free(names[i]);
	}
	action("Password reset", cmd);
}

/* ----------------------------------------------------------- report files -- */

static void ui_reports(void)
{
	size_t n = 0;
	char *out = run_capture("ls -1 /run/reports 2>/dev/null", &n);
	char *items[256];
	int count = 0, sel;
	char *p, *nl;

	if (!out || !*out) {
		free(out);
		message("Saved reports",
			"Nothing in /run/reports yet.\n\n"
			"Run the network report first.");
		return;
	}
	for (p = out; *p && count < 256; p = nl ? nl + 1 : p + strlen(p)) {
		nl = strchr(p, '\n');
		if (nl)
			*nl = 0;
		if (*p)
			items[count++] = strdup(p);
	}
	free(out);
	if (!count) {
		message("Saved reports", "Nothing in /run/reports yet.");
		return;
	}
	sel = pick("Reports in /run/reports", items, count, NULL);
	if (sel >= 0) {
		char cmd[1024];
		char path[512];
		size_t len = strlen(items[sel]);

		/* directories have no dot: list them instead of dumping binary */
		snprintf(path, sizeof(path), "/run/reports/%s", items[sel]);
		if (len > 4 && strstr(items[sel], ".xml"))
			snprintf(cmd, sizeof(cmd), "cat \"%s\"", path);
		else if (strchr(items[sel], '.'))
			snprintf(cmd, sizeof(cmd), "cat \"%s\"", path);
		else
			snprintf(cmd, sizeof(cmd), "ls -l \"%s\"", path);
		action(items[sel], cmd);
	}
	while (count-- > 0)
		free(items[count]);
}

/* ---------------------------------------------------------- file browser --- */

#define MAX_ENTRIES 4000

struct entry {
	char name[256];
	int is_dir;
	unsigned long long size;
};

static int entry_cmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	int r;

	if (x->is_dir != y->is_dir)
		return x->is_dir ? -1 : 1;
	r = strcasecmp(x->name, y->name);
	return r ? r : strcmp(x->name, y->name);
}

static int load_dir(const char *path, struct entry **out, int *count)
{
	DIR *d = opendir(path);
	struct dirent *de;
	struct entry *list = NULL;
	int n = 0, cap = 0;

	*out = NULL;
	*count = 0;
	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL) {
		struct stat st;
		char full[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			list = realloc(list, (size_t)cap * sizeof(*list));
			if (!list) {
				closedir(d);
				return -1;
			}
		}
		snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
		if (lstat(full, &st) != 0)
			continue;
		memset(&list[n], 0, sizeof(list[n]));
		snprintf(list[n].name, sizeof(list[n].name), "%s", de->d_name);
		list[n].is_dir = S_ISDIR(st.st_mode);
		list[n].size = (unsigned long long)st.st_size;
		if (n >= MAX_ENTRIES)
			break;
		n++;
	}
	closedir(d);
	qsort(list, (size_t)n, sizeof(*list), entry_cmp);
	*out = list;
	*count = n;
	return 0;
}

static void human_size(unsigned long long b, char *out, size_t outsz)
{
	static const char *u[] = { "B", "K", "M", "G", "T" };
	int i = 0;

	while (b >= 1024 && i < 4) {
		b /= 1024;
		i++;
	}
	snprintf(out, outsz, i ? "%llu%s" : "%llu%s", b, u[i]);
}

static int choose_destination(char *out, size_t outsz)
{
	char *items[64];
	int n = 0, i, sel;
	FILE *f = fopen("/run/state/mounts.db", "r");
	char line[512];

	items[n++] = strdup("/tmp  (RAM, lost on reboot)");
	if (f) {
		while (fgets(line, sizeof(line), f) && n < 63) {
			char *p = line, *fields[6] = { 0 };
			int k;

			for (k = 0; k < 6 && p; k++) {
				fields[k] = p;
				p = strchr(p, '|');
				if (p)
					*p++ = 0;
			}
			if (!fields[0] || !fields[3] || !fields[5])
				continue;
			if (strcmp(fields[2], "mounted") != 0)
				continue;
			if (strcmp(fields[5], "rw") != 0)
				continue;
			{
				char *tmp = malloc(320);

				snprintf(tmp, 320, "/mnt/%s  (%s%s%s)", fields[0],
					 fields[3], *fields[4] ? ", " : "",
					 fields[4]);
				items[n++] = tmp;
			}
		}
		fclose(f);
	}
	sel = pick("Copy to which volume?", items, n,
		   n == 1 ? "Nothing is mounted read-write - /tmp lives in RAM only. "
			    "Use 'wusb' for a USB stick." : NULL);
	if (sel < 0) {
		for (i = 0; i < n; i++)
			free(items[i]);
		return -1;
	}
	snprintf(out, outsz, "%s", strtok(items[sel], " "));
	for (i = 0; i < n; i++)
		free(items[i]);
	return 0;
}

static void view_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	size_t got, cap = 2 * 1024 * 1024;
	int binary = 0, i;
	char title[300];

	if (!f) {
		message("Cannot open", path);
		return;
	}
	buf = malloc(cap + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	got = fread(buf, 1, cap, f);
	fclose(f);
	buf[got] = 0;
	for (i = 0; i < (int)got && i < 4096; i++)
		if (buf[i] == 0) {
			binary = 1;
			break;
		}
	snprintf(title, sizeof(title), "%s (%zu bytes%s)", path, got,
		 got == cap ? ", first 2 MiB" : "");
	if (binary) {
		/* hex dump, 16 bytes per line */
		size_t pos = 0, outlen = (got / 16 + 1) * 80;
		char *out = malloc(outlen + 128);
		char *w = out;

		if (!out) {
			free(buf);
			return;
		}
		while (pos < got) {
			size_t k;
			int col = 0;

			w += sprintf(w, "%08zx  ", pos);
			for (k = 0; k < 16; k++) {
				if (pos + k < got)
					w += sprintf(w, "%02x ", (unsigned char)buf[pos + k]);
				else
					w += sprintf(w, "   ");
				if (k == 7)
					*w++ = ' ';
			}
			*w++ = ' ';
			for (k = 0; k < 16 && pos + k < got; k++) {
				unsigned char c = (unsigned char)buf[pos + k];

				*w++ = isprint(c) ? (char)c : '.';
				col++;
			}
			if (col)
				*w = 0;
			w += sprintf(w, "\n");
			pos += 16;
			if ((size_t)(w - out) + 200 > outlen)
				break;
		}
		*w = 0;
		page(title, out);
		free(out);
	} else {
		page(title, buf);
	}
	free(buf);
}

static void browse_dir(const char *start, const char *volname)
{
	char path[PATH_MAX], title[400];
	struct entry *list = NULL;
	int count = 0, sel = 0, top = 0, off = 0;

	snprintf(path, sizeof(path), "%s", start);
	for (;;) {
		int i, key, body;

		if (load_dir(path, &list, &count) != 0) {
			message("Cannot read directory", path);
			return;
		}
		off = strcmp(path, "/") ? 1 : 0;
		if (sel >= count + off)
			sel = count + off - 1;
		if (sel < 0)
			sel = 0;
		term_size();
		body = rows - 6;
		if (body < 3)
			body = 3;
		if (sel < top)
			top = sel;
		if (sel >= top + body)
			top = sel - body + 1;

		clear_screen();
		goto_xy(1, 1);
		snprintf(title, sizeof(title), " " A_BOLD "%s" A_OFF " [%s]", path,
			 volname);
		put_line(title);
		put_row(2, " enter open   c copy here   q back");

		for (i = 0; i < body; i++) {
			int idx = top + i;
			char line[600];

			goto_xy(3 + i, 1);
			fputs("\033[2K", stdout);
			if (idx >= count + off)
				break;
			if (off && idx == 0) {
				snprintf(line, sizeof(line), "d %-52.52s %10s",
					 "..", "<parent>");
			} else {
				struct entry *e = &list[idx - off];
				char sz[32];

				human_size(e->size, sz, sizeof(sz));
				snprintf(line, sizeof(line), "%s %-52.52s %10s",
					 e->is_dir ? "d" : "-", e->name,
					 e->is_dir ? "<dir>" : sz);
			}
			if (idx == sel) {
				fputs(A_REV " > " A_OFF A_REV, stdout);
				put_line(line);
				fputs(A_OFF, stdout);
			} else {
				fputs("   ", stdout);
				put_line(line);
			}
		}
		goto_xy(rows - 1, 1);
		if (count >= MAX_ENTRIES)
			put_line(" (directory truncated at 4000 entries)");
		else
			put_line("");
		goto_xy(rows, 1);
		printf(A_REV " up/down move   enter open   c copy   q back " A_OFF);
		fflush(stdout);

		key = read_key();
		if (key == 'q' || key == KEY_ESC) {
			free(list);
			return;
		}
		if (key == KEY_UP || key == 'k')
			sel = sel > 0 ? sel - 1 : count + off - 1;
		else if (key == KEY_DOWN || key == 'j')
			sel = sel < count + off - 1 ? sel + 1 : 0;
		else if (key == KEY_PGUP)
			sel = sel - body > 0 ? sel - body : 0;
		else if (key == KEY_PGDN)
			sel = sel + body < count + off ? sel + body : count + off - 1;
		else if (key == KEY_HOME)
			sel = 0;
		else if (key == KEY_END)
			sel = count + off - 1;
		else if (key == 'c') {
			char dst[PATH_MAX], src[PATH_MAX], cmd[PATH_MAX * 2 + 32];

			if (off && sel == 0)
				continue;
			snprintf(src, sizeof(src), "%s/%s", path, list[sel - off].name);
			if (choose_destination(dst, sizeof(dst)) != 0)
				continue;
			if (!strcmp(dst, "/tmp"))
				snprintf(dst, sizeof(dst), "/tmp");
			snprintf(cmd, sizeof(cmd), "wcopy \"%s\" \"%s\"", src, dst);
			{
				size_t outlen = 0;
				char *out = run_capture(cmd, &outlen);

				page("Copy", out ? out : "(no output)");
				free(out);
			}
		} else if (key == '\n') {
			char full[PATH_MAX];

			if (off && sel == 0) {
				char *slash;

				/* up one level */
				slash = strrchr(path, '/');
				if (!slash || slash == path)
					snprintf(path, sizeof(path), "/");
				else
					*slash = 0;
				sel = 0;
				top = 0;
				continue;
			}
			snprintf(full, sizeof(full), "%s/%s", path, list[sel - off].name);
			if (list[sel - off].is_dir) {
				snprintf(path, sizeof(path), "%s", full);
				sel = 0;
				top = 0;
			} else {
				view_file(full);
			}
		}
	}
}

static void ui_browse(void)
{
	char *items[64];
	char paths[64][PATH_MAX];
	int n = 0, i, sel;
	FILE *f = fopen("/run/state/mounts.db", "r");
	char line[512];

	if (!f) {
		message("Browse files", "Nothing is mounted yet.");
		return;
	}
	while (fgets(line, sizeof(line), f) && n < 63) {
		char *p = line, *fields[6] = { 0 };
		int k;

		for (k = 0; k < 6 && p; k++) {
			fields[k] = p;
			p = strchr(p, '|');
			if (p)
				*p++ = 0;
		}
		if (!fields[0] || !fields[2])
			continue;
		if (strcmp(fields[2], "mounted") != 0)
			continue;
		snprintf(paths[n], sizeof(paths[n]), "/mnt/%s", fields[0]);
		items[n] = malloc(320);
		snprintf(items[n], 320, "%-12s %-8s %s", fields[0],
			 fields[3] ? fields[3] : "?", fields[4] ? fields[4] : "");
		n++;
	}
	fclose(f);
	if (n == 0) {
		message("Browse files",
			"No volumes are mounted.\n\nUse 'Mount all volumes' first.");
		return;
	}
	sel = (n == 1) ? 0 : pick("Browse which volume?", items, n, NULL);
	if (sel < 0) {
		for (i = 0; i < n; i++)
			free(items[i]);
		return;
	}
	browse_dir(paths[sel], items[sel]);
	for (i = 0; i < n; i++)
		free(items[i]);
}

/* ------------------------------------------------------------------ shell -- */

static void drop_to_shell(void)
{
	term_restore();
	clear_screen();
	printf("Leaving the menu. Type 'wretui' to come back, or 'exit' to return\n"
	       "here automatically.\n\n");
	fflush(stdout);
	execl("/bin/sh", "sh", "-l", NULL);
	/* if exec fails, keep the menu alive */
	perror("sh");
}

/* ------------------------------------------------------------------ main --- */

static const char *MENU[] = {
	"Disks, partitions and Windows installs",
	"Mount all volumes (read-only)",
	"Unmount all volumes",
	"",
	"Network configuration report",
	"Wi-Fi profiles and keys",
	"VPN, proxy and hosts file",
	"Browse saved reports",
	"Browse files on a volume",
	"",
	"Reset a Windows password",
	"Put a command prompt at the login screen",
	"ESXi host: network configuration",
	"ESXi host: reset root password",
	"Save reports to a USB stick",
	"",
	"Shell",
	"Reboot",
	"Power off",
};

/* keep in the same order as MENU[] above */
enum {
	M_DISKS, M_MOUNT, M_UMOUNT, M_SEP1, M_NET, M_WIFI, M_VPN, M_REPORTS,
	M_BROWSE, M_SEP2, M_PASSWD, M_CONSOLE, M_ESXI_NET, M_ESXI_PW, M_SAVE,
	M_SEP3, M_SHELL, M_REBOOT, M_POWER
};

/* Draw the menu once and return the chosen action, or -1 to leave. */
static int menu_run(int *selp, int *top)
{
	int key, i, row, body, n = (int)(sizeof(MENU) / sizeof(MENU[0]));

	term_size();
	body = rows - 6;
	if (body < 3)
		body = 3;

	clear_screen();
	goto_xy(1, 1);
	printf(A_BOLD " WRecovery " WRE_VERSION A_OFF " - offline Windows / ESXi recovery");
	put_line("");
	{
		char status[256];

		snprintf(status, sizeof(status),
			 " %d volume(s) mounted   %d Windows install(s)   reports in /run/reports",
			 count_mounted(), count_installs());
		put_row(2, status);
	}

	if (*top < 0)
		*top = 0;
	if (*selp < *top)
		*top = *selp;
	if (*selp >= *top + body)
		*top = *selp - body + 1;

	row = 3;
	for (i = *top; i < n && row < rows - 2; i++) {
		if (!*MENU[i]) {
			goto_xy(row++, 1);
			fputs("  ------------------------------------------------------------\n", stdout);
			continue;
		}
		goto_xy(row, 1);
		fputs("\033[2K", stdout);
		if (i == *selp) {
			fputs(A_REV " > " A_OFF A_REV, stdout);
			put_line(MENU[i]);
			fputs(A_OFF, stdout);
		} else {
			fputs("   ", stdout);
			put_line(MENU[i]);
		}
		row++;
	}

	goto_xy(rows - 1, 1);
	put_line(" Enter selects - arrows/jk move - q drops to a shell");
	goto_xy(rows, 1);
	printf(A_REV " every entry here is also a command; 'whelp' lists them " A_OFF);
	fflush(stdout);

	key = read_key();
	if (key == 'q' || key == KEY_ESC)
		return -1;
	if (key == '\n')
		return MENU[*selp][0] ? *selp : -2;
	if (key == KEY_UP || key == 'k') {
		do {
			*selp = (*selp + n - 1) % n;
		} while (!*MENU[*selp]);
	} else if (key == KEY_DOWN || key == 'j') {
		do {
			*selp = (*selp + 1) % n;
		} while (!*MENU[*selp]);
	} else if (key == KEY_HOME) {
		*selp = M_DISKS;
	} else if (key == KEY_END) {
		*selp = M_POWER;
	} else if (key == KEY_PGUP) {
		*selp = M_DISKS;
	} else if (key == KEY_PGDN) {
		*selp = M_POWER;
	}
	return -2;
}

static void on_signal(int sig)
{
	(void)sig;
	term_restore();
	_exit(1);
}

int main(void)
{
	int sel = M_DISKS, top = 0, rc;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	atexit(term_restore);

	term_size();
	term_raw();

	for (;;) {
		rc = menu_run(&sel, &top);
		if (rc == -1)
			drop_to_shell();
		if (rc == -2)
			continue;
		sel = rc;

		switch (sel) {
		case M_DISKS:
			action("Disks, partitions and Windows installs", "wdisks");
			break;
		case M_MOUNT:
			action("Mount all volumes", "wmount");
			break;
		case M_UMOUNT:
			action("Unmount all volumes", "wmount -u");
			break;
		case M_NET:
			action("Network configuration", "wnet");
			break;
		case M_WIFI:
			action("Wi-Fi profiles and keys",
			       "awk '/== Wi-Fi profiles/,/== VPN|== hosts/' "
			       "/run/reports/network-*.txt 2>/dev/null");
			break;
		case M_VPN:
			action("VPN, proxy and hosts",
			       "awk '/== VPN|== Proxy|== hosts/,0' "
			       "/run/reports/network-*.txt 2>/dev/null");
			break;
		case M_REPORTS:
			ui_reports();
			break;
		case M_PASSWD:
			ui_reset_password();
			break;
		case M_CONSOLE: {
			char *items[2] = { "No, go back", "Yes, enable it" };
			char help[512];

			snprintf(help, sizeof(help),
				 "Replace sethc.exe/utilman.exe with cmd.exe on the Windows "
				 "volume? Undo with: wpasswd --console --undo");
			if (pick("Command prompt at login", items, 2, help) == 1)
				action("Command prompt at login", "wpasswd -y --console");
			break;
		}
		case M_BROWSE:
			ui_browse();
			break;
		case M_ESXI_NET:
			action("ESXi host configuration", "wesxi --report");
			break;
		case M_ESXI_PW: {
			char *items[2] = { "No, go back", "Yes, clear it" };
			char help[512];

			snprintf(help, sizeof(help),
				 "Clear the root password of the ESXi host on the mounted "
				 "bootpartition? (backup is kept, undo: wesxi --undo)");
			if (pick("ESXi root password", items, 2, help) == 1)
				action("ESXi root password", "wesxi --password");
			break;
		}
		case M_SAVE:
			action("Save reports to USB",
			       "wusb && wsave");
			break;
		case M_SHELL:
			drop_to_shell();
			break;
		case M_REBOOT: {
			char *items[2] = { "No", "Yes, reboot now" };

			if (pick("Reboot?", items, 2, NULL) == 1) {
				term_restore();
				execl("/sbin/reboot", "reboot", NULL);
			}
			break;
		}
		case M_POWER: {
			char *items[2] = { "No", "Yes, power off" };

			if (pick("Power off?", items, 2, NULL) == 1) {
				term_restore();
				execl("/sbin/poweroff", "poweroff", NULL);
			}
			break;
		}
		}
	}
	return 0;
}