/* hyprctl for gluewc: the same command line, sent to the Hyprland-style
 * socket gluewc serves. Installed outside PATH; the gluewc shell puts it
 * first in its own PATH so the Hyprland shells it runs find it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int
sigfromstate(char *buf, size_t n)
{
	const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
	char path[512];
	FILE *f;
	size_t len;

	if (st && *st)
		snprintf(path, sizeof path, "%s/gluewc/hypr-signature", st);
	else if (home)
		snprintf(path, sizeof path, "%s/.local/state/gluewc/hypr-signature", home);
	else
		return 0;
	if (!(f = fopen(path, "r")))
		return 0;
	if (!fgets(buf, (int)n, f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	len = strlen(buf);
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
		buf[--len] = '\0';
	return len > 0;
}

int
main(int argc, char *argv[])
{
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	const char *rt = getenv("XDG_RUNTIME_DIR"), *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	char sigbuf[128], flags[8] = "", req[16384], buf[65536];
	size_t len = 0, len2 = 0;
	ssize_t n;
	int i, fd, quiet = 0, batch = 0, first = 1, streamed = 0;

	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-j"))
			strcat(flags, "j");
		else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet"))
			quiet = 1;
		else if (!strcmp(argv[i], "--batch"))
			batch = 1;
		else if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--instance")) && i + 1 < argc)
			i++;
		else if (!strcmp(argv[i], "-r"))
			strcat(flags, "r");
		else if (!strcmp(argv[i], "-a"))
			strcat(flags, "a");
		else
			break;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: hyprctl [-j] [--batch] command [args]\n");
		return 1;
	}
	/* hyprpaper has its own socket and gluewc draws no wallpaper */
	if (!strcmp(argv[i], "hyprpaper")) {
		fprintf(stderr, "hyprpaper is not running\n");
		return 1;
	}
	if (!sig || !*sig) {
		if (!sigfromstate(sigbuf, sizeof sigbuf)) {
			fprintf(stderr, "HYPRLAND_INSTANCE_SIGNATURE not set (is gluewc running?)\n");
			return 3;
		}
		sig = sigbuf;
	}
	if (!rt || snprintf(sa.sun_path, sizeof sa.sun_path, "%s/hypr/%s/.socket.sock", rt, sig)
			>= (int)sizeof sa.sun_path)
		return 3;
	len = (size_t)snprintf(req, sizeof req, "%s%s%s", batch ? "[[BATCH]]" : "",
			flags, batch ? "" : "/");
	for (; i < argc && len + 2 < sizeof req; i++, first = 0)
		len += (size_t)snprintf(req + len, sizeof req - len, "%s%s", first ? "" : " ", argv[i]);
	if (len >= sizeof req)
		len = sizeof req - 1;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0
			|| connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
		fprintf(stderr, "Couldn't connect to %s\n", sa.sun_path);
		return 3;
	}
	if (write(fd, req, len) != (ssize_t)len)
		return 3;
	while ((n = read(fd, buf + len2, sizeof buf - 1 - len2)) > 0) {
		len2 += (size_t)n;
		if (len2 + 1 >= sizeof buf) {
			if (!quiet)
				fwrite(buf, 1, len2, stdout);
			len2 = 0;
			streamed = 1;
		}
	}
	close(fd);
	buf[len2] = '\0';
	if (!streamed && !strcmp(buf, "unknown request")) {
		fprintf(stderr, "unknown request\n");
		return 1;
	}
	if (!quiet) {
		fwrite(buf, 1, len2, stdout);
		putchar('\n');
	}
	return 0;
}
