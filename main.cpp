#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <sys/sendfile.h>
#include <charconv>
#include <cstring>

struct Entry {
    std::string name;
    std::string path;
    bool dir = false;
    bool exec = false;
    bool link = false;
    // --- Precomputed Cache ---
    std::string ext;
    bool isText = false;
    bool isKnownText = false;
    bool isImg = false;
    bool isVid = false;
    bool isAud = false;
    bool isPdfFile = false;
};

struct Clipboard {
    std::string path;
    bool move = false;
    bool active = false;
};

// --- Globals ---
termios orig;
std::vector<Entry> entries;
std::vector<Entry> parentEntries;
std::vector<Entry> childEntries;
std::string childCachePath;
Clipboard clip;
std::string cwd;
std::string old_cwd;

size_t selected = 0;
size_t scroll = 0;
size_t prev_selected = static_cast<size_t>(-1);
size_t prev_scroll = static_cast<size_t>(-1);

std::vector<std::string> historyPath;
std::vector<size_t> historyCursor;
int rows = 0;
int cols = 0;
bool running = true;
bool commandMode = false;
std::string command;
bool force_redraw = true;
std::string g_out;

// --- Text Preview Cache ---
std::string cached_preview_path;
std::vector<std::string> cached_preview_lines;

enum {
    KEY_NULL = 0,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_ESC,
    KEY_BACKSPACE
};

// --- Utilities ---
std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string basename(const std::string &path) {
    if (path.empty() || path == "/") return "";
    size_t p = path.find_last_of('/');
    if (p == std::string::npos) return path;
    return path.substr(p + 1);
}

std::string dirName(const std::string &path) {
    size_t p = path.find_last_of('/');
    if (p == std::string::npos) return ".";
    if (p == 0) return "/";
    return path.substr(0, p);
}

std::string extension(const std::string &name) {
    size_t p = name.rfind('.');
    if (p == std::string::npos || p == 0) return "";
    std::string ext = name.substr(p + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

bool isImage(const std::string &e) { return e == "png" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "gif" || e == "webp"; }
bool isText(const std::string &e) { return e == "txt" || e == "cpp" || e == "hpp" || e == "c" || e == "h" || e == "cc" || e == "hh" || e == "md" || e == "json" || e == "toml" || e == "yaml" || e == "yml" || e == "ini" || e == "conf" || e == "sh"; }
bool isAudio(const std::string &e) { return e == "mp3" || e == "ogg" || e == "wav" || e == "flac"; }
bool isVideo(const std::string &e) { return e == "mp4" || e == "mkv" || e == "avi" || e == "mov" || e == "webm"; }
bool isPdf(const std::string &e) { return e == "pdf"; }
bool isKnownTextFile(const std::string &name) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return n == "makefile" || n == ".gitignore" || n == "gnumakefile" || n == "readme" || n == "license" || n == "copying" || n == "todo" || n == "changelog" || n == "install" || n == "news" || n == "authors" || n == "contributing" || n == "security" || n == "codeowners";
}
bool isTextContent(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1)
        return false;

    char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n <= 0)
        return false;

    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\0')
            return false;
    }

    return true;
}


void populatePrecomputed(Entry &e) {
    e.ext = extension(e.name);
    e.isImg = isImage(e.ext);
    e.isAud = isAudio(e.ext);
    e.isVid = isVideo(e.ext);
    e.isPdfFile = isPdf(e.ext);
    e.isKnownText = isKnownTextFile(e.name);
    e.isText = isText(e.ext);

// Si no se reconoció por extensión,
// intenta detectarlo por contenido.
    if (!e.dir && !e.isImg && !e.isAud && !e.isVid && !e.isPdfFile && !e.isText && !e.isKnownText){
        e.isText = isTextContent(e.path);
    }
}

// --- Render Optimization Utilities ---
void appendInt(std::string &s, int v) {
    char buf[32];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, p - buf);
}

void appendCursor(std::string &out, int r, int c) {
    out += "\x1b[";
    appendInt(out, r);
    out += ';';
    appendInt(out, c);
    out += 'H';
}

void appendPadTruncate(std::string &out, const std::string &str, int width) {
    if (width <= 0) return;
    int len = str.length();
    if (len > width) {
        if (width > 3) {
            out.append(str.data(), width - 3);
            out.append("...");
        } else {
            out.append(str.data(), width);
        }
    } else {
        out.append(str);
        out.append(width - len, ' ');
    }
}

