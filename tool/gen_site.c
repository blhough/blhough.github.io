/*
 * Static site generator: reads src/site.txt, post sources under src/posts/,
 * and HTML templates under templates/.
 * Usage: gen_site [-i src_dir] [-o out_dir] [-t tmpl_dir] [-c css_path]
 * Defaults: -i src -o out -t templates -c style.css
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_PATH 4096
#define MAX_LINE 8192
#define MAX_TITLE 4096
#define MAX_DATE 4096
#define MAX_ISO 32
#define MAX_SLUG 512
#define MAX_SITE_KEY 64
#define MAX_SITE_VAL 8192
#define MAX_SITE_KEYS 48
#define MAX_POSTS 512
#define MAX_FILE (512 * 1024)
#define MAX_HEADER (64 * 1024)

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("gen_site: ", stderr);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p) die("out of memory");
  return p;
}

static void *xrealloc(void *p, size_t n) {
  p = realloc(p, n);
  if (!p) die("out of memory");
  return p;
}

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) die("%s: cannot open: %s", path, strerror(errno));
  if (fseek(f, 0, SEEK_END) != 0) die("%s: fseek: %s", path, strerror(errno));
  long sz = ftell(f);
  if (sz < 0) die("%s: ftell: %s", path, strerror(errno));
  if (sz > (long)MAX_FILE) die("%s: file too large (max %d bytes)", path, MAX_FILE);
  rewind(f);
  char *buf = xmalloc((size_t)sz + 1);
  size_t n = fread(buf, 1, (size_t)sz, f);
  if (n != (size_t)sz && ferror(f)) die("%s: read error", path);
  fclose(f);
  buf[n] = '\0';
  if (out_len) *out_len = n;
  return buf;
}

static void write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) die("%s: cannot create: %s", path, strerror(errno));
  if (len && fwrite(data, 1, len, f) != len) die("%s: write error", path);
  fclose(f);
}

static void copy_file(const char *src, const char *dst) {
  size_t len;
  char *buf = read_file(src, &len);
  write_file(dst, buf, len);
  free(buf);
}

static void mkdir_p(const char *path) {
  char tmp[MAX_PATH];
  size_t len = strnlen(path, sizeof(tmp) - 1);
  if (len >= sizeof(tmp)) die("path too long");
  memcpy(tmp, path, len + 1);
  for (size_t i = 1; i < len; i++) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) die("mkdir %s: %s", tmp, strerror(errno));
      tmp[i] = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) die("mkdir %s: %s", tmp, strerror(errno));
}

static void trim_crlf(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[n - 1] = '\0';
    n--;
  }
}

static char *trim_space(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
    *--end = '\0';
  }
  return s;
}

static void strip_cr_inplace(char *buf, size_t *len) {
  char *w = buf;
  for (size_t i = 0; i < *len; i++) {
    if (buf[i] != '\r') *w++ = buf[i];
  }
  *w = '\0';
  *len = (size_t)(w - buf);
}

static size_t html_escape(const char *in, char *out, size_t out_cap) {
  size_t w = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
    const char *rep = NULL;
    if (*p == '&')
      rep = "&amp;";
    else if (*p == '<')
      rep = "&lt;";
    else if (*p == '>')
      rep = "&gt;";
    else if (*p == '"')
      rep = "&quot;";
    if (rep) {
      size_t rl = strlen(rep);
      if (w + rl + 1 >= out_cap) die("html_escape: output overflow");
      memcpy(out + w, rep, rl);
      w += rl;
    } else {
      if (w + 2 >= out_cap) die("html_escape: output overflow");
      out[w++] = (char)*p;
    }
  }
  out[w] = '\0';
  return w;
}

typedef struct {
  char key[MAX_SITE_KEY];
  char val[MAX_SITE_VAL];
} SiteKV;

static SiteKV site[MAX_SITE_KEYS];
static size_t site_n;

static const char *site_get(const char *key) {
  for (size_t i = 0; i < site_n; i++) {
    if (strcmp(site[i].key, key) == 0) return site[i].val;
  }
  return "";
}

static void load_site(const char *path) {
  size_t len;
  char *buf = read_file(path, &len);
  strip_cr_inplace(buf, &len);
  char *p = buf;
  site_n = 0;
  while (*p) {
    char *line = p;
    char *nl = strchr(p, '\n');
    if (nl) {
      *nl = '\0';
      p = nl + 1;
    } else {
      p += strlen(p);
    }
    trim_crlf(line);
    line = trim_space(line);
    if (*line == '\0' || *line == '#') continue;
    char *colon = strchr(line, ':');
    if (!colon) {
      free(buf);
      die("%s: expected KEY: value line", path);
    }
    *colon = '\0';
    char *k = trim_space(line);
    char *v = trim_space(colon + 1);
    if (strlen(k) >= sizeof site[0].key) {
      free(buf);
      die("%s: key too long", path);
    }
    if (strlen(v) >= sizeof site[0].val) {
      free(buf);
      die("%s: value too long for key %s", path, k);
    }
    if (site_n >= MAX_SITE_KEYS) {
      free(buf);
      die("%s: too many keys", path);
    }
    strcpy(site[site_n].key, k);
    strcpy(site[site_n].val, v);
    site_n++;
  }
  free(buf);
}

typedef struct {
  char path[MAX_PATH];
  char slug[MAX_SLUG];
  char title[MAX_TITLE];
  char date[MAX_DATE];
  char iso[MAX_ISO];
  int sort_key; /* YYYYMMDD */
  char *body;
  size_t body_len;
} Post;

