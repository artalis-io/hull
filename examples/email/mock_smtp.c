/*
 * mock_smtp.c — Standalone mock SMTP server for e2e testing
 *
 * Usage: mock_smtp <port_file> <data_file>
 *
 * 1. Binds to an ephemeral port, writes the port number to <port_file>
 * 2. Accepts one connection, runs SMTP conversation
 * 3. Writes captured DATA payload to <data_file>
 * 4. Exits after the connection closes
 *
 * No threading — single connection, foreground process.
 * Start in background (&) and read port_file to discover the port.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>

/* Read one CRLF-terminated line from the client */
static int read_line(int fd, char *buf, int size)
{
    int pos = 0;
    while (pos < size - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0)
            return -1;
        pos++;
        if (pos >= 2 && buf[pos - 2] == '\r' && buf[pos - 1] == '\n') {
            buf[pos] = '\0';
            return pos;
        }
    }
    buf[pos] = '\0';
    return pos;
}

static int send_line(int fd, const char *s)
{
    size_t len = strlen(s);
    ssize_t w = write(fd, s, len);
    return (w == (ssize_t)len) ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port_file> <data_file>\n", argv[0]);
        return 1;
    }

    const char *port_file = argv[1];
    const char *data_file = argv[2];

    /* Bind to ephemeral port */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    socklen_t slen = sizeof(addr);
    if (getsockname(listen_fd, (struct sockaddr *)&addr, &slen) < 0) {
        perror("getsockname");
        close(listen_fd);
        return 1;
    }
    int port = ntohs(addr.sin_port);

    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    /* Write port to file so the test script can discover it */
    FILE *pf = fopen(port_file, "w");
    if (!pf) {
        perror("fopen port_file");
        close(listen_fd);
        return 1;
    }
    fprintf(pf, "%d", port);
    fclose(pf);

    /* Accept one connection */
    int client = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (client < 0) {
        perror("accept");
        return 1;
    }

    /* DATA capture buffer */
    char data_buf[16384];
    int  data_len = 0;

    /* 220 greeting */
    send_line(client, "220 mock.local ESMTP\r\n");

    char line[1024];
    int  in_data = 0;

    while (read_line(client, line, (int)sizeof(line)) > 0) {
        if (in_data) {
            if (strcmp(line, ".\r\n") == 0) {
                in_data = 0;
                send_line(client, "250 OK\r\n");
                continue;
            }
            int ll = (int)strlen(line);
            if (data_len + ll < (int)sizeof(data_buf) - 1) {
                memcpy(data_buf + data_len, line, (size_t)ll);
                data_len += ll;
                data_buf[data_len] = '\0';
            }
            continue;
        }

        if (strncasecmp(line, "EHLO ", 5) == 0) {
            send_line(client, "250 mock.local\r\n");
        }
        else if (strncasecmp(line, "AUTH PLAIN ", 11) == 0) {
            send_line(client, "235 OK\r\n");
        }
        else if (strncasecmp(line, "MAIL FROM:", 10) == 0) {
            send_line(client, "250 OK\r\n");
        }
        else if (strncasecmp(line, "RCPT TO:", 8) == 0) {
            send_line(client, "250 OK\r\n");
        }
        else if (strncasecmp(line, "DATA", 4) == 0) {
            in_data = 1;
            send_line(client, "354 Go\r\n");
        }
        else if (strncasecmp(line, "QUIT", 4) == 0) {
            send_line(client, "221 Bye\r\n");
            break;
        }
        else {
            /* Unknown command — respond so client doesn't hang */
            send_line(client, "500 Unrecognized\r\n");
        }
    }

    close(client);

    /* Write captured DATA to file */
    FILE *df = fopen(data_file, "w");
    if (!df) {
        perror("fopen data_file");
        return 1;
    }
    fwrite(data_buf, 1, (size_t)data_len, df);
    fclose(df);

    return 0;
}