void appendTruncate(std::string &out, const std::string &str, int width) {
    if (width <= 0) return;
    int len = str.length();
    if (len > width) {
        if (width > 3) {
            out.append(str.data(), width - 3);
            out.append("...");
        } else {
            out.append(str.data(), width);
        }
    } else {
        out.append(str);
    }
}

// --- POSIX System Implementations ---
int copy_file_posix(const char *src, const char *dst) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) return -1;
    struct stat st;
    fstat(in_fd, &st);
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (out_fd < 0) { close(in_fd); return -1; }
    off_t offset = 0;
    sendfile(out_fd, in_fd, &offset, st.st_size);
    close(in_fd);
    close(out_fd);
    return 0;
}

void copy_dir_posix(const std::string &src, const std::string &dst) {
    mkdir(dst.c_str(), 0755);
    DIR *dir = opendir(src.c_str());
    if (!dir) return;
    struct dirent *d;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        std::string s = join(src, d->d_name);
        std::string t = join(dst, d->d_name);
        struct stat st;
        if (lstat(s.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) copy_dir_posix(s, t);
            else if (S_ISREG(st.st_mode)) copy_file_posix(s.c_str(), t.c_str());
            else if (S_ISLNK(st.st_mode)) {
                char buf[PATH_MAX];
                ssize_t len = readlink(s.c_str(), buf, sizeof(buf) - 1);
                if (len > 0) {
                    buf[len] = '\0';
                    int ign = symlink(buf, t.c_str());
                    (void)ign;
                }
            }
        }
    }
    closedir(dir);
}

void remove_all_posix(const std::string &path) {
    DIR *dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent *d;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        std::string p = join(path, d->d_name);
        struct stat st;
        if (!lstat(p.c_str(), &st)) {
            if (S_ISDIR(st.st_mode)) remove_all_posix(p);
            else unlink(p.c_str());
        }
    }
    closedir(dir);
    rmdir(path.c_str());
}

Entry createEntryFromPath(const std::string &path) {
    Entry e;
    e.name = basename(path);
    e.path = path;
    struct stat st;
    if (lstat(path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            e.link = true;
            struct stat tgt;
            if (stat(path.c_str(), &tgt) == 0) {
                e.dir = S_ISDIR(tgt.st_mode);
                e.exec = (tgt.st_mode & S_IXUSR) && !e.dir;
            }
        } else {
            e.dir = S_ISDIR(st.st_mode);
            e.exec = (st.st_mode & S_IXUSR) && !e.dir;
        }
    }
    populatePrecomputed(e);
    return e;
}

// --- Terminal & Input ---
void disableRaw() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    ssize_t ign = write(STDOUT_FILENO, "\x1b[?25h\x1b[0m", 10);
    (void)ign;
}

void enableRaw() {
    tcgetattr(STDIN_FILENO, &orig);
    atexit(disableRaw);
    termios raw = orig;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void updateSize() {
    winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    if (rows != ws.ws_row || cols != ws.ws_col) force_redraw = true;
    rows = ws.ws_row;
    cols = ws.ws_col;
    if (rows < 6) rows = 6;
    if (cols < 20) cols = 20;
}

int readKey() {
    char c;
    while (read(STDIN_FILENO, &c, 1) != 1);
    if (c == 13) return KEY_ENTER;
    if (c == 127) return KEY_BACKSPACE;
    if (c != 27) return c;

    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESC;
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESC;
    if (seq[0] == '[') {
        switch (seq[1]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
        }
    }
    return KEY_ESC;
}

// --- Directory Logic ---
bool cmp(const Entry &a, const Entry &b) {
    if (a.dir != b.dir) return a.dir > b.dir;
    return a.name < b.name;
}

std::vector<Entry> readDir(const std::string &path) {
    std::vector<Entry> res;
    DIR *dir = opendir(path.c_str());
    if (!dir) return res;
    dirent *d;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        Entry e;
        e.name = d->d_name;
        e.path = join(path, e.name);
        struct stat st;
        if (lstat(e.path.c_str(), &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                e.link = true;
                struct stat tgt;
                if (stat(e.path.c_str(), &tgt) == 0) {
                    e.dir = S_ISDIR(tgt.st_mode);
                    e.exec = (tgt.st_mode & S_IXUSR) && !e.dir;
                }
            } else {
                e.dir = S_ISDIR(st.st_mode);
                e.exec = (st.st_mode & S_IXUSR) && !e.dir;
            }
        }
        populatePrecomputed(e);
        res.push_back(std::move(e));
    }
    closedir(dir);
    std::sort(res.begin(), res.end(), cmp);
    return res;
}

