#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT 9000
#define MAX_LINE 16384
#define READ_CHUNK 4096

typedef struct {
    char data[READ_CHUNK];
    size_t pos;
    size_t len;
} Reader;

typedef struct {
    const char *cursor;
    int error;
    int div_zero;
    int depth;
} Parser;

static int parse_port(const char *s) {
    char *end;
    errno = 0;
    long port = strtol(s, &end, 10);
    if (errno != 0 || *s == '\0' || *end != '\0' || port < 1 || port > 65535) {
        return -1;
    }
    return (int)port;
}

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int read_line(int fd, Reader *reader, char *out) {
    size_t used = 0;
    int too_long = 0;
    for (;;) {
        if (reader->pos == reader->len) {
            ssize_t n = recv(fd, reader->data, sizeof reader->data, 0);
            if (n == 0) {
                if (used != 0 || too_long) {
                    return -2;
                }
                return 0;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            reader->pos = 0;
            reader->len = (size_t)n;
        }
        unsigned char ch = (unsigned char)reader->data[reader->pos++];
        if (ch == '\n') {
            out[used] = '\0';
            return too_long ? -3 : 1;
        }
        if (!too_long) {
            if (used == MAX_LINE || ch == '\0') {
                too_long = 1;
            } else {
                out[used++] = (char)ch;
            }
        }
    }
}

static void skip_spaces(Parser *p) {
    while (*p->cursor == ' ' || *p->cursor == '\t' || *p->cursor == '\r') {
        p->cursor++;
    }
}

static double parse_expression(Parser *p);

static double parse_factor(Parser *p) {
    if (++p->depth > 128) {
        p->error = 1;
        p->depth--;
        return 0;
    }
    skip_spaces(p);
    double value = 0;
    if (*p->cursor == '+' || *p->cursor == '-') {
        int negative = *p->cursor == '-';
        p->cursor++;
        value = parse_factor(p);
        if (negative) {
            value = -value;
        }
    } else if (*p->cursor == '(') {
        p->cursor++;
        value = parse_expression(p);
        skip_spaces(p);
        if (*p->cursor != ')') {
            p->error = 1;
        } else {
            p->cursor++;
        }
    } else if (isdigit((unsigned char)*p->cursor) || *p->cursor == '.') {
        char *end;
        errno = 0;
        value = strtod(p->cursor, &end);
        if (end == p->cursor || errno == ERANGE || !isfinite(value)) {
            p->error = 1;
        } else {
            p->cursor = end;
        }
    } else {
        p->error = 1;
    }
    p->depth--;
    return value;
}

static double parse_term(Parser *p) {
    double value = parse_factor(p);
    while (!p->error) {
        skip_spaces(p);
        char operator = *p->cursor;
        if (operator != '*' && operator != '/') {
            break;
        }
        p->cursor++;
        double next = parse_factor(p);
        if (p->error) {
            break;
        }
        if (operator == '/' && next == 0) {
            p->div_zero = 1;
            p->error = 1;
            break;
        }
        value = operator == '*' ? value * next : value / next;
        if (!isfinite(value)) {
            p->error = 1;
        }
    }
    return value;
}

static double parse_expression(Parser *p) {
    double value = parse_term(p);
    while (!p->error) {
        skip_spaces(p);
        char operator = *p->cursor;
        if (operator != '+' && operator != '-') {
            break;
        }
        p->cursor++;
        double next = parse_term(p);
        if (p->error) {
            break;
        }
        value = operator == '+' ? value + next : value - next;
        if (!isfinite(value)) {
            p->error = 1;
        }
    }
    return value;
}

static void calculate(const char *expression, char *response, size_t capacity) {
    Parser parser = {expression, 0, 0, 0};
    double answer = parse_expression(&parser);
    skip_spaces(&parser);
    if (*parser.cursor != '\0') {
        parser.error = 1;
    }
    if (parser.div_zero) {
        snprintf(response, capacity, "ERR division_by_zero\n");
    } else if (parser.error || !isfinite(answer)) {
        snprintf(response, capacity, "ERR invalid_expression\n");
    } else {
        snprintf(response, capacity, "OK %.15g\n", answer);
    }
}

static void serve_client(int fd) {
    Reader reader = {{0}, 0, 0};
    char request[MAX_LINE + 1];
    char response[128];
    for (;;) {
        int status = read_line(fd, &reader, request);
        if (status == 0) {
            break;
        }
        if (status == -1) {
            perror("recv");
            break;
        }
        if (status == -2) {
            fprintf(stderr, "Соединение закрыто посреди сообщения\n");
            break;
        }
        if (status == -3) {
            snprintf(response, sizeof response, "ERR too_long\n");
        } else {
            calculate(request, response, sizeof response);
            printf("Запрос: %s | Ответ: %s", request, response);
            fflush(stdout);
        }
        if (send_all(fd, response, strlen(response)) != 0) {
            perror("send");
            break;
        }
    }
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 2 || (argc == 2 && (port = parse_port(argv[1])) == -1)) {
        fprintf(stderr, "Использование: %s [порт]\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return 1;
    }
    int yes = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 16) == -1) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    printf("Калькулятор слушает 127.0.0.1:%d\n", port);
    fflush(stdout);
    for (;;) {
        struct sockaddr_in peer = {0};
        socklen_t peer_size = sizeof peer;
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_size);
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip) != NULL) {
            printf("Подключение: %s:%u\n", ip, ntohs(peer.sin_port));
            fflush(stdout);
        }
        serve_client(client_fd);
        close(client_fd);
    }
    close(listen_fd);
    return 1;
}
