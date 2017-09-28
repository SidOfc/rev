#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

 /* includes */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/* defines */
#define REV_VERSION "0.0.1"
#define TABSIZE 4
#define CTRL_KEY(k) ((k) & 0x1f)

enum key {
  BACKSPACE = 127,
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

typedef struct row {
  int  size;
  int  rsize;
  char *chars;
  char *render;
} row;

/* data */
struct configuration {
  int    render_x;
  int    cursor_x;
  int    cursor_y;
  int    row_offset;
  int    col_offset;
  int    screen_rows;
  int    screen_cols;
  int    rows;
  row    *row;
  char   *filename;
  char   status_msg[80];
  time_t status_ts;
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
int curxToRenx(row *row, int curx) {
  int rx = 0;

  for (int j = 0; j < curx; j++) {
    if (row->chars[j] == '\t') rx += (TABSIZE - 1) - (rx % TABSIZE);
    rx++;
  }

  return rx;
}

void updateRow(row *row) {
  int idx  = 0;
  int tabs = 0;

  for (int j = 0; j < row->size; j++) if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs * (TABSIZE - 1) + 1);

  for (int i = 0; i < row->size; i++) {
    switch (row->chars[i]) {
      case '\t':
        for (int l = 0; l < TABSIZE; l++) row->render[idx++] = ' ';
        break;
      default:
        row->render[idx++] = row->chars[i];
        break;
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}

void insertChar(row *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  updateRow(row);
}

void appendRow(char *s, size_t len) {
  CONF.row = realloc(CONF.row, sizeof(row) * (CONF.rows + 1));

  int at = CONF.rows;
  CONF.row[at].size  = len;
  CONF.row[at].chars = malloc(len + 1);

  memcpy(CONF.row[at].chars, s, len);

  CONF.row[at].chars[len] = '\0';
  CONF.row[at].rsize = 0;
  CONF.row[at].render = NULL;

  updateRow(&CONF.row[at]);

  CONF.rows++;
}

/* editor operations */
void editorInsertChar(int c) {
  if (CONF.cursor_y == CONF.rows) appendRow("", 0);
  insertChar(&CONF.row[CONF.cursor_y], CONF.cursor_x, c);
  CONF.cursor_x++;
}

void editorOpen(char *filename) {
  free(CONF.filename);
  CONF.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("editorOpen:fopen");

  char    *line = NULL;
  size_t  linecap = '0';
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
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
  snprintf(buf, sizeof(buf), "\x1b[%d;%d;H", (CONF.cursor_y - CONF.row_offset - 1) + 1, (CONF.render_x - CONF.col_offset - 1) + 1);
  abufAppend(ab, buf, strlen(buf));
}

void showCursor(struct abuf *ab) {
  abufAppend(ab, "\x1b[?25h", 6);
}

void drawStatusMsg(struct abuf *ab) {
  abufAppend(ab, "\x1b[K", 3);

  int len = strlen(CONF.status_msg);
  if (len > CONF.screen_cols) len = CONF.screen_cols;
  if (len && (time(NULL) - CONF.status_ts) < 5)
    abufAppend(ab, CONF.status_msg, len);
}

void setStatusMsg(const char *msg_fmt, ...) {
  va_list fmt_list;

  va_start(fmt_list, msg_fmt);
  vsnprintf(CONF.status_msg, sizeof(CONF.status_msg), msg_fmt, fmt_list);
  va_end(fmt_list);
  CONF.status_ts = time(NULL);
}

void drawDebugStatusBar(struct abuf *ab) {
  char status[80];
  int  len = snprintf(status, sizeof(status),
                      " [cy:%03d] [cx:%03d] [ro:%03d] [co:%03d]",
                      CONF.cursor_y,
                      CONF.cursor_x,
                      CONF.row_offset,
                      CONF.col_offset);

  if (len  > CONF.screen_cols) len  = CONF.screen_cols;

  abufAppend(ab, "\x1b[7m", 4);
  abufAppend(ab, status, len);
  for (int i = len; i < CONF.screen_cols; i++) abufAppend(ab, " ", 1);
  abufAppend(ab, "\x1b[m", 3);
  drawStatusMsg(ab);
}

void drawStatusBar(struct abuf *ab) {
  char status[80];
  char rstatus[80];
  int  len = snprintf(status, sizeof(status),
                      " [%.20s] - %d line%s",
                      CONF.filename ? CONF.filename : "new",
                      CONF.rows,
                      CONF.rows != 1 ? "s" : "");
  int  rlen = snprintf(rstatus, sizeof(rstatus),
                       "%d:%d | %02.2f%% ",
                       CONF.cursor_y + 1,
                       CONF.cursor_x + 1,
                       100.0 - ((((float) CONF.rows - CONF.cursor_y) / CONF.rows) * 100));

  if (len  > CONF.screen_cols) len  = CONF.screen_cols;
  if (rlen > CONF.screen_cols) rlen = CONF.screen_cols;

  abufAppend(ab, "\x1b[7m", 4);
  abufAppend(ab, status, len);
  for (int i = len + rlen; i < CONF.screen_cols; i++) abufAppend(ab, " ", 1);
  abufAppend(ab, rstatus, rlen);
  abufAppend(ab, "\x1b[m", 3);
  drawStatusMsg(ab);
}

void drawRows(struct abuf *ab) {
  for (int y = 0; y < CONF.screen_rows; y++) {
    int file_row = y + CONF.row_offset;
    if (file_row >= CONF.rows) {
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
      } else abufAppend(ab, "~", 1);
    } else {
      int len = CONF.row[file_row].rsize - CONF.col_offset;
      if (len < 0) len = 0;
      if (len > CONF.screen_cols) len = CONF.screen_cols;
      abufAppend(ab, &CONF.row[file_row].render[CONF.col_offset], len);
    }

    abufAppend(ab, "\x1b[K\r\n", 5);
  }
}

void updateCursorPos(int key) {
  row *row = (CONF.cursor_y >= CONF.rows) ? NULL : &CONF.row[CONF.cursor_y];

  switch (key) {
    case ARROW_LEFT:
      if (CONF.cursor_x != 0) CONF.cursor_x--;
      else if (CONF.cursor_y > 0) {
        CONF.cursor_y--;
        CONF.cursor_x = CONF.row[CONF.cursor_y].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && CONF.cursor_x < row->size) CONF.cursor_x++;
      else if (CONF.cursor_y != CONF.rows && CONF.cursor_x == row->size) {
        CONF.cursor_y++;
        CONF.cursor_x = 0;
      }
      break;
    case PAGE_UP:
    case ARROW_UP:
      if (CONF.row_offset > 0) CONF.row_offset--;
      if (CONF.cursor_y != 0) CONF.cursor_y--;
      break;
    case PAGE_DOWN:
    case ARROW_DOWN:
      if (CONF.cursor_y < CONF.rows) CONF.cursor_y++;
      break;
    case HOME:
      CONF.cursor_x = 0;
      break;
    case END:
      CONF.cursor_x = CONF.row[CONF.cursor_y].size;
      break;
  }

  row = (CONF.cursor_y >= CONF.rows) ? NULL : &CONF.row[CONF.cursor_y];
  int len = row ? row->size : 0;
  if (CONF.cursor_x > len) CONF.cursor_x = len;
}

void scroll() {
  CONF.render_x = 0;

  if (CONF.cursor_y < CONF.rows)
    CONF.render_x = curxToRenx(&CONF.row[CONF.cursor_y], CONF.cursor_x);

  if (CONF.cursor_y < CONF.row_offset)
    CONF.row_offset = CONF.cursor_y;

  if (CONF.cursor_y > CONF.row_offset + CONF.screen_rows)
    CONF.row_offset = CONF.cursor_y - CONF.screen_rows;

  if (CONF.render_x < CONF.col_offset)
    CONF.col_offset = CONF.render_x;

  if (CONF.render_x > CONF.col_offset + CONF.screen_cols)
    CONF.col_offset = CONF.render_x - CONF.screen_cols;
}

void render(struct abuf ab) {
  scroll();
  hideCursor(&ab);
  abufAppend(&ab, "\x1b[H",  3);
  drawRows(&ab);
  /* drawStatusBar(&ab); */
  drawDebugStatusBar(&ab);
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

    case '\x1b':
      break;

    case '\r':
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL:
      break;

    case CTRL_KEY('l'):
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        CONF.cursor_y = CONF.row_offset;
        if (c == PAGE_DOWN) {
          CONF.cursor_y += CONF.screen_rows;
          if (CONF.cursor_y > CONF.rows) CONF.cursor_y = CONF.rows;
        }

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
    default:
      editorInsertChar(c);
      break;
  }
}

/* init */
void initConf() {
  CONF.filename      = NULL;
  CONF.row           = NULL;
  CONF.render_x      = 0;
  CONF.cursor_x      = 0;
  CONF.cursor_y      = 0;
  CONF.rows          = 0;
  CONF.row_offset    = 0;
  CONF.col_offset    = 0;
  CONF.status_ts     = 0;
  CONF.status_msg[0] = '\0';

  if (winSize(&CONF.screen_rows, &CONF.screen_cols) == -1)
    die("init{winSize}");

  CONF.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
  struct abuf ab = {NULL, 0};

  enableRawMode();
  initConf();
  if (argc >= 2) editorOpen(argv[1]);

  setStatusMsg("use <C-q> to quit");

  while (1) {
    render(ab);
    processKeypress();
  }

  return 0;
}