void load() {
    entries = readDir(cwd);
    if (cwd != "/") parentEntries = readDir(join(cwd, ".."));
    else parentEntries.clear();
    childCachePath = "";
    cached_preview_path = "";
    if (selected >= entries.size()) selected = entries.empty() ? 0 : entries.size() - 1;
    if (scroll > selected) scroll = selected;
    force_redraw = true;
}

void clear() {
    ssize_t ign = write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7);
    (void)ign;
    force_redraw = true;
}

// --- Rendering ---
void drawPath(std::string &out) {
    std::string s = cwd;
    const char *home = getenv("HOME");
    if (home) {
        std::string h = home;
        if (s.rfind(h, 0) == 0) s = "~" + s.substr(h.size());
    }
    int max_len = cols - 4;
    if (max_len < 1) max_len = 1;
    
    appendCursor(out, 1, 1);
    out += "\x1b[90m  ";
    appendTruncate(out, s, max_len);
    out += "\x1b[0m\x1b[K";

    appendCursor(out, 2, 1);
    out += "\x1b[90m";
    for (int i = 0; i < cols; i++) out += "─";
    out += "\x1b[0m\x1b[K";
}

void drawEntries(std::string &out, bool partial) {
    int visible = rows - 4;
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + visible) scroll = selected - visible + 1;

    bool multi = cols >= 80;
    int w1 = multi ? cols * 0.30 : 0;
    int w3 = multi ? cols * 0.40 : 0;
    int w2 = cols - w1 - w3;

    size_t parentSelected = 0;
    size_t parentScroll = 0;

    if (multi && cwd != "/") {
        std::string currentDirName = basename(cwd);
        for (size_t i = 0; i < parentEntries.size(); i++) {
            if (parentEntries[i].name == currentDirName) {
                parentSelected = i; break;
            }
        }
        parentScroll = parentSelected;
        if (parentScroll + visible > parentEntries.size()) {
            parentScroll = parentEntries.size() > (size_t)visible ? parentEntries.size() - visible : 0;
        }
        if (parentSelected >= parentScroll + visible) parentScroll = parentSelected - visible + 1;
        if (parentSelected < parentScroll) parentScroll = parentSelected;
    }

    if (multi && !entries.empty() && entries[selected].dir) {
        if (entries[selected].path != childCachePath) {
            childCachePath = entries[selected].path;
            childEntries = readDir(childCachePath);
        }
    }

    for (int i = 0; i < visible; i++) {
        int row = i + 3;
        bool is_old_sel = (i + scroll == prev_selected);
        bool is_new_sel = (i + scroll == selected);
        bool update_mid = !partial || is_old_sel || is_new_sel;

        if (!partial) {
            appendCursor(out, row, 1);
            out += "\x1b[K";
        }

        // Parent Pane
        if (multi && !partial) {
            appendCursor(out, row, 2);
            if (cwd != "/" && i + parentScroll < parentEntries.size()) {
                auto &e = parentEntries[i + parentScroll];
                std::string color = "\x1b[0m";
                if (e.dir) color = "\x1b[1;34m";
                else if (e.link) color = "\x1b[1;36m";
                else if (e.exec) color = "\x1b[1;32m";

                if (i + parentScroll == parentSelected) out += "\x1b[7m" + color;
                else out += color;
                appendPadTruncate(out, e.dir ? e.name + "/" : e.name, w1 - 2);
                out += "\x1b[0m";
            } else {
                out.append(w1 - 2, ' ');
            }
            appendCursor(out, row, w1);
            out += "\x1b[90m│\x1b[0m";
        }

        // Middle Pane
        if (update_mid) {
            int c_start = multi ? w1 + 2 : 2;
            int c_width = multi ? w2 - 2 : cols - 4;
            appendCursor(out, row, c_start);

            if (i + scroll < entries.size()) {
                auto &e = entries[i + scroll];
                std::string color = "\x1b[0m";
                if (e.dir) color = "\x1b[1;34m";
                else if (e.link) color = "\x1b[1;36m";
                else if (e.exec) color = "\x1b[1;32m";

                if (i + scroll == selected) out += "\x1b[44;97m";
                else out += color;
                appendPadTruncate(out, e.dir ? e.name + "/" : e.name, c_width);
                out += "\x1b[0m";
            } else {
                out.append(c_width, ' ');
            }
        }

        // Child/Preview Pane (Always refresh on change)
        if (multi) {
            int r_start = w1 + w2 + 2;
            int r_width = w3 - 2;

            if (!partial) {
                appendCursor(out, row, w1 + w2);
                out += "\x1b[90m│\x1b[0m";
            }
            appendCursor(out, row, r_start);
            out += "\x1b[K"; 

            if (!entries.empty()) {
                auto &e = entries[selected];
                if (e.dir) {
                    if (i < (int)childEntries.size()) {
                        auto &ce = childEntries[i];
                        std::string color = "\x1b[0m";
                        if (ce.dir) color = "\x1b[1;34m";
                        else if (ce.link) color = "\x1b[1;36m";
                        else if (ce.exec) color = "\x1b[1;32m";
                        out += color;
                        appendTruncate(out, ce.dir ? ce.name + "/" : ce.name, r_width);
                        out += "\x1b[0m";
                    }
                } else {
                    if (!e.isText && !e.isKnownText) {
                        std::vector<std::string> fileHelp;
                        if (e.isImg) {
                            fileHelp = {"Imagen", "────────────────────", "ENTER", "Abre con wachar", "", "m", "Marca para mover", "", "c", "Marca para copiar", "", "p", "Pegar", "", ":r", "Renombrar", "", ":d", "Eliminar", "", "/", "Buscar", "", "q", "Salir"};
                        } else if (e.isAud || e.isVid) {
                            fileHelp = {"Multimedia", "────────────────────", "ENTER", "Abre con mpv", "", "m", "Marca para mover", "", "c", "Marca para copiar", "", "p", "Pegar", "", ":r", "Renombrar", "", ":d", "Eliminar", "", "/", "Buscar", "", "q", "Salir"};
                        } else if (e.isPdfFile) {
                            fileHelp = {"Documento PDF", "────────────────────", "ENTER", "Abre con zathura", "", "m", "Marca para mover", "", "c", "Marca para copiar", "", "p", "Pegar", "", ":r", "Renombrar", "", ":d", "Eliminar", "", "/", "Buscar", "", "q", "Salir"};
                        }
                        if (i < (int)fileHelp.size()) {
                            appendTruncate(out, fileHelp[i], r_width);
                        }
                    }
                }
            }
        }
    }

    // Text Preview System (Cached to avoid re-reading files on every frame)
    if (multi && !entries.empty()) {
        auto &e = entries[selected];
        if (!e.dir && (e.isText || e.isKnownText)) {
            int r_start = w1 + w2 + 2;
            int r_width = w3 - 2;

            if (e.path != cached_preview_path) {
                cached_preview_path = e.path;
                cached_preview_lines.clear();
                int fd = open(e.path.c_str(), O_RDONLY);
                if (fd != -1) {
                    char buf[8192];
                    ssize_t n = read(fd, buf, sizeof(buf));
                    close(fd);
                    if (n > 0) {
                        std::string cur;
                        for (ssize_t b = 0; b < n && cached_preview_lines.size() < (size_t)visible; b++) {
                            if (buf[b] == '\n') {
                                cached_preview_lines.push_back(cur);
                                cur.clear();
                            } else if (buf[b] != '\r') {
                                cur += buf[b];
                            }
                        }
                        if (!cur.empty() && cached_preview_lines.size() < (size_t)visible) {
                            cached_preview_lines.push_back(cur);
                        }
                    }
                }
            }

            for (size_t l = 0; l < cached_preview_lines.size() && l < (size_t)visible; l++) {
                appendCursor(out, l + 3, r_start);
                std::string pfx;
                size_t line = l + 1;
                if (line < 10)
                    pfx += "  ";
                else if (line < 100)
                    pfx += " ";

                appendInt(pfx, line);
                pfx += " │ ";
                int w = r_width - (int)pfx.size();
                out += "\x1b[90m" + pfx + "\x1b[0m";
                appendTruncate(out, cached_preview_lines[l], w > 0 ? w : 0);
            }
        }
    }
}