static int parse_iso(const char *iso, int *key_out) {
  if (strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') return -1;
  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) continue;
    if (!isdigit((unsigned char)iso[i])) return -1;
  }
  int y = (iso[0] - '0') * 1000 + (iso[1] - '0') * 100 + (iso[2] - '0') * 10 + (iso[3] - '0');
  int m = (iso[5] - '0') * 10 + (iso[6] - '0');
  int d = (iso[8] - '0') * 10 + (iso[9] - '0');
  if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
  *key_out = y * 10000 + m * 100 + d;
  return 0;
}

static int starts_with(const char *line, const char *prefix) {
  size_t n = strlen(prefix);
  return strncmp(line, prefix, n) == 0;
}

static void header_value(const char *line, const char *prefix, char *out, size_t outsz, const char *path) {
  size_t n = strlen(prefix);
  if (!starts_with(line, prefix)) die("%s: expected line starting with %s", path, prefix);
  const char *v = line + n;
  while (*v == ' ' || *v == '\t') v++;
  if (strlen(v) >= outsz) die("%s: value too long for %s", path, prefix);
  strcpy(out, v);
}

static void path_slug(const char *filepath, char *slug, size_t slug_sz) {
  const char *base = strrchr(filepath, '/');
  base = base ? base + 1 : filepath;
  const char *dot = strrchr(base, '.');
  if (!dot || strcmp(dot, ".post") != 0) die("%s: filename must end with .post", filepath);
  size_t blen = (size_t)(dot - base);
  if (blen == 0 || blen >= slug_sz) die("%s: bad slug", filepath);
  memcpy(slug, base, blen);
  slug[blen] = '\0';
}

static void parse_post(Post *post) {
  size_t len;
  char *buf = read_file(post->path, &len);
  if (len > MAX_HEADER + MAX_FILE) die("%s: file too large", post->path);
  strip_cr_inplace(buf, &len);

  size_t off = 0;
  if (len >= 3 && (unsigned char)buf[0] == 0xef && (unsigned char)buf[1] == 0xbb && (unsigned char)buf[2] == 0xbf) {
    off = 3;
  }

  size_t starts[4];
  size_t pos = off;
  for (int i = 0; i < 3; i++) {
    if (pos >= len) {
      free(buf);
      die("%s: missing header line %d (need TITLE, DATE, ISO_DATE)", post->path, i + 1);
    }
    starts[i] = pos;
    while (pos < len && buf[pos] != '\n') pos++;
    if (pos >= len && i < 2) {
      free(buf);
      die("%s: missing newline after header", post->path);
    }
    if (pos < len) {
      buf[pos] = '\0';
      pos++;
    }
  }
  starts[3] = pos;

  char *line1 = buf + starts[0];
  char *line2 = buf + starts[1];
  char *line3 = buf + starts[2];
  trim_crlf(line1);
  trim_crlf(line2);
  trim_crlf(line3);

  if (!starts_with(line1, "TITLE:")) {
    free(buf);
    die("%s: first line must be TITLE:", post->path);
  }
  if (!starts_with(line2, "DATE:")) {
    free(buf);
    die("%s: second line must be DATE:", post->path);
  }
  if (!starts_with(line3, "ISO_DATE:")) {
    free(buf);
    die("%s: third line must be ISO_DATE:", post->path);
  }

  header_value(line1, "TITLE:", post->title, sizeof post->title, post->path);
  header_value(line2, "DATE:", post->date, sizeof post->date, post->path);
  header_value(line3, "ISO_DATE:", post->iso, sizeof post->iso, post->path);

  if (parse_iso(post->iso, &post->sort_key) != 0) {
    free(buf);
    die("%s: invalid ISO_DATE (expected YYYY-MM-DD)", post->path);
  }

  post->body_len = len - starts[3];
  post->body = xmalloc(post->body_len + 1);
  memcpy(post->body, buf + starts[3], post->body_len);
  post->body[post->body_len] = '\0';
  /* trim leading newlines in body */
  char *b = post->body;
  while (*b == '\n' || *b == '\r') b++;
  if (b != post->body) {
    size_t shift = (size_t)(b - post->body);
    memmove(post->body, b, post->body_len - shift + 1);
    post->body_len -= shift;
  }
  free(buf);
  path_slug(post->path, post->slug, sizeof post->slug);
}

