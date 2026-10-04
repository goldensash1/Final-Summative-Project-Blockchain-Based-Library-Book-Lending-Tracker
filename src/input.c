#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <termios.h>
#include <unistd.h>

#include "input.h"

static int g_eof = 0;

int input_eof(void) {
    return g_eof;
}

int read_line(const char *prompt, char *buf, size_t size) {
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }

    if (fgets(buf, (int)size, stdin) == NULL) {
        buf[0] = '\0';
        g_eof = 1;
        printf("\n");
        return INPUT_EOF;
    }

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
        return INPUT_OK;
    }

    /* No newline: either the line was longer than the buffer, or the last
     * line of piped input had no trailing newline. */
    int c, overflow = 0;
    while ((c = getchar()) != '\n' && c != EOF) overflow = 1;
    if (!overflow) return INPUT_OK;

    buf[0] = '\0';
    printf("ERROR: Input too long (max %zu characters)\n", size - 1);
    return INPUT_TOO_LONG;
}

int read_password(const char *prompt, char *buf, size_t size) {
    if (!isatty(STDIN_FILENO)) return read_line(prompt, buf, size);

    struct termios old_attr, new_attr;
    if (tcgetattr(STDIN_FILENO, &old_attr) != 0) return read_line(prompt, buf, size);

    new_attr = old_attr;
    new_attr.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_attr);

    int rc = read_line(prompt, buf, size);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_attr);
    if (rc != INPUT_EOF) printf("\n");
    return rc;
}

int parse_int(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || v < INT_MIN || v > INT_MAX) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = (int)v;
    return 1;
}

void to_upper_trim(char *s) {
    int j = 0;
    for (int i = 0; s[i]; i++) {
        if (!isspace((unsigned char)s[i])) s[j++] = (char)toupper((unsigned char)s[i]);
    }
    s[j] = '\0';
}

void trim_spaces(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    size_t len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
    memmove(s, start, len);
    s[len] = '\0';
}

int prompt_id(const char *prompt, char *buf, size_t size) {
    if (read_line(prompt, buf, size) != INPUT_OK) return 0;
    to_upper_trim(buf);
    return 1;
}

int prompt_int_range(const char *prompt, int min, int max, int default_value, int *out) {
    for (;;) {
        char buf[32];
        int rc = read_line(prompt, buf, sizeof(buf));
        if (rc == INPUT_EOF) return 0;

        int value;
        if (rc == INPUT_OK && buf[0] == '\0' && default_value >= min && default_value <= max) {
            *out = default_value;
            return 1;
        }
        if (rc == INPUT_OK && parse_int(buf, &value) && value >= min && value <= max) {
            *out = value;
            return 1;
        }
        printf("ERROR: Please enter a number from %d to %d.\n", min, max);
    }
}