void drawStatus(std::string &out) {
    appendCursor(out, rows - 1, 1);
    out += "\x1b[90m";
    for (int i = 0; i < cols; i++) out += "─";
    out += "\x1b[0m\x1b[K";

    appendCursor(out, rows, 1);
    out += "\x1b[K";

    std::string left = "  ";
    if (commandMode) {
        if (!command.empty() && command[0] == '/') left += "SEARCH " + command;
        else left += "COMMAND :" + command;
    } else {
        left += "NORMAL";
    }
    out += "\x1b[1m" + left + "\x1b[0m";

    std::string right;
    appendInt(right, entries.empty() ? 0 : selected + 1);
    right += "/";
    appendInt(right, entries.size());
    right += "  ";

    int right_pos = cols - right.length() + 1;
    if (right_pos < 1) right_pos = 1;

    std::string mid = "";
    if (clip.active) mid = (clip.move ? "[MOVE] " : "[COPY] ") + basename(clip.path);

    if (!mid.empty()) {
        int mid_pos = (cols / 2) - (mid.length() / 2);
        if (mid_pos > (int)left.length() + 2 && mid_pos + (int)mid.length() < right_pos) {
            appendCursor(out, rows, mid_pos);
            out += "\x1b[33m" + mid + "\x1b[0m";
        }
    }
    appendCursor(out, rows, right_pos);
    out += "\x1b[1m" + right + "\x1b[0m";
}

