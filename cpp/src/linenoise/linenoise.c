/* linenoise.c — minimal cross-platform line editing with history
 * Vendored for LOOK language. Inspired by antirez/linenoise (BSD-2-Clause).
 * Windows: Console API (ReadConsoleInput). POSIX: termios raw mode.
 */
#include "linenoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LN_MAX_LINE  4096
#define LN_HISTORY_DEF 100

/* ── History ──────────────────────────────────────────────────────────── */
static char** ln_history     = NULL;
static int    ln_history_len = 0;
static int    ln_history_max = LN_HISTORY_DEF;

void linenoiseHistorySetMaxLen(int len) {
    if (len < 1) len = 1;
    ln_history_max = len;
}

int linenoiseHistoryAdd(const char* line) {
    if (!line || !*line) return 0;
    /* Skip duplicate of last entry */
    if (ln_history_len > 0 && strcmp(ln_history[ln_history_len-1], line) == 0)
        return 0;
    if (!ln_history) {
        ln_history = (char**)calloc(ln_history_max, sizeof(char*));
        if (!ln_history) return 0;
    }
    if (ln_history_len == ln_history_max) {
        free(ln_history[0]);
        memmove(ln_history, ln_history+1, (ln_history_max-1)*sizeof(char*));
        ln_history_len--;
    }
    ln_history[ln_history_len] = strdup(line);
    if (!ln_history[ln_history_len]) return 0;
    ln_history_len++;
    return 1;
}

void linenoiseHistorySave(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    for (int i = 0; i < ln_history_len; i++)
        fprintf(f, "%s\n", ln_history[i]);
    fclose(f);
}

void linenoiseHistoryLoad(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return;
    char buf[LN_MAX_LINE];
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n-1]=='\n'||buf[n-1]=='\r')) buf[--n]=0;
        linenoiseHistoryAdd(buf);
    }
    fclose(f);
}

void linenoiseClearScreen(void) {
#ifdef _WIN32
    system("cls");
#else
    fputs("\x1b[H\x1b[2J", stdout);
    fflush(stdout);
#endif
}

void linenoise_free(char* line) { free(line); }

/* ══════════════════════════════════════════════════════════════════════
 * WINDOWS IMPLEMENTATION
 * ══════════════════════════════════════════════════════════════════════ */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>

