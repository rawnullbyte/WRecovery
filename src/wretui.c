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
	free(out);

	if (nusers == 0) {
		message("Reset a Windows password", "No local accounts found in the SAM hive.");
		return;
	}
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
	"",
	"Reset a Windows password",
	"Save reports to a USB stick",
	"",
	"Shell",
	"Reboot",
	"Power off",
};

enum {
	M_DISKS, M_MOUNT, M_UMOUNT, M_SEP1, M_NET, M_WIFI, M_VPN, M_REPORTS,
	M_SEP2, M_PASSWD, M_SAVE, M_SEP3, M_SHELL, M_REBOOT, M_POWER
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
	printf(A_BOLD " WRecovery " WRE_VERSION A_OFF " - offline Windows recovery");
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