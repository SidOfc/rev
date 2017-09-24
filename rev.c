#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

 /* includes */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/* defines */
#define REV_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum key {
  ARROW_UP = 1000,
  ARROW_DOWN,
  ARROW_RIGHT,
  ARROW_LEFT,
  HOME,
  END,
  DEL,
  PAGE_UP,
  PAGE_DOWN
};

typedef struct erow {
  int  size;
  char *chars;
} row;

/* data */
struct configuration {
  int cursor_x;
  int cursor_y;
  int screen_rows;
  int screen_cols;
  int rows;
  struct erow row;
  struct termios original_termios;
};

struct configuration CONF;

/* append buf */
struct abuf {
  char *b;
  int len;
};

void abufAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);

  ab->b    = new;
  ab->len += len;
}

void abufFree(struct abuf *ab) {
  free(ab->b);
}

/* terminal */
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H",  3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &CONF.original_termios) == -1)
    die("disableRawMode{tcsetattr}");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &CONF.original_termios) == -1)
    die("enableRawMode{tcgetattr}");

  atexit(disableRawMode);

  struct termios current = CONF.original_termios;

  current.c_iflag &= ~(IXON | ICRNL | ISTRIP | INPCK | BRKINT);
  current.c_oflag &= ~(OPOST);
  current.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  current.c_cflag |=  (CS8);

  current.c_cc[VMIN]  = 0;
  current.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &current) == -1)
    die("enableRawMode{tcsetattr}");
}

int readKeypress() {
  int  nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    if (nread == -1 && errno != EAGAIN) die("handleKeypress{read}");

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME;
            case '3': return DEL;
            case '4': return END;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME;
            case '8': return END;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'F': return END;
          case 'H': return HOME;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'F': return END;
        case 'H': return HOME;
      }
    }

    return '\x1b';
  }

  return c;
}

int cursorPos(int *rows, int *cols) {
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  char buf[32];
  unsigned int i = 0;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }

  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int winSize(int *rows, int *cols) {
  struct winsize w;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return cursorPos(rows, cols);
  } else {
    *cols = w.ws_col;
    *rows = w.ws_row;
    return 0;
  }
}

/* row operations */
void appendRow(char *s, size_t len) {
  CONF.row.size  = len;
  CONF.row.chars = malloc(len + 1);

  memcpy(CONF.row.chars, s, len);

  CONF.row.chars[len] = '\0';
  CONF.rows = 1;
}

/* file io */
void editorOpen(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) die("editorOpen:fopen");

  char    *line = NULL;
  size_t  linecap = '0';
  ssize_t linelen;

  linelen = getline(&line, &linecap, fp);

  if (linelen != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    appendRow(line, linelen);
  }

  free(line);
  fclose(fp);
}

/* output */
void hideCursor(struct abuf *ab) {
  abufAppend(ab, "\x1b[?25l", 6);
}

void moveCursor(struct abuf *ab) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%d;H", CONF.cursor_y + 1, CONF.cursor_x + 1);
  abufAppend(ab, buf, strlen(buf));
}

void showCursor(struct abuf *ab) {
  abufAppend(ab, "\x1b[?25h", 6);
}

void drawRows(struct abuf *ab) {
  for (int y = 0; y < CONF.screen_rows; y++) {
    if (y >= CONF.rows) {
      if (CONF.rows == 0 && y == CONF.screen_rows / 2) {
        char welcome[80];
        int welcome_len = snprintf(welcome, sizeof(welcome),
                                   "Rev editor -- version %s", REV_VERSION);

        if (welcome_len > CONF.screen_cols) welcome_len = CONF.screen_cols;

        int padding = (CONF.screen_cols - welcome_len) / 2;

        if (padding) {
          abufAppend(ab, "~", 1);
          padding--;
          while (padding--) abufAppend(ab, " ", 1);
        }

        abufAppend(ab, welcome, welcome_len);
      } else {
        abufAppend(ab, "~", 1);
      }
    } else {
      int len = CONF.row.size;
      if (len > CONF.screen_cols) len = CONF.screen_cols;
      abufAppend(ab, CONF.row.chars, len);
    }

    abufAppend(ab, "\x1b[K", 3);
    if (y < CONF.screen_rows - 1) abufAppend(ab, "\r\n", 2);
  }
}

void updateCursorPos(int key) {
  switch (key) {
    case ARROW_LEFT:
      if (CONF.cursor_x != 0) CONF.cursor_x--;
      break;
    case ARROW_RIGHT:
      if (CONF.cursor_x != CONF.screen_cols - 1) CONF.cursor_x++;
      break;
    case PAGE_UP:
    case ARROW_UP:
      if (CONF.cursor_y != 0) CONF.cursor_y--;
      break;
    case PAGE_DOWN:
    case ARROW_DOWN:
      if (CONF.cursor_y != CONF.screen_rows - 1) CONF.cursor_y++;
      break;
    case HOME:
      CONF.cursor_x = 0;
      break;
    case END:
      CONF.cursor_x = CONF.screen_cols - 1;
      break;
  }
}

void render(struct abuf ab) {
  hideCursor(&ab);
  abufAppend(&ab, "\x1b[H",  3);
  drawRows(&ab);
  moveCursor(&ab);
  showCursor(&ab);

  write(STDOUT_FILENO, ab.b, ab.len);
  abufFree(&ab);
}

/* input */
void processKeypress() {
  int c = readKeypress();

  switch(c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H",  3);
      exit(0);
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      {
        int dist = CONF.screen_rows;
        while (dist--) updateCursorPos(c);
      }
      break;
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_DOWN:
    case HOME:
    case END:
      updateCursorPos(c);
      break;
  }
}

/* init */
void initConf() {
  CONF.cursor_x = 0;
  CONF.cursor_y = 0;
  CONF.rows     = 0;
  /* CONF.row      = NULL; */

  if (winSize(&CONF.screen_rows, &CONF.screen_cols) == -1)
    die("init{winSize}");
}

int main(int argc, char *argv[]) {
  struct abuf ab = {NULL, 0};

  enableRawMode();
  initConf();
  if (argc >= 2) editorOpen(argv[1]);

  while (1) {
    render(ab);
    processKeypress();
  }

  return 0;
}
