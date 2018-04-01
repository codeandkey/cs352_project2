#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#define RESPONSE_OUT "response"
#define RESPONSE_CHUNKLEN 512
#define HOSTNAME_MAXLEN 64

int help(void);
int fail(const char* msg, ...);

int main(int argc, char** argv) {
	int header_only = 0, opt, s_remote;
	char* ims_str = NULL, *url = NULL, *datestr = NULL, *port = NULL;

	if (argc < 2) return help();

	/* parse command line arguments, relatively straightforward */
	while ((opt = getopt(argc, argv, "hd:")) != -1) {
		switch (opt) {
		case 'h':
			header_only = 1;
			break;
		case 'd':
			ims_str = optarg;
			break;
		default:
			return help();
		}
	}

	if (optind >= argc) return help();
	url = argv[optind];

	/*
	 * arguments and flags ready, we parse the URL
	 * nasty strtok trick incoming
	 */

	char* proto = strtok(url, ":/"); /* flexible parsing allows mangled URLs to still work */
	char* host = strtok(NULL, "/");
	char* resource = strtok(NULL, "/"); /* note: missing the leading '/' */

	resource = resource ?: ""; /* strtok ignores the last delim so we have to make sure resource is still OK */

	if (!proto || !host) {
		printf("cl: invalid URL\n");
		return 1;
	}

	/* we need to construct a datestring if we're using that too. */
	if (ims_str) {
		time_t target = time(NULL);

		char* daystr, *hrstr, *mnstr;
		int day, hr, mn;

		daystr = strtok(ims_str, ":");
		hrstr = strtok(NULL, ":");
		mnstr = strtok(NULL, ":");

		day = strtol(daystr, NULL, 10);
		hr = strtol(hrstr, NULL, 10);
		mn = strtol(mnstr, NULL, 10);

		target -= day * 86400;
		target -= hr * 3600;
		target -= mn * 60;

		/* we send over a time in UTC */
		datestr = asctime(gmtime(&target));

		if (datestr[strlen(datestr) - 1] == '\n') datestr[strlen(datestr) - 1] = 0;
	}

	port = NULL;

	/* only support http */
	if (!strcmp(proto, "http")) {
		port = "80";
	}

	if (!port) return fail("cl: unsupported protocol %s\n", proto);

	strtok(host, ":"); /* parse a port number out of the hostname */
	char* preport = strtok(NULL, ":");

	if (preport) {
		port = preport; /* hostname will be automatically shortened by strtok() */
	}

	printf("cl: connecting to [%s], proto [%s], resource [/%s], port [%s]\n", host, proto, resource, port);

	struct addrinfo* remote_addr = {0}, remote_hints = {0}, *pinfo = remote_addr;

	remote_hints.ai_flags = AI_PASSIVE;
	remote_hints.ai_family = AF_INET;
	remote_hints.ai_socktype = SOCK_STREAM;
	remote_hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(host, port, (struct addrinfo*) &remote_hints, (struct addrinfo**) &remote_addr)) {
		return fail("cl: host resolution failed\n");
	}

	for (pinfo = remote_addr; pinfo; pinfo = pinfo->ai_next) {
		s_remote = socket(remote_addr->ai_family, remote_addr->ai_socktype, remote_addr->ai_protocol);

		if (s_remote < 0) {
			printf("cl: failed to create socket\n");
			continue;
		}

		printf("cl: trying %s\n", inet_ntoa(((struct sockaddr_in*) remote_addr->ai_addr)->sin_addr));

		if (connect(s_remote, remote_addr->ai_addr, remote_addr->ai_addrlen)) {
			printf("cl: connect() failed (%s)\n", strerror(errno));
			continue;
		}

		break;
	}

	freeaddrinfo(remote_addr);

	if (!pinfo) {
		return fail("cl: connection failed\n");
	}

	printf("cl: connected to %s\n", host);

	/*
	 * connection is good! we send over the request.
	 */

	/* here we subtly insert the '/' before the resource */
	char method_get[] = "GET /";
	char method_head[] = "HEAD /";

	char* preheader = method_get, *postheader = " HTTP/1.1\r\n";
	if (header_only) preheader = method_head;

	write(s_remote, preheader, strlen(preheader));
	write(s_remote, resource, strlen(resource));
	write(s_remote, postheader, strlen(postheader));

	/*
	 * now, send any extra params. first the HTTP Host..
	 */

	const char* hp_pre = "Host: ", *hp_post = "\r\n";

	write(s_remote, hp_pre, strlen(hp_pre));
	write(s_remote, host, strlen(host));
	write(s_remote, hp_post, strlen(hp_post));

	/* indicate that we DON'T want a keep-alive connection as is the default,
	 * this makes it significantly more clean to know when we're done
	 * reading the server response */

	const char* cc_header = "Connection: close\r\n";
	write(s_remote, cc_header, strlen(cc_header));

	/* send over an If-Modified-Since header if we need to */
	if (ims_str) {
		const char* ims_pre = "If-Modified-Since: ", *ims_post = " GMT\r\n";
		write(s_remote, ims_pre, strlen(ims_pre));
		write(s_remote, datestr, strlen(datestr));
		write(s_remote, ims_post, strlen(ims_post));
	}

	/* write an extra CRLF to terminate the request */
	write(s_remote, hp_post, strlen(hp_post));

	preheader[strlen(preheader) - 2] = 0;
	printf("cl: sent %s request\n", preheader);

	/*
	 * request sent over, now we wait for a response.
	 * we might get a bit of the file read while trying to read the header, so
	 *   we have to be prepared to write a little extra bit to the file
	 */

	FILE* f = fopen(RESPONSE_OUT, "w");

	if (!f) {
		close(s_remote);
		return fail("cl: failed to open response file [%s] for writing\n", RESPONSE_OUT);
	}

	char rbuf[RESPONSE_CHUNKLEN] = {0}, rbuf2[RESPONSE_CHUNKLEN];
	int rlen = 0;

	while (1) {
		int res = read(s_remote, rbuf, sizeof rbuf);

		if (!rlen) {
			/* we don't want strtok to mutate the original chunk so we just copy it. */
			memcpy(rbuf2, rbuf, sizeof rbuf);

			char* headerline = strtok(rbuf2, "\r\n");
			strtok(headerline, " ");
			char* code = strtok(NULL, " ");
			char* msg = strtok(NULL, "\r\n"); /* we know we're not going to find it, grabs the rest of the line. */

			printf("cl: received response %s : %s\n", code, msg);
		}

		printf("cl: receiving data.. (%d)     \r", rlen);

		if (!res) break;
		if (res < 0) {
			printf("cl: read fail\n");
			break;
		}

		/*
		 * write what we have in the buffer to the file
		 */

		fwrite(rbuf, 1, res, f);
		rlen += res;
	}

	printf("\ncl: wrote %d bytes to %s\n", rlen, RESPONSE_OUT);

	fclose(f);
	return 0;
}

int help(void) {
	printf("usage: client [-h] [-d D:H:M] <URL>\n");
	return 1;
}

int fail(const char* msg, ...) {
	va_list args;
	va_start(args, msg);

	vprintf(msg, args);

	va_end(args);
	return -1;
}
