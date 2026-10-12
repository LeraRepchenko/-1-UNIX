#define _GNU_SOURCE
/*
 * Лабораторная 1. Продвинутый уровень. Вариант 15.
 * Логирование операций в файл.
 *
 * Использование: ./lab1 <domain> <type> <ip> <port> [--bind]
 * Компиляция:    gcc -std=c11 -Wall -Wextra -o lab1 lab1.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static FILE *g_log = NULL;
static int   g_sockfd = -1;

/* Запись в лог и на экран */
static void log_msg(const char *level, const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));

    va_list args;

    printf("[%s] %-5s ", ts, level);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");

    if (g_log) {
        fprintf(g_log, "[%s] %-5s ", ts, level);
        va_start(args, fmt);
        vfprintf(g_log, fmt, args);
        va_end(args);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
}

/* Вывод открытых FD из /proc/self/fd */
static void dump_fds(const char *label)
{
    log_msg("INFO", "--- Открытые FD (%s) ---", label);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ls /proc/%d/fd/", getpid());
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char line[64];
    while (fgets(line, sizeof(line), p)) {
        size_t n = strlen(line);
        if (n && line[n-1] == '\n') line[n-1] = '\0';
        log_msg("FD", "  %s", line);
    }
    pclose(p);
}

/* Закрытие сокета и лога, выход с кодом */
static void cleanup_and_exit(int code)
{
    if (g_sockfd >= 0) {
        if (close(g_sockfd) == 0)
            log_msg("INFO", "close() успешно. FD=%d освобождён.", g_sockfd);
        else
            log_msg("ERROR", "close() ошибка: %s", strerror(errno));
        g_sockfd = -1;
    }
    if (g_log) {
        log_msg("INFO", "===== Завершение =====");
        fclose(g_log);
        g_log = NULL;
    }
    exit(code);
}

/* Обработчик Ctrl+C */
static void on_sigint(int sig)
{
    (void)sig;
    log_msg("WARN", "Получен SIGINT. Завершение...");
    cleanup_and_exit(EXIT_FAILURE);
}

/* ---- Парсеры аргументов ---- */

static int parse_domain(const char *s, int *out)
{
    if (!strcmp(s, "AF_INET"))  { *out = AF_INET;  return 0; }
    if (!strcmp(s, "AF_INET6")) { *out = AF_INET6; return 0; }
    if (!strcmp(s, "AF_UNIX"))  { *out = AF_UNIX;  return 0; }
    return -1;
}

static int parse_type(const char *s, int *out)
{
    if (!strcmp(s, "SOCK_STREAM")) { *out = SOCK_STREAM; return 0; }
    if (!strcmp(s, "SOCK_DGRAM"))  { *out = SOCK_DGRAM;  return 0; }
    if (!strcmp(s, "SOCK_RAW"))    { *out = SOCK_RAW;    return 0; }
    return -1;
}

static int parse_port(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0') return -1;
    if (v < 0 || v > 65535) return -1;
    *out = (int)v;
    return 0;
}

/* ============================= MAIN ============================= */