void refresh() {
    updateSize();
    g_out.clear();
    if (g_out.capacity() < 32768) g_out.reserve(32768);

    g_out += "\x1b[?25l";

    bool partial = !force_redraw && (scroll == prev_scroll) && (cwd == old_cwd);

    if (!partial) drawPath(g_out);
    drawEntries(g_out, partial);
    drawStatus(g_out);

    ssize_t ign = write(STDOUT_FILENO, g_out.data(), g_out.size());
    (void)ign;

    prev_selected = selected;
    prev_scroll = scroll;
    old_cwd = cwd;
    force_redraw = false;
}

void showHelp() {
    clear();
    std::string out;
    out.reserve(2048);
    out += "\x1b[1;1H\x1b[1m  AYUDA\x1b[0m\r\n";
    out += "\x1b[2;1H\x1b[90m";
    for (int i = 0; i < cols; i++) out += "─";
    out += "\x1b[0m\x1b[K\x1b[3;1H\r\n  NAVEGACIÓN\r\n\r\n";
    out += "  ↑ ↓     Mover selección\r\n  ←       Volver\r\n  →       Entrar directorio\r\n  ENTER   Abrir archivo\r\n\r\n";
    out += "  COMANDOS\r\n\r\n  :       Entrar en modo comando\r\n  :n      Crear archivo\r\n  :r      Renombrar\r\n  :d      Eliminar\r\n";
    out += "  m       Mover\r\n  c       Copiar\r\n  p       Pegar elemento marcado\r\n  /       Buscar recursivamente\r\n  //      Buscar desde origen\r\n  q       Salir\r\n  ESC     Cancelar\r\n\r\n";
    out += "  PROGRAMAS UTILIZADOS\r\n\r\n";
    const char *progs[][2] = {{"Texto", "nvim"}, {"C/C++", "nvim"}, {"Markdown", "nvim"}, {"JSON", "nvim"}, {"YAML", "nvim"}, {"XML", "nvim"}, {"HTML", "nvim"}, {"Makefile", "nvim"}, {"README", "nvim"}, {"LICENSE", "nvim"}, {"Imágenes", "wachar"}, {"PDF", "zathura"}, {"Vídeo", "mpv"}, {"Audio", "mpv"}};
    for (const auto &p : progs) { out += "  "; out += p[0]; out += "\r\n    "; out += p[1]; out += "\r\n\r\n"; }
    ssize_t ign = write(STDOUT_FILENO, out.data(), out.size());
    (void)ign;
    readKey();
    clear();
}

void clearClipboard() {
    clip.active = false;
    clip.move = false;
    clip.path.clear();
    force_redraw = true;
}

void reload() {
    load();
    if (entries.empty()) { selected = 0; scroll = 0; return; }
    if (selected >= entries.size()) selected = entries.size() - 1;
}

// --- Operaciones Mutables y Caché Interna ---
void paste() {
    if (!clip.active) return;
    std::string dst = join(cwd, basename(clip.path));
    if (clip.path == dst || access(dst.c_str(), F_OK) == 0) return;

    bool success = false;
    if (clip.move) {
        if (rename(clip.path.c_str(), dst.c_str()) == 0) {
            success = true;
            clearClipboard();
        }
    } else {
        struct stat st;
        if (stat(clip.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            copy_dir_posix(clip.path, dst);
            success = true;
        } else {
            if (copy_file_posix(clip.path.c_str(), dst.c_str()) == 0) success = true;
        }
    }

    if (success) {
        entries.push_back(createEntryFromPath(dst));
        std::sort(entries.begin(), entries.end(), cmp);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].path == dst) { selected = i; break; }
        }
        force_redraw = true;
    }
}