char* linenoise(const char* prompt) {
    HANDLE hin  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    /* If not a console (pipe / redirect), fall back to fgets */
    if (!_isatty(_fileno(stdin))) {
        fputs(prompt, stdout);
        fflush(stdout);
        char* buf = (char*)malloc(LN_MAX_LINE);
        if (!buf) return NULL;
        if (!fgets(buf, LN_MAX_LINE, stdin)) { free(buf); return NULL; }
        size_t n = strlen(buf);
        while (n && (buf[n-1]=='\n'||buf[n-1]=='\r')) buf[--n]=0;
        return buf;
    }

    DWORD orig_mode;
    GetConsoleMode(hin, &orig_mode);
    DWORD raw_mode = orig_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
    SetConsoleMode(hin, raw_mode);

    char*  buf  = (char*)malloc(LN_MAX_LINE);
    if (!buf) { SetConsoleMode(hin, orig_mode); return NULL; }
    int    len  = 0;
    int    pos  = 0;
    int    hist_idx = ln_history_len;  /* points past last = current input */

    /* Print prompt */
    DWORD written;
    WriteConsoleA(hout, prompt, (DWORD)strlen(prompt), &written, NULL);

    while (1) {
        INPUT_RECORD ir;
        DWORD nr;
        ReadConsoleInputA(hin, &ir, 1, &nr);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        KEY_EVENT_RECORD* ke = &ir.Event.KeyEvent;
        WORD vk  = ke->wVirtualKeyCode;
        char ch  = ke->uChar.AsciiChar;

        if (vk == VK_RETURN) {
            buf[len] = 0;
            WriteConsoleA(hout, "\r\n", 2, &written, NULL);
            break;
        }
        if (vk == VK_ESCAPE || (ch == 'C' && (ke->dwControlKeyState & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED)))) {
            /* Ctrl+C */
            buf[0] = 0;
            WriteConsoleA(hout, "\r\n", 2, &written, NULL);
            SetConsoleMode(hin, orig_mode);
            free(buf);
            return NULL;
        }
        if (vk == VK_BACK && pos > 0) {
            memmove(buf+pos-1, buf+pos, len-pos);
            pos--; len--;
            /* Redraw */
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            int prompt_len = (int)strlen(prompt);
            COORD c = {(SHORT)(prompt_len), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            buf[len]=0;
            WriteConsoleA(hout, buf, len, &written, NULL);
            WriteConsoleA(hout, " ", 1, &written, NULL);
            c.X = (SHORT)(prompt_len + pos);
            SetConsoleCursorPosition(hout, c);
            continue;
        }
        if (vk == VK_DELETE && pos < len) {
            memmove(buf+pos, buf+pos+1, len-pos-1);
            len--;
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            int prompt_len = (int)strlen(prompt);
            COORD c = {(SHORT)(prompt_len), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            buf[len]=0;
            WriteConsoleA(hout, buf, len, &written, NULL);
            WriteConsoleA(hout, " ", 1, &written, NULL);
            c.X = (SHORT)(prompt_len + pos);
            SetConsoleCursorPosition(hout, c);
            continue;
        }
        if (vk == VK_LEFT && pos > 0) {
            pos--;
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            COORD c = {(SHORT)(csbi.dwCursorPosition.X - 1), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            continue;
        }
        if (vk == VK_RIGHT && pos < len) {
            pos++;
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            COORD c = {(SHORT)(csbi.dwCursorPosition.X + 1), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            continue;
        }
        if (vk == VK_HOME) {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            COORD c = {(SHORT)strlen(prompt), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            pos = 0;
            continue;
        }
        if (vk == VK_END) {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            COORD c = {(SHORT)(strlen(prompt)+len), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            pos = len;
            continue;
        }
        /* History navigation */
        if (vk == VK_UP && hist_idx > 0) {
            hist_idx--;
            const char* h = ln_history[hist_idx];
            len = (int)strlen(h);
            if (len >= LN_MAX_LINE) len = LN_MAX_LINE-1;
            memcpy(buf, h, len); buf[len]=0;
            pos = len;
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            int prompt_len = (int)strlen(prompt);
            COORD c = {(SHORT)prompt_len, csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            WriteConsoleA(hout, buf, len, &written, NULL);
            WriteConsoleA(hout, "   ", 3, &written, NULL);
            c.X = (SHORT)(prompt_len + pos);
            SetConsoleCursorPosition(hout, c);
            continue;
        }
        if (vk == VK_DOWN) {
            if (hist_idx < ln_history_len - 1) {
                hist_idx++;
                const char* h = ln_history[hist_idx];
                len = (int)strlen(h);
                if (len >= LN_MAX_LINE) len = LN_MAX_LINE-1;
                memcpy(buf, h, len); buf[len]=0;
            } else {
                hist_idx = ln_history_len;
                len = 0; buf[0] = 0;
            }
            pos = len;
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hout, &csbi);
            int prompt_len = (int)strlen(prompt);
            COORD c = {(SHORT)prompt_len, csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hout, c);
            WriteConsoleA(hout, buf, len, &written, NULL);
            WriteConsoleA(hout, "   ", 3, &written, NULL);
            c.X = (SHORT)(prompt_len + pos);
            SetConsoleCursorPosition(hout, c);
            continue;
        }

        /* Ctrl+L = clear */
        if (ch == 12) { linenoiseClearScreen(); continue; }

        /* Printable character */
        if (ch >= 32 && len < LN_MAX_LINE-1) {
            if (pos < len) memmove(buf+pos+1, buf+pos, len-pos);
            buf[pos] = ch;
            pos++; len++;
            buf[len] = 0;
            /* Redraw from pos */
            WriteConsoleA(hout, buf+pos-1, len-pos+1, &written, NULL);
            /* Move cursor back to pos */
            if (pos < len) {
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                GetConsoleScreenBufferInfo(hout, &csbi);
                COORD c = {(SHORT)(csbi.dwCursorPosition.X - (len-pos)), csbi.dwCursorPosition.Y};
                SetConsoleCursorPosition(hout, c);
            }
        }
    }

    SetConsoleMode(hin, orig_mode);
    return buf;
}

/* ══════════════════════════════════════════════════════════════════════
 * POSIX IMPLEMENTATION
 * ══════════════════════════════════════════════════════════════════════ */
#else  /* !_WIN32 */

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>

static struct termios ln_orig_termios;

static void ln_disable_raw(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ln_orig_termios);
}

static int ln_enable_raw(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &ln_orig_termios) == -1) return -1;
    struct termios raw = ln_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;
    return 0;
}

static void ln_refresh(const char* prompt, const char* buf, int len, int pos) {
    /* Move to col 0, print prompt + buf, clear to EOL, move cursor to pos */
    char seq[64];
    /* CR */
    write(STDOUT_FILENO, "\r", 1);
    write(STDOUT_FILENO, prompt, strlen(prompt));
    write(STDOUT_FILENO, buf, len);
    /* Clear rest of line */
    write(STDOUT_FILENO, "\x1b[0K", 4);
    /* Move cursor to pos */
    int col = (int)strlen(prompt) + pos + 1;  /* 1-based */
    snprintf(seq, sizeof(seq), "\r\x1b[%dC", (int)strlen(prompt) + pos);
    write(STDOUT_FILENO, seq, strlen(seq));
}

char* linenoise(const char* prompt) {
    /* Not a tty — simple fgets */
    if (!isatty(STDIN_FILENO)) {
        fputs(prompt, stdout);
        fflush(stdout);
        char* buf = (char*)malloc(LN_MAX_LINE);
        if (!buf) return NULL;
        if (!fgets(buf, LN_MAX_LINE, stdin)) { free(buf); return NULL; }
        size_t n = strlen(buf);
        while (n && (buf[n-1]=='\n'||buf[n-1]=='\r')) buf[--n]=0;
        return buf;
    }

    if (ln_enable_raw() == -1) return NULL;

    char* buf = (char*)malloc(LN_MAX_LINE);
    if (!buf) { ln_disable_raw(); return NULL; }
    int len = 0, pos = 0, hist_idx = ln_history_len;

    fputs(prompt, stdout);
    fflush(stdout);

    while (1) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;

        if (c == '\r' || c == '\n') {
            buf[len] = 0;
            write(STDOUT_FILENO, "\r\n", 2);
            break;
        }
        if (c == 3) { /* Ctrl+C */
            buf[0] = 0;
            write(STDOUT_FILENO, "\r\n", 2);
            ln_disable_raw();
            free(buf);
            return NULL;
        }
        if (c == 4 && len == 0) { /* Ctrl+D on empty line = EOF */
            ln_disable_raw();
            free(buf);
            return NULL;
        }
        if (c == 12) { /* Ctrl+L */
            linenoiseClearScreen();
            ln_refresh(prompt, buf, len, pos);
            continue;
        }
        if ((c == 127 || c == 8) && pos > 0) { /* Backspace */
            memmove(buf+pos-1, buf+pos, len-pos);
            pos--; len--;
            ln_refresh(prompt, buf, len, pos);
            continue;
        }
        if (c == 27) { /* ESC sequence */
            unsigned char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
            if (seq[0] == '[') {
                if (seq[1] == 'A') { /* Up */
                    if (hist_idx > 0) {
                        hist_idx--;
                        const char* h = ln_history[hist_idx];
                        len = (int)strlen(h);
                        if (len >= LN_MAX_LINE) len = LN_MAX_LINE-1;
                        memcpy(buf, h, len); buf[len]=0;
                        pos = len;
                        ln_refresh(prompt, buf, len, pos);
                    }
                } else if (seq[1] == 'B') { /* Down */
                    if (hist_idx < ln_history_len-1) {
                        hist_idx++;
                        const char* h = ln_history[hist_idx];
                        len = (int)strlen(h);
                        if (len >= LN_MAX_LINE) len = LN_MAX_LINE-1;
                        memcpy(buf, h, len); buf[len]=0;
                    } else {
                        hist_idx = ln_history_len;
                        len = 0; buf[0] = 0;
                    }
                    pos = len;
                    ln_refresh(prompt, buf, len, pos);
                } else if (seq[1] == 'C' && pos < len) { /* Right */
                    pos++;
                    ln_refresh(prompt, buf, len, pos);
                } else if (seq[1] == 'D' && pos > 0) { /* Left */
                    pos--;
                    ln_refresh(prompt, buf, len, pos);
                } else if (seq[1] == 'H') { /* Home */
                    pos = 0;
                    ln_refresh(prompt, buf, len, pos);
                } else if (seq[1] == 'F') { /* End */
                    pos = len;
                    ln_refresh(prompt, buf, len, pos);
                } else if (seq[1] == '3') { /* Delete (ESC[3~) */
                    unsigned char tilde;
                    read(STDIN_FILENO, &tilde, 1);
                    if (tilde == '~' && pos < len) {
                        memmove(buf+pos, buf+pos+1, len-pos-1);
                        len--;
                        ln_refresh(prompt, buf, len, pos);
                    }
                }
            }
            continue;
        }

        /* Printable */
        if (c >= 32 && len < LN_MAX_LINE-1) {
            if (pos < len) memmove(buf+pos+1, buf+pos, len-pos);
            buf[pos] = (char)c;
            pos++; len++;
            buf[len] = 0;
            ln_refresh(prompt, buf, len, pos);
        }
    }

    ln_disable_raw();
    return buf;
}

#endif /* _WIN32 */
