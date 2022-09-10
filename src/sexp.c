#include "sexp.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct token_t token_t;
struct token_t {
  char type;       // [' ', '(', ')', 's', 'e'] s: string, e: eof
  const char *str; // points into source
  int len;
};

static token_t token_make(char type, const char *p, int plen) {
  assert(strchr(" ()s", type) || type == -1);
  token_t t = {type, p, plen};
  return t;
}

typedef struct scanner_t scanner_t;
struct scanner_t {
  const char *ptr;
  const char *end;
  int putback;
  token_t token;
};

static void scan_init(scanner_t *sp, const char *p, int plen);
static token_t *scan_next(scanner_t *sp);
static token_t *scan_match(scanner_t *sp, int type);
static token_t *scan_peek(scanner_t *sp);

typedef struct parser_t parser_t;
struct parser_t {
  scanner_t scanner;
  char errbuf[200];
};
static void parse_init(parser_t *pp, const char *buf, int len);
static sexp_object_t *parse_next(parser_t *pp);
static sexp_object_t *parse_list(parser_t *pp);
static sexp_object_t *parse_string(parser_t *pp);

static void parse_init(parser_t *pp, const char *buf, int len) {
  scan_init(&pp->scanner, buf, len);
  pp->errbuf[0] = 0;
}

static sexp_object_t *parse_string(parser_t *pp) {
  token_t *tok = scan_match(&pp->scanner, 's');
  if (!tok) {
    return 0;
  }
  const char *s = tok->str;
  int len = tok->len;

  // if quoted, take out the first and last "
  int quoted = (*s == '"');
  if (quoted) {
    s++;
    len -= 2;
  }

  // make a copy of s, and add extra byte for NUL
  char *p = malloc(len + 1);
  if (!p) {
    return 0;
  }
  memcpy(p, s, len);
  p[len] = 0;

  if (quoted) {
    // unescape two double-quote
    char *next = p;
    char *curr = p;
    char *q = p + len;
    for (; next < q; next++) {
      char ch = *next;
      if (ch == '"' && next + 1 < q && next[1] == '"') {
        next++;
      }
      *curr++ = ch;
    }
    *curr = 0;
  }

  sexp_string_t *ret = malloc(sizeof(*ret));
  if (!ret) {
    free(p);
    return 0;
  }

  ret->type = 'S';
  ret->str = p;
  return (sexp_object_t *)ret;
}

static sexp_object_t *parse_list(parser_t *pp) {
  sexp_list_t *list = 0;
  sexp_object_t *obj = 0;

  // parse ( [WS] [ item WS item WS item [WS] ] )
  scanner_t *sp = &pp->scanner;
  if (!scan_match(sp, '(')) {
    goto bail;
  }
  list = sexp_list_create();
  if (!list) {
    goto bail;
  }
  // skip white space after (
  while (scan_match(sp, ' '))
    ;

  // is this an empty list ?
  if (!scan_match(sp, ')')) {
    // fill in content...
    for (;;) {
      obj = parse_next(pp);
      if (!obj) {
        goto bail;
      }
      if (sexp_list_append_object(list, obj)) {
        goto bail;
      }
      obj = 0;

      int has_space = (0 != scan_match(sp, ' '));
      if (scan_match(sp, ')')) {
        break;
      }
      if (!has_space) {
        goto bail;
      }
    }
  }

  return (sexp_object_t *)list;

bail:
  if (obj) {
    sexp_release(obj);
  }
  if (list) {
    sexp_release((sexp_object_t *)list);
  }
  return 0;
}

static sexp_object_t *parse_next(parser_t *pp) {
again:
  scanner_t *sp = &pp->scanner;
  token_t *tok = scan_peek(sp);
  if (!tok) {
    return 0;
  }
  switch (tok->type) {
  case ' ':
    scan_next(sp);
    goto again;
  case 's':
    return parse_string(pp);
  case '(':
    return parse_list(pp);
  case 'e':
    return 0;
  default:
    return 0;
  }
}