static int post_cmp(const void *a, const void *b) {
  const Post *pa = a;
  const Post *pb = b;
  if (pa->sort_key != pb->sort_key) return (pa->sort_key > pb->sort_key) ? -1 : 1;
  return strcmp(pa->slug, pb->slug);
}

typedef struct {
  char **items;
  size_t n, cap;
} StrList;

static void strlist_push(StrList *sl, const char *s) {
  if (sl->n + 1 > sl->cap) {
    sl->cap = sl->cap ? sl->cap * 2 : 32;
    sl->items = xrealloc(sl->items, sl->cap * sizeof *sl->items);
  }
  size_t n = strlen(s) + 1;
  char *p = xmalloc(n);
  memcpy(p, s, n);
  sl->items[sl->n++] = p;
}

static void collect_posts(const char *posts_dir, Post *posts, size_t *nposts) {
  DIR *d = opendir(posts_dir);
  if (!d) die("%s: cannot open: %s", posts_dir, strerror(errno));
  StrList paths = {0};
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') continue;
    size_t nl = strlen(de->d_name);
    if (nl < 6 || strcmp(de->d_name + nl - 5, ".post") != 0) continue;
    char full[MAX_PATH];
    int nw = snprintf(full, sizeof full, "%s/%s", posts_dir, de->d_name);
    if (nw < 0 || (size_t)nw >= sizeof full) die("path too long: %s/%s", posts_dir, de->d_name);
    strlist_push(&paths, full);
  }
  closedir(d);
  if (paths.n > MAX_POSTS) die("too many posts (max %d)", MAX_POSTS);
  for (size_t i = 0; i < paths.n; i++) {
    strcpy(posts[i].path, paths.items[i]);
    parse_post(&posts[i]);
    free(paths.items[i]);
  }
  free(paths.items);
  *nposts = paths.n;
}

typedef struct {
  const char *key;
  const char *value;
} TplVar;

static const char *tpl_lookup(const char *key, size_t key_len, TplVar *vars, size_t nvar) {
  for (size_t i = 0; i < nvar; i++) {
    if (strlen(vars[i].key) == key_len && memcmp(vars[i].key, key, key_len) == 0) return vars[i].value;
  }
  return NULL;
}

static char *apply_template(const char *tpl, TplVar *vars, size_t nvar) {
  size_t cap = strlen(tpl) * 4 + 65536;
  char *out = xmalloc(cap);
  size_t w = 0;
  const char *r = tpl;
  while (*r) {
    if (r[0] == '{' && r[1] == '{') {
      const char *start = r + 2;
      const char *end = strstr(start, "}}");
      if (!end) die("template: unclosed {{");
      size_t key_len = (size_t)(end - start);
      if (key_len >= 128) die("template: placeholder too long");
      char keybuf[128];
      memcpy(keybuf, start, key_len);
      keybuf[key_len] = '\0';
      const char *val = tpl_lookup(keybuf, key_len, vars, nvar);
      if (!val) die("template: unknown placeholder {{%s}}", keybuf);
      size_t vl = strlen(val);
      while (w + vl + 1 > cap) {
        cap *= 2;
        out = xrealloc(out, cap);
      }
      memcpy(out + w, val, vl);
      w += vl;
      r = end + 2;
    } else {
      if (w + 2 > cap) {
        cap *= 2;
        out = xrealloc(out, cap);
      }
      out[w++] = *r++;
    }
  }
  out[w] = '\0';
  return out;
}

static void path_join2(char *out, size_t outsz, const char *a, const char *b) {
  int n = snprintf(out, outsz, "%s/%s", a, b);
  if (n < 0 || (size_t)n >= outsz) die("path_join2 overflow");
}

#define MAX_VARS 24
#define ESC_CAP (MAX_TITLE * 6)