// --- Recursión de Búsqueda sin Allocaciones Redundantes ---
bool recSearch(std::string &path_buf, size_t rel_idx, const std::string &query, std::string &result) {
    DIR *dir = opendir(path_buf.c_str());
    if (!dir) return false;
    struct dirent *d;
    size_t orig_len = path_buf.size();

    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;

        path_buf.push_back('/');
        path_buf.append(d->d_name);

        const char *rel_path = path_buf.c_str() + rel_idx;
        if (strstr(rel_path, query.c_str())) {
            result = path_buf;
            closedir(dir);
            return true;
        }

        struct stat st;
        if (lstat(path_buf.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (recSearch(path_buf, rel_idx, query, result)) {
                closedir(dir);
                return true;
            }
        }
        path_buf.resize(orig_len); // Rollback del buffer para evitar allocations adicionales
    }
    closedir(dir);
    return false;
}

bool recursiveSearch(const std::string &root, const std::string &query, std::string &result) {
    std::string buf;
    buf.reserve(PATH_MAX);
    buf = root;
    size_t rel_idx = root.empty() ? 0 : root.length() + 1;
    if (root == "/") rel_idx = 1;
    return recSearch(buf, rel_idx, query, result);
}

void updateCwd() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf))) cwd = buf;
}

void search() {
    std::string text;
    std::string orig_cwd = cwd;
    size_t orig_selected = selected;
    std::string orig_search_root = entries.empty() ? cwd : (entries[selected].dir ? entries[selected].path : cwd);
    std::string search_root = orig_search_root;
    bool double_slash = false;
    force_redraw = true;

    while (true) {
        commandMode = true;
        command = (double_slash ? "//" : "/") + text;
        refresh();
        int k = readKey();
        if (k == KEY_ESC) {
            commandMode = false;
            command.clear();
            if (cwd != orig_cwd && chdir(orig_cwd.c_str()) == 0) { updateCwd(); load(); }
            selected = orig_selected;
            if (selected < scroll) scroll = selected;
            if (selected >= scroll + rows - 4) scroll = selected - (rows - 4) + 1;
            force_redraw = true;
            return;
        }
        if (k == KEY_ENTER) {
            commandMode = false;
            command.clear();
            force_redraw = true;
            return;
        }
        if (k == KEY_BACKSPACE) {
            if (!text.empty()) { text.pop_back(); force_redraw = true; }
            else if (double_slash) { double_slash = false; search_root = orig_search_root; force_redraw = true; }
        } else if (k >= 32 && k < 127) {
            if (text.empty() && k == '/' && !double_slash) {
                double_slash = true;
                search_root = orig_cwd;
                force_redraw = true;
                continue;
            }
            text.push_back((char)k);
            force_redraw = true;
        }

        if (!text.empty() && force_redraw) {
            std::string found_path;
            if (recursiveSearch(search_root, text, found_path)) {
                std::string dir = dirName(found_path);
                if (cwd != dir && chdir(dir.c_str()) == 0) { updateCwd(); load(); }
                std::string fname = basename(found_path);
                for (size_t i = 0; i < entries.size(); i++) {
                    if (entries[i].name == fname) {
                        selected = i;
                        if (selected < scroll) scroll = selected;
                        if (selected >= scroll + rows - 4) scroll = selected - (rows - 4) + 1;
                        break;
                    }
                }
            }
        } else if (force_redraw) {
            if (cwd != orig_cwd && chdir(orig_cwd.c_str()) == 0) { updateCwd(); load(); }
            selected = orig_selected;
            if (selected < scroll) scroll = selected;
            if (selected >= scroll + rows - 4) scroll = selected - (rows - 4) + 1;
        }
    }
}

void runCommand();

void commandInput() {
    command.clear();
    commandMode = true;
    force_redraw = true;
    while (commandMode) {
        refresh();
        int k = readKey();
        if (k == KEY_ESC) { commandMode = false; force_redraw = true; return; }
        if (k == KEY_BACKSPACE) {
            if (!command.empty()) { command.pop_back(); force_redraw = true; }
            continue;
        }
        if (k == KEY_ENTER) {
            commandMode = false;
            runCommand();
            command.clear();
            force_redraw = true;
            return;
        }
        if (k >= 32 && k < 127) { command.push_back((char)k); force_redraw = true; }
    }
}

