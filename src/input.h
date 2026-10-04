#ifndef INPUT_H
#define INPUT_H

#include <stddef.h>

#define INPUT_OK        1
#define INPUT_EOF       0
#define INPUT_TOO_LONG -1

/* Prints `prompt`, then reads one line from stdin into `buf` (newline
 * stripped). Returns INPUT_OK, INPUT_EOF when stdin is closed, or
 * INPUT_TOO_LONG when the line did not fit (the rest of the line is
 * discarded so it cannot leak into the next prompt, and buf is emptied). */
int read_line(const char *prompt, char *buf, size_t size);

/* Same as read_line, but disables terminal echo while typing when stdin is
 * an interactive terminal. */
int read_password(const char *prompt, char *buf, size_t size);

/* Returns 1 once stdin has reached end-of-file. */
int input_eof(void);

/* Strictly parses a base-10 int (optional surrounding whitespace).
 * Returns 1 on success, 0 if the text is empty, non-numeric or out of range. */
int parse_int(const char *s, int *out);

/* Removes all whitespace and upper-cases the string in place (IDs are
 * case-insensitive, e.g. " bk001 " -> "BK001"). */
void to_upper_trim(char *s);

/* Removes leading and trailing whitespace in place (case is kept). */
void trim_spaces(char *s);

/* Reads an ID and normalises it with to_upper_trim. Returns 0 if input was
 * aborted (EOF or too long). */
int prompt_id(const char *prompt, char *buf, size_t size);

/* Prompts until an integer in [min, max] is entered. Empty input selects
 * `default_value` when it is in range (pass min - 1 for "no default").
 * Returns 1 with *out set, or 0 on EOF. */
int prompt_int_range(const char *prompt, int min, int max, int default_value, int *out);

#endif
