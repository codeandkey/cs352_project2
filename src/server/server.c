#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include <unistd.h>
#include <signal.h>
#include <time.h>

#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 80
#define REQLEN 512
#define BS_SEND 512 /* chunk size when sending over files */

int s_listen = -1;
int s_client = -1;
int is_child = 0;

char req[REQLEN];
int reqlen;

const char* header_ok = "HTTP/1.1 200 OK\r\n";
const char* header_ims = "HTTP/1.1 304 Not Modified\r\n";
const char* header_notfound = "HTTP/1.1 404 Not Found\r\n";

int fail(const char* msg, ...);
int dispatch(void);
void cleanup(void);
void sig(int s);
ssize_t dual_write(int s, const void* data, size_t size);

int main(int argc, char** argv) {
	/*
	 * first, we prepare the listen socket.
	 * this is pretty straightforward with POSIX
	 */

	if (getuid()) return fail("Must be executed as root.\n");

	signal(SIGINT, sig);
	signal(SIGTERM, sig);

	printf("sv: preparing socket\n");

	struct sockaddr_in sa = {0};

	sa.sin_family = AF_INET;
	sa.sin_port = htons(PORT);
	sa.sin_addr.s_addr = INADDR_ANY;

	s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (s_listen < 0) return fail("Failed to create socket.\n");

	int one = 1;
	if (setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one)) {
		return fail("Failed to set socket options.\n");
	}

	if (bind(s_listen, (const struct sockaddr*) &sa, sizeof sa) < 0) return fail("Failed to bind socket.\n");
	if (listen(s_listen, SOMAXCONN) < 0) return fail("Failed to listen on socket.\n");

	/*
	 * then, we chroot into the working directory -- this makes file operations-
	 *   much, much simpler (and also makes the server much more secure)
	 */

	char cwd[PATH_MAX] = {0};
	if (!getcwd(cwd, sizeof cwd)) return fail("Failed to query working directory.\n");

	printf("sv: chrooting into %s\n", cwd);
	if (chroot(cwd)) return fail("Failed to change root path.\n");

	/*
	 * chroot ready. start the server and starting dispatching workers
	 */

	printf("sv: listening for connections on :%d\n", PORT);

	struct sockaddr_in client_addr = {0};
	socklen_t client_len = sizeof client_addr;

	while ((s_client = accept(s_listen, (struct sockaddr*) &client_addr, &client_len)) >= 0) {
		if (fork()) {
			close(s_client); /* close the fd in the main thread so it doesn't get stuck open */
			s_client = -1;
			continue;
		}

		is_child = 1;

		close(s_listen); /* don't hold onto the listen fd */
		s_listen = -1;

		if (getpeername(s_client, (struct sockaddr*) &client_addr, &client_len)) {
			printf("sv[%d]: accepted connection from unknown\n", getpid());
		} else {
			printf("sv[%d]: accepted connection from %s\n", getpid(), inet_ntoa(client_addr.sin_addr));
		}

		int ret = dispatch();

		printf("sv[%d]: worker terminating, status %d\n", getpid(), ret);
		return ret;
	}

	/*
	 * cleanup, we join all working threads and close socket FDs
	 * it's actually impossible to actually get here anyway, the program is terminated via ctrl+c
	 */

	cleanup();
	return 0;
}

int fail(const char* msg, ...) {
	va_list args;
	va_start(args, msg);
	vprintf(msg, args);
	va_end(args);

	cleanup();
	return -1;
}

void cleanup(void) {
	/* cleans up sockets if they aren't already */

	if (s_listen >= 0) close(s_listen);
	s_listen = -1;

	if (s_client >= 0) {
		printf("sv[%d]: dropping connection to client\n", getpid());

		close(s_client);
		s_client = -1;
	}
}