void cmdNew(const std::string &name) {
    if (name.empty()) return;
    std::string target = join(cwd, name);
    int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd != -1) {
        close(fd);
        entries.push_back(createEntryFromPath(target));
        std::sort(entries.begin(), entries.end(), cmp);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name == name) { selected = i; break; }
        }
        force_redraw = true;
    }
}

void cmdRename(const std::string &name) {
    if (entries.empty() || name.empty()) return;
    std::string target = join(cwd, name);
    if (rename(entries[selected].path.c_str(), target.c_str()) == 0) {
        entries[selected].name = name;
        entries[selected].path = target;
        populatePrecomputed(entries[selected]);
        std::sort(entries.begin(), entries.end(), cmp);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name == name) { selected = i; break; }
        }
        force_redraw = true;
    }
}

void cmdDelete() {
    if (entries.empty()) return;
    if (entries[selected].dir) remove_all_posix(entries[selected].path);
    else unlink(entries[selected].path.c_str());

    entries.erase(entries.begin() + selected);
    if (selected >= entries.size() && !entries.empty()) selected = entries.size() - 1;
    else if (entries.empty()) selected = 0;
    force_redraw = true;
}

void runCommand() {
    if (command.empty()) return;
    if (command[0] == 'n') { if (command.size() > 2) cmdNew(command.substr(2)); return; }
    if (command[0] == 'r') { if (command.size() > 2) cmdRename(command.substr(2)); return; }
    if (command == "d") { cmdDelete(); return; }
    if (command == "q") { running = false; return; }
    if (command == "reload") { reload(); return; }
}

void launch(const char *prog, const std::string &file) {
    disableRaw();
    pid_t pid = fork();
    if (pid == 0) {
        execlp(prog, prog, file.c_str(), nullptr);
        _exit(1);
    }
    waitpid(pid, nullptr, 0);
    enableRaw();
    force_redraw = true;
}

void enter() {
    if (entries.empty() || !entries[selected].dir) return;
    historyPath.push_back(cwd);
    historyCursor.push_back(selected);
    if (chdir(entries[selected].name.c_str()) == 0) {
        updateCwd();
        selected = 0; scroll = 0;
        load();
    }
}

void back() {
    if (cwd == "/") return;
    std::string old = cwd;
    if (chdir("..") != 0) return;
    updateCwd();
    load();
    if (!historyPath.empty() && historyPath.back() == cwd) {
        selected = historyCursor.back();
        historyPath.pop_back();
        historyCursor.pop_back();
        return;
    }
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].path == old) { selected = i; break; }
    }
}

void markMove() {
    if (entries.empty()) return;
    clip.path = entries[selected].path;
    clip.move = true; clip.active = true;
    force_redraw = true;
}

void markCopy() {
    if (entries.empty()) return;
    clip.path = entries[selected].path;
    clip.move = false; clip.active = true;
    force_redraw = true;
}

void open() {
    if (entries.empty()) return;
    auto &e = entries[selected];
    if (e.dir) { enter(); return; }

    const char *prog = nullptr;
    if (e.isImg) prog = "wachar";
    else if (e.isText || e.isKnownText) prog = "nvim";
    else if (e.isAud || e.isVid) prog = "mpv";
    else if (e.isPdfFile) prog = "zathura";

    if (prog) launch(prog, e.path);
}

void loop() {
    while (running) {
        refresh();
        switch (readKey()) {
            case 'q': running = false; break;
            case '/': search(); break;
            case KEY_ESC: clearClipboard(); break;
            case 'r': reload(); break;
            case ':': commandInput(); break;
            case 'm': markMove(); break;
            case 'c': markCopy(); break;
            case 'p': paste(); break;
            case 'h': showHelp(); break;
            case KEY_UP: if (selected) selected--; break;
            case KEY_DOWN: if (selected + 1 < entries.size()) selected++; break;
            case KEY_RIGHT: enter(); break;
            case KEY_LEFT: back(); break;
            case KEY_ENTER: open(); break;
        }
    }
}

int main() {
    enableRaw();
    updateCwd();
    load();
    clear();
    loop();
    return 0;
}