sexp_object_t *sexp_parse(const char *buf, int len, const char **endp) {
  parser_t parser;
  parse_init(&parser, buf, len);
  sexp_object_t *ox = parse_next(&parser);
  *endp = parser.scanner.ptr;
  return ox;
}

static token_t *_scan_quoted(scanner_t *sp);
static token_t *_scan_unquoted(scanner_t *sp);
static token_t *_scan_whitespace(scanner_t *sp);
static token_t *_scan_comment(scanner_t *sp);

static void scan_init(scanner_t *sp, const char *p, int plen) {
  memset(sp, 0, sizeof(*sp));
  sp->ptr = p;
  sp->end = p + plen;
}

static token_t *scan_peek(scanner_t *sp) {
  token_t *ret = scan_next(sp);
  assert(!sp->putback);
  sp->putback = (ret != 0);
  return ret;
}

static token_t *scan_match(scanner_t *sp, int type) {
  token_t *ret = scan_next(sp);
  if (ret && ret->type != type) {
    // putback if no match
    assert(!sp->putback);
    sp->putback = 1;
    ret = 0;
  }
  return ret;
}

/*
static token_t* scan_match_any(scanner_t* sp, const char* types) {
  token_t* ret = scan_next(sp);
  if (ret) {
    if (!strchr(types, ret->type)) {
      scan_putback(sp);
      ret = 0;
    }
  }
  return ret;
}
*/

static token_t *scan_next(scanner_t *sp) {
  if (sp->putback) {
    sp->putback = 0;
    return &sp->token;
  }

  memset(&sp->token, 0, sizeof(sp->token));
  if (sp->ptr >= sp->end) {
    sp->token = token_make('e', 0, 0);
    return &sp->token;
  }

  switch (*sp->ptr) {
  case '"':
    return _scan_quoted(sp);
  case '(':
  case ')':
    sp->token = token_make(*sp->ptr++, 0, 0);
    return &sp->token;
  case ';':
    return _scan_comment(sp);
  case ' ':
  case '\t':
  case '\r':
  case '\n':
    return _scan_whitespace(sp);
  default:
    return _scan_unquoted(sp);
  }
}

static token_t *_scan_quoted(scanner_t *sp) {
  assert('"' == *sp->ptr);
  const char *p = sp->ptr + 1;
  const char *q = sp->end;
  for (; p < q; p++) {
    int ch = *p;
    if (ch != '"') {
      continue;
    }
    if (p + 1 < q && p[1] == '"') {
      // two double-quotes
      p++;
    } else {
      break;
    }
  }
  if (p >= q) {
    // unterminated quote
    return 0;
  }
  assert(*p == '"');
  p++;
  sp->token = token_make('s', sp->ptr, p - sp->ptr);
  sp->ptr = p;
  return &sp->token;
}

static token_t *_scan_unquoted(scanner_t *sp) {
  const char *p = sp->ptr;
  const char *q = sp->end;
  for (; p < q; p++) {
    int ch = *p;
    if (strchr(" \r\n\t()", ch)) {
      break;
    }
  }
  sp->token = token_make('s', sp->ptr, p - sp->ptr);
  sp->ptr = p;
  return &sp->token;
}

static token_t *_scan_whitespace(scanner_t *sp) {
  assert(strchr(" \r\n\t", *sp->ptr));
  const char *p = sp->ptr;
  const char *q = sp->end;
  // skip to first non-whitespace
  while (p < q && strchr(" \r\n\t", *p)) {
    p++;
  }
  sp->token = token_make(' ', 0, 0);
  sp->ptr = p;
  return &sp->token;
}

static token_t *_scan_comment(scanner_t *sp) {
  assert(';' == *sp->ptr);
  const char *p = sp->ptr;
  const char *q = sp->end;
  // skip to the first '\n'
  while (p < q && *p != '\n') {
    p++;
  }
  // return a whitespace
  sp->token = token_make(' ', 0, 0);
  // point to char after '\n'
  sp->ptr = (p < q ? p + 1 : q);
  return &sp->token;
}