/* kronk.c -- the KRONK language interpreter
 *
 * There's no compiler running inside this OS, so "write code in-OS
 * and run it" needed a different approach: an interpreted language,
 * executed directly with no build step. This is a classic
 * line-numbered BASIC-family subset -- LET, PRINT, IF/THEN, GOTO,
 * INPUT, END, REM, and one language-specific addition: KRONK, a
 * statement that does something deliberately useless and destructive
 * (see below). Integer arithmetic only, 26 variables (A-Z). Write a
 * program with the existing `edit` command, then run it with
 * `kronk <file>`.
 *
 * Design choice: each line's statement text is re-parsed from scratch
 * every time it executes (no pre-built AST). Simpler to implement
 * correctly, and these programs are small enough that the extra
 * parsing work per line is irrelevant.
 */

#include "kronk.h"
#include "vga.h"
#include "keyboard.h"
#include "fat16.h"
#include "heap.h"

#define MAX_LINES 300
#define MAX_LINE_LEN 100

struct basic_line {
    int number;
    char text[MAX_LINE_LEN];
};

static struct basic_line program[MAX_LINES];
static int program_count;
static int variables[26];

static int had_error;
static int error_line;

static void print_int(int n) {
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    unsigned int u;
    int neg = 0;
    if (n < 0) { neg = 1; u = (unsigned int)(-n); } else { u = (unsigned int)n; }
    if (u == 0) { buf[--i] = '0'; }
    while (u > 0) { buf[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[--i] = '-';
    vga_write(&buf[i]);
}

static void skip_spaces(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_letter(char c) { return is_upper(c) || is_lower(c); }
static int var_index(char c) { return is_lower(c) ? (c - 'a') : (c - 'A'); }
static char upper_char(char c) { return is_lower(c) ? (char)(c - 'a' + 'A') : c; }

static int parse_expr(const char **p);

static int parse_number(const char **p) {
    int neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    int val = 0;
    while (is_digit(**p)) { val = val * 10 + (**p - '0'); (*p)++; }
    return neg ? -val : val;
}

static int parse_factor(const char **p) {
    skip_spaces(p);
    if (**p == '(') {
        (*p)++;
        int v = parse_expr(p);
        skip_spaces(p);
        if (**p == ')') (*p)++;
        return v;
    }
    if (**p == '-') {
        (*p)++;
        return -parse_factor(p);
    }
    if (is_digit(**p)) {
        return parse_number(p);
    }
    if (is_letter(**p)) {
        int idx = var_index(**p);
        (*p)++;
        return variables[idx];
    }
    had_error = 1;
    return 0;
}

static int parse_term(const char **p) {
    int v = parse_factor(p);
    for (;;) {
        skip_spaces(p);
        if (**p == '*') { (*p)++; v *= parse_factor(p); }
        else if (**p == '/') {
            (*p)++;
            int rhs = parse_factor(p);
            if (rhs == 0) { had_error = 1; return 0; }
            v /= rhs;
        } else break;
    }
    return v;
}

static int parse_expr(const char **p) {
    skip_spaces(p);
    int v = parse_term(p);
    for (;;) {
        skip_spaces(p);
        if (**p == '+') { (*p)++; v += parse_term(p); }
        else if (**p == '-') { (*p)++; v -= parse_term(p); }
        else break;
    }
    return v;
}

/* Parses "expr RELOP expr" and returns 1/0. Assumes p points just
 * after any leading keyword (e.g. "IF ") with the condition next. */
static int parse_condition(const char **p) {
    int lhs = parse_expr(p);
    skip_spaces(p);
    char op1 = **p;
    char op2 = *(*p + 1);
    int result = 0;
    if (op1 == '<' && op2 == '=') { *p += 2; result = (lhs <= parse_expr(p)); }
    else if (op1 == '>' && op2 == '=') { *p += 2; result = (lhs >= parse_expr(p)); }
    else if (op1 == '<' && op2 == '>') { *p += 2; result = (lhs != parse_expr(p)); }
    else if (op1 == '<') { (*p)++; result = (lhs < parse_expr(p)); }
    else if (op1 == '>') { (*p)++; result = (lhs > parse_expr(p)); }
    else if (op1 == '=') { (*p)++; result = (lhs == parse_expr(p)); }
    else { had_error = 1; return 0; }
    return result;
}

static int str_starts_with(const char *s, const char *kw) {
    while (*kw) {
        if (upper_char(*s) != *kw) return 0;
        s++; kw++;
    }
    return 1;
}

static int find_line_index(int number) {
    for (int i = 0; i < program_count; i++)
        if (program[i].number == number) return i;
    return -1;
}

/* Executes one line. Returns:
 *   -1  -> advance to the next line normally
 *   -2  -> stop (END, or an error already reported)
 *   >=0 -> jump to this array index (GOTO / IF-THEN) */
static int exec_line(int idx) {
    const char *p = program[idx].text;
    skip_spaces(&p);

    if (str_starts_with(p, "REM")) return -1;
    if (str_starts_with(p, "END")) return -2;

    if (str_starts_with(p, "LET")) {
        p += 3;
        skip_spaces(&p);
        if (!is_letter(*p)) { had_error = 1; return -2; }
        int var = var_index(*p);
        p++;
        skip_spaces(&p);
        if (*p != '=') { had_error = 1; return -2; }
        p++;
        variables[var] = parse_expr(&p);
        return -1;
    }

    /* KRONK -- pulls the lever. Turns out it was the wrong one: does
     * nothing useful and wipes every variable back to zero, undoing
     * whatever the program had carefully computed so far. Any text
     * after the keyword (leftover from an older program, or just a
     * comment-like note) is deliberately ignored rather than
     * rejected -- consistent with the statement's whole point: it
     * doesn't actually pay attention to anything, it just breaks
     * stuff. */
    if (str_starts_with(p, "KRONK")) {
        for (int i = 0; i < 26; i++) variables[i] = 0;
        vga_write("KRONK! WRONG LEVER. EVERY VARIABLE IS NOW ZERO. YOU'RE WELCOME.\n");
        return -1;
    }

    if (str_starts_with(p, "INPUT")) {
        p += 5;
        skip_spaces(&p);
        if (!is_letter(*p)) { had_error = 1; return -2; }
        int var = var_index(*p);
        vga_write("? ");
        char buf[32];
        int len = 0;
        for (;;) {
            char c = keyboard_getchar();
            if (c == '\n') { vga_putc('\n'); break; }
            if (c == '\b') { if (len > 0) { len--; vga_putc('\b'); } continue; }
            if (len < 31) { buf[len++] = c; vga_putc(c); }
        }
        buf[len] = '\0';
        const char *bp = buf;
        variables[var] = parse_number(&bp);
        return -1;
    }

    if (str_starts_with(p, "PRINT")) {
        p += 5;
        for (;;) {
            skip_spaces(&p);
            if (*p == '\0') break;
            if (*p == '"') {
                p++;
                while (*p && *p != '"') { vga_putc(*p); p++; }
                if (*p == '"') p++;
            } else {
                print_int(parse_expr(&p));
            }
            skip_spaces(&p);
            if (*p == ',') { p++; vga_putc(' '); continue; }
            break;
        }
        vga_putc('\n');
        return -1;
    }

    if (str_starts_with(p, "GOTO")) {
        p += 4;
        skip_spaces(&p);
        int target = parse_number(&p);
        int ti = find_line_index(target);
        if (ti < 0) { had_error = 1; error_line = program[idx].number; return -2; }
        return ti;
    }

    if (str_starts_with(p, "IF")) {
        p += 2;
        skip_spaces(&p);
        int cond = parse_condition(&p);
        skip_spaces(&p);
        if (!str_starts_with(p, "THEN")) { had_error = 1; return -2; }
        p += 4;
        skip_spaces(&p);
        if (str_starts_with(p, "GOTO")) { p += 4; skip_spaces(&p); }
        if (!cond) return -1;
        int target = parse_number(&p);
        int ti = find_line_index(target);
        if (ti < 0) { had_error = 1; error_line = program[idx].number; return -2; }
        return ti;
    }

    had_error = 1;
    return -2;
}

void kronk_run(const char *filename) {
    struct fat16_file f;
    if (!fat16_find_file(filename, &f)) {
        vga_write("kronk: file not found: ");
        vga_write(filename);
        vga_write("\n");
        return;
    }

    unsigned char *buf = (unsigned char *)kmalloc(f.file_size + 1);
    if (!fat16_read_file(&f, buf, f.file_size)) {
        vga_write("kronk: read failed\n");
        return;
    }
    buf[f.file_size] = '\0';

    program_count = 0;
    for (int i = 0; i < 26; i++) variables[i] = 0;
    had_error = 0;
    error_line = 0;

    const char *cursor = (const char *)buf;
    while (*cursor && program_count < MAX_LINES) {
        const char *line_start = cursor;
        while (*cursor && *cursor != '\n') cursor++;
        const char *line_end = cursor;
        if (*cursor == '\n') cursor++;

        const char *lp = line_start;
        skip_spaces(&lp);
        if (lp >= line_end || !is_digit(*lp)) continue; /* skip blank/unnumbered lines */

        int number = 0;
        while (lp < line_end && is_digit(*lp)) { number = number * 10 + (*lp - '0'); lp++; }
        skip_spaces(&lp);

        struct basic_line *bl = &program[program_count];
        bl->number = number;
        int copy_len = (int)(line_end - lp);
        if (copy_len >= MAX_LINE_LEN) copy_len = MAX_LINE_LEN - 1;
        for (int k = 0; k < copy_len; k++) {
            char c = lp[k];
            if (c == '\r') { copy_len = k; break; }
            bl->text[k] = c;
        }
        bl->text[copy_len] = '\0';
        program_count++;
    }

    kfree(buf);

    if (program_count == 0) {
        vga_write("kronk: no numbered lines found in file\n");
        return;
    }

    vga_write("Running ");
    vga_write(filename);
    vga_write("...\n\n");

    int pc = 0;
    int steps = 0;
    const int MAX_STEPS = 200000; /* guard against a runaway infinite loop */
    while (pc >= 0 && pc < program_count) {
        int cur_line_number = program[pc].number;
        int result = exec_line(pc);

        if (had_error) {
            vga_write("\n?SYNTAX ERROR AT LINE ");
            print_int(error_line ? error_line : cur_line_number);
            vga_write("\n");
            return;
        }
        if (result == -2) break;
        pc = (result == -1) ? pc + 1 : result;

        if (++steps > MAX_STEPS) {
            vga_write("\n?PROGRAM TERMINATED -- too many steps (possible infinite loop)\n");
            return;
        }
    }

    vga_write("\nProgram finished.\n");
}