int main(int argc, char **argv) {
  const char *in_dir = "src";
  const char *out_dir = "out";
  const char *tmpl_dir = "templates";
  const char *css_path = "style.css";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      in_dir = argv[++i];
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      tmpl_dir = argv[++i];
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      css_path = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      fputs("Usage: gen_site [-i src_dir] [-o out_dir] [-t tmpl_dir] [-c css_path]\n"
            "Defaults: -i src -o out -t templates -c style.css\n"
            "Posts: <src_dir>/posts/*.post with lines TITLE:, DATE:, ISO_DATE: then HTML body.\n",
            stdout);
      return 0;
    } else {
      die("unknown argument: %s (try -h)", argv[i]);
    }
  }

  char site_path[MAX_PATH];
  path_join2(site_path, sizeof site_path, in_dir, "site.txt");
  load_site(site_path);

  char posts_dir[MAX_PATH];
  snprintf(posts_dir, sizeof posts_dir, "%s/posts", in_dir);

  Post posts[MAX_POSTS];
  size_t nposts = 0;
  collect_posts(posts_dir, posts, &nposts);
  qsort(posts, nposts, sizeof posts[0], post_cmp);

  mkdir_p(out_dir);
  char posts_out[MAX_PATH];
  snprintf(posts_out, sizeof posts_out, "%s/posts", out_dir);
  mkdir_p(posts_out);

  char tpl_post[MAX_PATH], tpl_index[MAX_PATH], tpl_archive[MAX_PATH];
  path_join2(tpl_post, sizeof tpl_post, tmpl_dir, "post.html");
  path_join2(tpl_index, sizeof tpl_index, tmpl_dir, "index.html");
  path_join2(tpl_archive, sizeof tpl_archive, tmpl_dir, "archive.html");

  char *template_post = read_file(tpl_post, NULL);
  char *template_index = read_file(tpl_index, NULL);
  char *template_archive = read_file(tpl_archive, NULL);

  const char *site_title = site_get("SITE_TITLE");
  const char *author = site_get("AUTHOR");
  const char *github = site_get("GITHUB_URL");
  const char *meta = site_get("META_DESCRIPTION");
  const char *home_h1 = site_get("HOME_H1");
  const char *home_h2 = site_get("HOME_H2");

  char site_title_esc[ESC_CAP];
  char author_esc[ESC_CAP];
  char meta_esc[ESC_CAP];
  html_escape(site_title, site_title_esc, sizeof site_title_esc);
  html_escape(author, author_esc, sizeof author_esc);
  html_escape(meta, meta_esc, sizeof meta_esc);

  /* --- emit each post --- */
  for (size_t i = 0; i < nposts; i++) {
    Post *p = &posts[i];
    char title_esc[ESC_CAP];
    char date_esc[ESC_CAP];
    html_escape(p->title, title_esc, sizeof title_esc);
    html_escape(p->date, date_esc, sizeof date_esc);

    size_t pt_len = strlen(title_esc) + strlen(site_title_esc) + 8;
    char *page_title = xmalloc(pt_len);
    snprintf(page_title, pt_len, "%s — %s", title_esc, site_title_esc);

    char meta_post[ESC_CAP];
    html_escape(p->title, meta_post, sizeof meta_post);

    TplVar vars[MAX_VARS];
    size_t nv = 0;
#define V(k, v)                                                                                    \
  do {                                                                                             \
    if (nv >= MAX_VARS) die("too many template vars");                                             \
    vars[nv].key = (k);                                                                            \
    vars[nv].value = (v);                                                                           \
    nv++;                                                                                          \
  } while (0)

    V("ROOT", "../");
    V("SITE_TITLE", site_title_esc);
    V("AUTHOR", author_esc);
    V("GITHUB_URL", github);
    V("PAGE_TITLE", page_title);
    V("META_DESCRIPTION", meta_post);
    V("HEADER_TITLE", title_esc);
    V("HEADER_SUB", date_esc);
    V("MAIN_HTML", p->body);

#undef V

    char *html = apply_template(template_post, vars, nv);
    char outpath[MAX_PATH];
    snprintf(outpath, sizeof outpath, "%s/posts/%s.html", out_dir, p->slug);
    write_file(outpath, html, strlen(html));
    free(html);
    free(page_title);
  }

  /* --- POST_LIST & ARCHIVE_LIST --- */
  size_t list_cap = 65536;
  char *post_list = xmalloc(list_cap);
  size_t plen = 0;
  char *arch_list = xmalloc(list_cap);
  size_t alen = 0;

  for (size_t i = 0; i < nposts; i++) {
    Post *p = &posts[i];
    char title_esc[ESC_CAP];
    char date_esc[ESC_CAP];
    html_escape(p->title, title_esc, sizeof title_esc);
    html_escape(p->date, date_esc, sizeof date_esc);

    char chunk[MAX_LINE * 4];
    int n1 = snprintf(chunk, sizeof chunk,
                      "<article>\n"
                      "<h3><a href=\"posts/%s.html\">%s</a></h3>\n"
                      "<p class=\"meta\"><span title=\"Published\">%s</span></p>\n"
                      "</article>\n",
                      p->slug, title_esc, date_esc);
    if (n1 < 0 || (size_t)n1 >= sizeof chunk) die("POST_LIST chunk overflow");
    while (plen + (size_t)n1 + 1 > list_cap) {
      list_cap *= 2;
      post_list = xrealloc(post_list, list_cap);
      arch_list = xrealloc(arch_list, list_cap);
    }
    memcpy(post_list + plen, chunk, (size_t)n1);
    plen += (size_t)n1;

    int n2 = snprintf(chunk, sizeof chunk,
                      "<li><time datetime=\"%s\">%s</time> <a href=\"posts/%s.html\">%s</a></li>\n", p->iso,
                      date_esc, p->slug, title_esc);
    if (n2 < 0 || (size_t)n2 >= sizeof chunk) die("ARCHIVE_LIST chunk overflow");
    while (alen + (size_t)n2 + 1 > list_cap) {
      list_cap *= 2;
      post_list = xrealloc(post_list, list_cap);
      arch_list = xrealloc(arch_list, list_cap);
    }
    memcpy(arch_list + alen, chunk, (size_t)n2);
    alen += (size_t)n2;
  }
  post_list[plen] = '\0';
  arch_list[alen] = '\0';

  /* index */
  {
    TplVar vars[MAX_VARS];
    size_t nv = 0;
#define V(k, v)                                                                                    \
  do {                                                                                             \
    if (nv >= MAX_VARS) die("too many template vars");                                             \
    vars[nv].key = (k);                                                                            \
    vars[nv].value = (v);                                                                           \
    nv++;                                                                                          \
  } while (0)
    V("ROOT", "");
    V("SITE_TITLE", site_title_esc);
    V("AUTHOR", author_esc);
    V("GITHUB_URL", github);
    V("PAGE_TITLE", site_title_esc);
    V("META_DESCRIPTION", meta_esc);
    V("HOME_H1", home_h1);
    V("HOME_H2", home_h2);
    V("POST_LIST", post_list);
#undef V
    char *html = apply_template(template_index, vars, nv);
    char outpath[MAX_PATH];
    path_join2(outpath, sizeof outpath, out_dir, "index.html");
    write_file(outpath, html, strlen(html));
    free(html);
  }

  /* archive */
  {
    size_t at_len = strlen("Archive — ") + strlen(site_title_esc) + 1;
    char *arch_page_title = xmalloc(at_len);
    snprintf(arch_page_title, at_len, "Archive — %s", site_title_esc);
    TplVar vars[MAX_VARS];
    size_t nv = 0;
#define V(k, v)                                                                                    \
  do {                                                                                             \
    if (nv >= MAX_VARS) die("too many template vars");                                             \
    vars[nv].key = (k);                                                                            \
    vars[nv].value = (v);                                                                           \
    nv++;                                                                                          \
  } while (0)
    V("ROOT", "");
    V("SITE_TITLE", site_title_esc);
    V("AUTHOR", author_esc);
    V("GITHUB_URL", github);
    V("PAGE_TITLE", arch_page_title);
    V("ARCHIVE_LIST", arch_list);
#undef V
    char *html = apply_template(template_archive, vars, nv);
    char outpath[MAX_PATH];
    path_join2(outpath, sizeof outpath, out_dir, "archive.html");
    write_file(outpath, html, strlen(html));
    free(html);
    free(arch_page_title);
  }

  free(post_list);
  free(arch_list);
  free(template_post);
  free(template_index);
  free(template_archive);

  for (size_t i = 0; i < nposts; i++) free(posts[i].body);

  /* static assets */
  char css_out[MAX_PATH];
  path_join2(css_out, sizeof css_out, out_dir, "style.css");
  copy_file(css_path, css_out);

  char about_src[MAX_PATH], about_dst[MAX_PATH];
  path_join2(about_src, sizeof about_src, in_dir, "about.html");
  path_join2(about_dst, sizeof about_dst, out_dir, "about.html");
  if (access(about_src, R_OK) == 0) {
    copy_file(about_src, about_dst);
  }

  return 0;
}
