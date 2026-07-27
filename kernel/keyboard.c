/* keyboard.c -- PS/2 keyboard driver with an input ring buffer
 *
 * The IRQ handler only decodes scancodes and pushes ASCII characters
 * into a buffer -- it does NOT echo to the screen itself.
 * Echoing (and handling backspace visually) is the shell's job, via
 * keyboard_getchar(), so the shell has full control over line editing.
 */

#include "keyboard.h"
#include "idt.h"
#include "task.h"

static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Scancode set 1 -> ASCII, unshifted. 0 means "no printable character". */
static const char scancode_ascii[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

static const char scancode_ascii_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

#define KB_BUF_SIZE 256
static volatile char kb_buffer[KB_BUF_SIZE];
static volatile unsigned int kb_head = 0; /* next write position */
static volatile unsigned int kb_tail = 0; /* next read position */
static int shift_held = 0;

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

static void kb_buffer_push(char c) {
    unsigned int next = (kb_head + 1) % KB_BUF_SIZE;
    if (next == kb_tail) return; /* buffer full, drop the character */
    kb_buffer[kb_head] = c;
    kb_head = next;
}

static void keyboard_callback(void) {
    unsigned char scancode = inb(0x60);
    int released = scancode & 0x80;
    unsigned char code = scancode & 0x7F;

    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        shift_held = !released;
        return;
    }

    if (released) return; /* only act on key-down for everything else */

    if (code < 128) {
        char c = shift_held ? scancode_ascii_shift[code] : scancode_ascii[code];
        if (c) kb_buffer_push(c);
    }
}

void keyboard_init(void) {
    irq_install_handler(1, keyboard_callback);
}

int keyboard_has_char(void) {
    if (task_current_uses_input_redirect()) return task_current_input_has_char();
    return kb_head != kb_tail;
}

/* Blocks (via hlt, so other tasks keep running via preemption) until a
 * character is available, then returns it. If the calling task has
 * input redirection enabled (i.e. it's hosted inside a GUI window),
 * transparently reads from that task's own private queue instead --
 * see task.c's comment on use_input_redirect for why. */
char keyboard_getchar(void) {
    if (task_current_uses_input_redirect()) {
        while (!task_current_input_has_char()) {
            __asm__ volatile ("hlt");
        }
        return task_current_input_getchar();
    }

    while (kb_head == kb_tail) {
        __asm__ volatile ("hlt");
    }
    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}
