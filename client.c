#include <arpa/inet.h>
#include <errno.h>
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

static void discard_stdin_line(void) {
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {
    }
}

int main(int argc, char **argv) {
    const char *host = argc >= 2 ? argv[1] : "127.0.0.1";
    int port = DEFAULT_PORT;
    if (argc > 3 || (argc == 3 && (port = parse_port(argv[2])) == -1)) {
        fprintf(stderr, "Использование: %s [IPv4-адрес [порт]]\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Некорректный IPv4-адрес: %s\n", host);
        close(fd);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
        if (errno == ECONNREFUSED) {
            fprintf(stderr, "Соединение отклонено: сервер %s:%d не принимает подключения\n", host, port);
        } else {
            perror("connect");
        }
        close(fd);
        return 1;
    }
    Reader reader = {{0}, 0, 0};
    char input[MAX_LINE + 2];
    char reply[MAX_LINE + 1];
    int result = 0;
    for (;;) {
        printf("Введите выражение (exit для выхода): ");
        fflush(stdout);
        if (fgets(input, sizeof input, stdin) == NULL) {
            break;
        }
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] != '\n' && !feof(stdin)) {
            if (len >= MAX_LINE + 1) {
                discard_stdin_line();
                fprintf(stderr, "Слишком длинное выражение\n");
                continue;
            }
        }
        if (len > 0 && input[len - 1] == '\n') {
            input[--len] = '\0';
        }
        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
            break;
        }
        if (len > MAX_LINE) {
            fprintf(stderr, "Слишком длинное выражение\n");
            continue;
        }
        input[len++] = '\n';
        if (send_all(fd, input, len) == -1) {
            perror("send");
            result = 1;
            break;
        }
        int status = read_line(fd, &reader, reply);
        if (status == 0) {
            fprintf(stderr, "Сервер закрыл соединение без ответа\n");
            result = 1;
            break;
        }
        if (status == -1) {
            if (errno == ECONNRESET) {
                fprintf(stderr, "Соединение сброшено сервером (ECONNRESET)\n");
            } else {
                perror("recv");
            }
            result = 1;
            break;
        }
        if (status == -2) {
            fprintf(stderr, "Сервер закрыл соединение посреди ответа\n");
            result = 1;
            break;
        }
        if (status == -3) {
            fprintf(stderr, "Ответ сервера слишком длинный\n");
            result = 1;
            break;
        }
        if (reply[0] == '\0') {
            fprintf(stderr, "Сервер отправил пустой ответ\n");
            result = 1;
            break;
        }
        if (strncmp(reply, "OK ", 3) == 0 || strncmp(reply, "ERR ", 4) == 0) {
            printf("Ответ сервера: %s\n", reply);
        } else {
            fprintf(stderr, "Неверный формат ответа сервера: %s\n", reply);
            result = 1;
            break;
        }
    }
    close(fd);
    return result;
}
