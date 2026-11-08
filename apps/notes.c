#include "../ui/window.h"
#include "luna.h"
#include "settings.h"
static char document[FILE_CAP] = "# Luna Notes\n\nWrite Markdown or code here.\n";
static char path[PATH_CAP] = "/home/luna/note.md";
static int caret, top, language;
static bool editing_path, dirty;
static const char *message = "Ctrl+S save / Ctrl+O open / Ctrl+L path / Tab indent";
static const char *languages[] = {"Markdown",   "Python", "C / C++",
                                  "JavaScript", "JSON",   "Plain text"};
static void detect(void) {
    char *ext = 0;
    for (char *p = path; *p; p++)
        if (*p == '.')
            ext = p;
    language = !ext                                                                 ? 5
               : !strcmp(ext, ".md")                                                ? 0
               : !strcmp(ext, ".py")                                                ? 1
               : (!strcmp(ext, ".c") || !strcmp(ext, ".h") || !strcmp(ext, ".cpp")) ? 2
               : !strcmp(ext, ".js")                                                ? 3
               : !strcmp(ext, ".json")                                              ? 4
                                                                                    : 5;
}
static void insert(char c) {
    int n = strlen(document);
    if (n >= FILE_CAP - 1) {
        message = "File limit reached (8 KiB).";
        return;
    }
    memmove(document + caret + 1, document + caret, n - caret + 1);
    document[caret++] = c;
    dirty = true;
}
static int line_start(int p) {
    while (p && document[p - 1] != '\n')
        p--;
    return p;
}
static int line_end(int p) {
    while (document[p] && document[p] != '\n')
        p++;
    return p;
}
void notes_key(int key) {
    int c = key & 0xfff;
    if (key & MOD_CTRL) {
        if (c == 'l' || c == 'o') {
            editing_path = true;
            message = "Edit path, Enter to open / Esc to keep document";
        }
        if (c == 's') {
            bool ok = fs_write(path, document, false) == 0 && fs_save();
            message = ok ? "Saved to disk." : "Save failed; document kept in memory.";
            if (ok)
                dirty = false;
        }
    } else if (editing_path) {
        int n = strlen(path);
        if (c == 27)
            editing_path = false;
        else if (c == 8 && n)
            path[n - 1] = 0;
        else if (c == 10) {
            File *f = fs_find(path);
            if (f && !f->dir && fs_read_text(f, document, sizeof(document))) {
                caret = top = 0;
                dirty = false;
                message = "Opened.";
            } else
                message = "File missing, unreadable or larger than the editor buffer.";
            detect();
            editing_path = false;
        } else if (c >= 32 && c < 127 && n < PATH_CAP - 1) {
            path[n] = c;
            path[n + 1] = 0;
        }
    } else {
        int n = strlen(document);
        if (c == KEY_LEFT && caret) {
            do {
                caret--;
            } while (caret && (document[caret] & 0xc0) == 0x80);
        } else if (c == KEY_RIGHT && caret < n) {
            do {
                caret++;
            } while (caret < n && (document[caret] & 0xc0) == 0x80);
        } else if (c == KEY_HOME)
            caret = line_start(caret);
        else if (c == KEY_END)
            caret = line_end(caret);
        else if (c == KEY_UP) {
            int start = line_start(caret), col = caret - start;
            if (start) {
                int prev = line_start(start - 1);
                caret = prev + col;
                if (caret > start - 1)
                    caret = start - 1;
            }
        } else if (c == KEY_DOWN) {
            int col = caret - line_start(caret), end = line_end(caret);
            if (document[end]) {
                int next = line_end(end + 1);
                caret = end + 1 + col;
                if (caret > next)
                    caret = next;
            }
        } else if (c == 8 && caret) {
            int end = caret;
            do {
                caret--;
            } while (caret && (document[caret] & 0xc0) == 0x80);
            memmove(document + caret, document + end, n - end + 1);
            dirty = true;
        } else if (c == KEY_DELETE && caret < n) {
            int end = caret + 1;
            while (end < n && (document[end] & 0xc0) == 0x80)
                end++;
            memmove(document + caret, document + end, n - end + 1);
            dirty = true;
        } else if (c == 9) {
            for (int i = 0; i < 4; i++)
                insert(' ');
        } else if (c == 10) {
            int start = line_start(caret), count = 0;
            while (document[start + count] == ' ' && start + count < caret)
                count++;
            insert('\n');
            while (count--)
                insert(' ');
        } else if (c >= 32 && c < 256)
            insert(c);
    }
    gui_invalidate_app(APP_NOTES);
}
void notes_click(int x, int y) {
    if (y < 35) {
        if (x < 420)
            editing_path = true;
        else
            language = (language + 1) % 6;
    }
    gui_invalidate_app(APP_NOTES);
}
static bool wordchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9');
}
static bool keyword(const char *s) {
    const char *words[] = {"def",   "class", "return", "import", "from",   "if",       "else",
                           "elif",  "for",   "while",  "try",    "except", "with",     "as",
                           "async", "await", "True",   "False",  "None",   "int",      "char",
                           "void",  "const", "static", "struct", "bool",   "function", "let",
                           "var",   "new",   "null",   "true",   "false",  "export",   0};
    for (int i = 0; words[i]; i++)
        if (!strcmp(s, words[i]))
            return true;
    return false;
}
void notes_draw(int x, int y, int w, int h) {
    round_rect(x, y, w, 32, 10, 0xffffff, 20);
    text(x + 10, y + 8, path, 13, editing_path ? 0xffce74 : 0xe6f0ff);
    text(x + w - 125, y + 8, languages[language], 13, 0x8bdddc);
    int rows = (h - 84) / 22, row = 0;
    for (int i = 0; i < caret; i++)
        if (document[i] == '\n')
            row++;
    if (row < top)
        top = row;
    if (row >= top + rows)
        top = row - rows + 1;
    int p = 0, line = 0;
    while (line < top && document[p])
        if (document[p++] == '\n')
            line++;
    gfx_clip(x, y + 40, w, h - 78);
    for (int r = 0; r < rows; r++, line++) {
        int yy = y + 43 + r * 22;
        char n[16];
        number(n, line + 1);
        text(x + 3, yy, n, 13, 0x8ca3bc);
        int xx = x + 43, quote = 0;
        bool comment = false, heading = language == 0 && document[p] == '#';
        while (document[p] && document[p] != '\n') {
            char token[128];
            int count = 1;
            uint32_t color = 0xe6efff;
            char c = document[p];
            if (language != 5) {
                if (!quote && ((c == '#' && language == 1) || (c == '/' && document[p + 1] == '/' &&
                                                               (language == 2 || language == 3))))
                    comment = true;
                if (comment)
                    color = 0x8db898;
                else if (quote || c == '\'' || c == '"' || c == '`') {
                    color = 0xe8c18b;
                    if (!quote)
                        quote = c;
                    else if (c == quote && (p == 0 || document[p - 1] != '\\'))
                        quote = 0;
                } else if (c >= '0' && c <= '9')
                    color = 0xd8a4d9;
                else if (wordchar(c)) {
                    while (count < 127 && wordchar(document[p + count]))
                        count++;
                    memcpy(token, document + p, count);
                    token[count] = 0;
                    if (keyword(token))
                        color = 0x9aaeff;
                }
                if (heading)
                    color = 0x8bdddc;
            }
            if (count == 1 && ((unsigned char)c) >= 0xc0) {
                while (count < 4 && (document[p + count] & 0xc0) == 0x80)
                    count++;
            }
            memcpy(token, document + p, count);
            token[count] = 0;
            if (caret >= p && caret < p + count)
                rect(xx, yy, 2, 18, 0xffffff);
            text(xx, yy, token, 16, color);
            xx += text_width(token, 16);
            p += count;
        }
        if (caret == p)
            rect(xx, yy, 2, 18, 0xffffff);
        if (!document[p])
            break;
        p++;
    }
    gfx_unclip();
    text(x + 4, y + h - 35, dirty ? "Modified" : "Saved / unchanged", 13,
         dirty ? 0xffce74 : 0x8bdddc);
    text(x + 4, y + h - 17, message, 13, 0xb9cee5);
}