int main(int argc, char *argv[])
{
    /* Открываем лог */
    g_log = fopen("lab1.log", "a");
    if (!g_log) {
        fprintf(stderr, "Не удалось открыть lab1.log: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    log_msg("INFO", "===== Запуск =====");

    /* SIGINT */
    signal(SIGINT, on_sigint);

    /* Проверка количества аргументов */
    if (argc < 5) {
        log_msg("ERROR", "Использование: %s <domain> <type> <ip> <port> [--bind]", argv[0]);
        cleanup_and_exit(EXIT_FAILURE);
    }

    const char *domain_s = argv[1];
    const char *type_s   = argv[2];
    const char *ip_s     = argv[3];
    const char *port_s   = argv[4];
    int do_bind = (argc >= 6 && !strcmp(argv[5], "--bind"));

    if (argc >= 6 && strcmp(argv[5], "--bind") != 0) {
        log_msg("ERROR", "Неизвестный флаг '%s'", argv[5]);
        cleanup_and_exit(EXIT_FAILURE);
    }

    log_msg("INFO", "Аргументы: domain=%s type=%s ip=%s port=%s bind=%s",
            domain_s, type_s, ip_s, port_s, do_bind ? "yes" : "no");

    /* --- Парсинг domain --- */
    int domain;
    if (parse_domain(domain_s, &domain) != 0) {
        log_msg("ERROR", "Неверный domain '%s'. Допустимо: AF_INET, AF_INET6, AF_UNIX", domain_s);
        cleanup_and_exit(EXIT_FAILURE);
    }

    /* --- Парсинг type --- */
    int type;
    if (parse_type(type_s, &type) != 0) {
        log_msg("ERROR", "Неверный type '%s'. Допустимо: SOCK_STREAM, SOCK_DGRAM, SOCK_RAW", type_s);
        cleanup_and_exit(EXIT_FAILURE);
    }

    /* --- Парсинг порта --- */
    int port;
    if (parse_port(port_s, &port) != 0) {
        log_msg("ERROR", "Неверный порт '%s' (нужно 0..65535)", port_s);
        cleanup_and_exit(EXIT_FAILURE);
    }

    /* --- Проверка IP через inet_pton --- */
    struct in_addr tmp;
    int r = inet_pton(AF_INET, ip_s, &tmp);
    if (r == 0) {
        log_msg("ERROR", "inet_pton: неверный формат IP '%s' (вернул 0)", ip_s);
        cleanup_and_exit(EXIT_FAILURE);
    }
    if (r < 0) {
        log_msg("ERROR", "inet_pton ошибка: %s", strerror(errno));
        cleanup_and_exit(EXIT_FAILURE);
    }
    log_msg("INFO", "inet_pton: IP '%s' распознан", ip_s);

    dump_fds("до socket()");

    /* ===== ШАГ 1: socket() ===== */
    g_sockfd = socket(domain, type, 0);
    if (g_sockfd < 0) {
        log_msg("ERROR", "socket(): errno=%d (%s)", errno, strerror(errno));
        cleanup_and_exit(EXIT_FAILURE);
    }
    log_msg("INFO", "socket() создан. FD = %d", g_sockfd);

    /* FD должен быть >= 3 */
    if (g_sockfd < 3)
        log_msg("WARN", "Неожиданно низкий FD = %d (< 3)", g_sockfd);
    else
        log_msg("INFO", "Проверка FD >= 3: OK (%d)", g_sockfd);

    /* Какой протокол выбрало ядро */
    int proto = 0; socklen_t plen = sizeof(proto);
    if (getsockopt(g_sockfd, SOL_SOCKET, SO_PROTOCOL, &proto, &plen) == 0)
        log_msg("INFO", "Протокол: %s (%d)",
                proto == IPPROTO_TCP ? "TCP" : proto == IPPROTO_UDP ? "UDP" : "OTHER", proto);

    /* Тип сокета */
    int st = 0; socklen_t stl = sizeof(st);
    if (getsockopt(g_sockfd, SOL_SOCKET, SO_TYPE, &st, &stl) == 0)
        log_msg("INFO", "SO_TYPE = %d (%s)", st,
                st == SOCK_STREAM ? "SOCK_STREAM" :
                st == SOCK_DGRAM  ? "SOCK_DGRAM"  : "OTHER");

    dump_fds("после socket()");

    /* ===== ШАГ 2: заполнение адреса ===== */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip_s, &addr.sin_addr) <= 0) {
        log_msg("ERROR", "inet_pton: %s", strerror(errno));
        cleanup_and_exit(EXIT_FAILURE);
    }

    char ip_str[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str))) {
        log_msg("ERROR", "inet_ntop: %s", strerror(errno));
        cleanup_and_exit(EXIT_FAILURE);
    }
    log_msg("INFO", "Адрес заполнен: %s:%d", ip_str, ntohs(addr.sin_port));
    log_msg("INFO", "htons(%d)=0x%04X -> ntohs=%d", port, ntohs(addr.sin_port), ntohs(addr.sin_port));

    /* ===== ШАГ 3: bind() ===== */
    if (do_bind) {
        dump_fds("перед bind()");
        if (bind(g_sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            log_msg("ERROR", "bind(): errno=%d (%s)", errno, strerror(errno));
            if (errno == EADDRINUSE)
                log_msg("ERROR", "Порт %d занят (EADDRINUSE)", ntohs(addr.sin_port));
            else if (errno == EACCES)
                log_msg("ERROR", "Нет прав на порт %d (< 1024)", ntohs(addr.sin_port));
            else if (errno == EADDRNOTAVAIL)
                log_msg("ERROR", "IP %s не принадлежит машине", ip_str);
            cleanup_and_exit(EXIT_FAILURE);
        }
        log_msg("INFO", "bind() OK: %s:%d", ip_str, ntohs(addr.sin_port));

        /* Порт 0 -> узнаём выбранный порт */
        if (port == 0) {
            struct sockaddr_in act; socklen_t al = sizeof(act);
            if (getsockname(g_sockfd, (struct sockaddr *)&act, &al) == 0)
                log_msg("INFO", "Порт 0 -> ядро выбрало %d", ntohs(act.sin_port));
        }
    } else {
        log_msg("INFO", "Флаг --bind не указан, bind() пропущен");
    }

    dump_fds("после bind()");

    /* ===== ШАГ 4: информация ===== */
    log_msg("INFO", "--- Информация о сокете ---");
    log_msg("INFO", "FD         : %d", g_sockfd);
    log_msg("INFO", "Семейство  : AF_INET (%d)", addr.sin_family);
    log_msg("INFO", "Порт(сеть) : 0x%04X", ntohs(addr.sin_port));
    log_msg("INFO", "Порт(host) : %d", ntohs(addr.sin_port));
    log_msg("INFO", "IP         : %s", ip_str);
    log_msg("INFO", "bind       : %s", do_bind ? "да" : "нет");

    /* ===== ШАГ 5: close() ===== */
    if (close(g_sockfd) != 0) {
        log_msg("ERROR", "close() ошибка: %s", strerror(errno));
        g_sockfd = -1;
        cleanup_and_exit(EXIT_FAILURE);
    }
    log_msg("INFO", "close() успешно. FD=%d освобождён.", g_sockfd);
    g_sockfd = -1;

    dump_fds("после close()");

    log_msg("INFO", "Программа успешно завершена.");
    cleanup_and_exit(EXIT_SUCCESS);
}