int dispatch(void) {
	/*
	 * get the request data before parsing
	 */

	memset(req, 0, sizeof req);
	if ((reqlen = read(s_client, req, sizeof req)) < 0) {
		return fail("sv[%d]: sock read failed\n", getpid());
	}

	char* savep_line = NULL;
	char* firstline = strtok_r(req, "\r\n", &savep_line);

	char* method = strtok(firstline, " ");
	char* destpath = strtok(NULL, " ");
	char* httpver = strtok(NULL, " ");

	if (!method || !destpath || !httpver) return fail("sv[%d]: invalid request from client\n", getpid());

	printf("sv[%d]: %s %s %s\n", getpid(), method, destpath, httpver);

	/* we have method, url, version. scan for headers.. */

	struct tm* ims_tm = NULL;
	struct tm imst;
	time_t ims_time, mod_time;

	for (char* line = strtok_r(NULL, "\r\n", &savep_line); line; line = strtok_r(NULL, "\r\n", &savep_line)) {
		char* save_keys = NULL;

		if (!strlen(line)) continue;

		char* key = strtok_r(line, ": ", &save_keys);
		char* val = key + strlen(key) + 2;

		if (!key || !val) continue;

		printf("sv[%d]: [%s]: %s\n", getpid(), key, val);

		if (!strcmp(key, "If-Modified-Since")) {
			strptime(val, "%a %b %d %T %Y", &imst);
			ims_tm = &imst;
			ims_time = mktime(ims_tm);
		}
	}

	/* try and access the file. */
	FILE* f = fopen(destpath, "r");

	if (!f) {
		printf("sv[%d]: ", getpid());
		dual_write(s_client, header_notfound, strlen(header_notfound));
		printf("\n");
		return fail("sv[%d]: file not found: %s\n", getpid(), destpath);
	}

	struct stat ft;

	/* realistically this should never fail, the file is already open.. */
	if (fstat(fileno(f), &ft)) return fail("Failed to get fstat for %s\n", destpath); 

	/* before calling it good we use the info from stat() to make sure there's
	 * actually a file here and not a directory (as fopen() wont fail on dirs) */

	if ((ft.st_mode & S_IFMT) != S_IFREG) {
		fclose(f);
		printf("sv[%d]: ", getpid());
		dual_write(s_client, header_notfound, strlen(header_notfound));
		printf("\n");
		return fail("sv[%d]: file type not permitted: %s\n", getpid(), destpath);
	}

	/*
	 * if the user specified a date then we test for that quickly
	 */

	if (ims_tm) {
		mod_time = ft.st_mtime;

		printf("sv[%d]: file modified at UTC: %s", getpid(), ctime(&mod_time));
		printf("sv[%d]: IMS time at UTC: %s", getpid(), ctime(&ims_time));

		if (mod_time < ims_time) {
			fclose(f);
			printf("sv[%d]: ", getpid());
			dual_write(s_client, header_ims, strlen(header_ims));
			printf("\n");
			printf("sv[%d]: GET %s: not modified since req\n", getpid(), destpath);
			cleanup();
			return 0;
		}
	}

	/*
	 * at this point we send an OK header and then the file data if the user wants it
	 */

	printf("sv[%d]: ", getpid());
	dual_write(s_client, header_ok, strlen(header_ok));
	printf("\n");

	if (!strcmp(method, "HEAD")) {
		fclose(f);
		cleanup();
		return 0;
	} else if (strcmp(method, "GET")) {
		return fail("sv[%d]: invalid method %s\n", getpid(), method);
	}

	printf("sv[%d]: sending data from %s\n", getpid(), destpath);

	write(s_client, "\r\n", 2); /* write an extra newline to signal the body */

	char rbuf[BS_SEND];
	int rv;

	while (1) {
		memset(rbuf, 0, sizeof rbuf);
		rv = fread(rbuf, 1, sizeof rbuf, f);

		if (!rv) break;

		write(s_client, rbuf, rv);

		if (rv < BS_SEND) break;
	}

	fclose(f);
	cleanup();
	return 0;
}

void sig(int s) {
	if (is_child) {
		printf("sv[%d]: caught signal, stopping\n", getpid());
		cleanup();
		exit(1);
	}

	printf("sv: caught signal\n");
	cleanup();

	printf("sv: waiting for children\n");
	wait(NULL);

	printf("sv: terminating cleanly\n");
	exit(1);
}

ssize_t dual_write(int s, const void* data, size_t size) {
	/* function to intercept and output socket writes to stdout for verbosity */
	char dc[size];
	memcpy(dc, data, size);

	/* get rid of nasty dos newlines */
	if (dc[size - 2] == '\r') {
		dc[size - 2] = 0;
		dc[size - 1] = 0;
	}

	printf("%s", dc);
	return write(s, data, size);
}
