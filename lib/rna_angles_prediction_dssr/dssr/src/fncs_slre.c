#include "x3dna.h"
struct slre {
    unsigned char code[256];
    unsigned char data[256];
    int code_size;
    int data_size;
    int num_caps;
    int anchored;
    enum slre_option options;
    const char *error_string;
};
struct cap {
    const char *ptr;
    int len;
};
enum {
    END, BRANCH, ANY, EXACT, ANYOF, ANYBUT, OPEN, CLOSE, BOL, EOL, STAR, PLUS,
    STARQ, PLUSQ, QUEST, SPACE, NONSPACE, DIGIT
};
static const char *meta_characters = "|.^$*+?()[\\";
static const char *error_no_match = "No match";
static void set_jump_offset(struct slre *r, int pc, int offset)
{
    assert(offset < r->code_size);
    if (r->code_size - offset > 0xff) {
        r->error_string = "Jump offset is too big";
    } else {
        r->code[pc] = (unsigned char) (r->code_size - offset);
    }
}

static void emit(struct slre *r, int code)
{
    if (r->code_size >= (int) NELEMS(r->code)) {
        r->error_string = "RE is too long (code overflow)";
    } else {
        r->code[r->code_size++] = (unsigned char) code;
    }
}

static void store_char_in_data(struct slre *r, int ch)
{
    if (r->data_size >= (int) sizeof(r->data)) {
        r->error_string = "RE is too long (data overflow)";
    } else {
        r->data[r->data_size++] = ch;
    }
}

static void exact(struct slre *r, const char **re)
{
    int old_data_size = r->data_size;
    while (**re != '\0' && (strchr(meta_characters, **re)) == NULL) {
        store_char_in_data(r, *(*re)++);
    }
    emit(r, EXACT);
    emit(r, old_data_size);
    emit(r, r->data_size - old_data_size);
}

static int get_escape_char(const char **re)
{
    int res;
    switch (*(*re)++) {
    case 'n':
        res = '\n';
        break;
    case 'r':
        res = '\r';
        break;
    case 't':
        res = '\t';
        break;
    case '0':
        res = 0;
        break;
    case 'S':
        res = NONSPACE << 8;
        break;
    case 's':
        res = SPACE << 8;
        break;
    case 'd':
        res = DIGIT << 8;
        break;
    default:
        res = (*re)[-1];
        break;
    }
    return res;
}

static void anyof(struct slre *r, const char **re)
{
    int esc, old_data_size = r->data_size, op = ANYOF;
    if (**re == '^') {
        op = ANYBUT;
        (*re)++;
    }
    while (**re != '\0')
        switch (*(*re)++) {
        case ']':
            emit(r, op);
            emit(r, old_data_size);
            emit(r, r->data_size - old_data_size);
            return;
            break;
        case '\\':
            esc = get_escape_char(re);
            if ((esc & 0xff) == 0) {
                store_char_in_data(r, 0);
                store_char_in_data(r, esc >> 8);
            } else {
                store_char_in_data(r, esc);
            }
            break;
        default:
            store_char_in_data(r, (*re)[-1]);
            break;
        }
    r->error_string = "No closing ']' bracket";
}

static void relocate(struct slre *r, int begin, int shift)
{
    emit(r, END);
    memmove(r->code + begin + shift, r->code + begin, r->code_size - begin);
    r->code_size += shift;
}

static void quantifier(struct slre *r, int prev, int op)
{
    if (r->code[prev] == EXACT && r->code[prev + 2] > 1) {
        r->code[prev + 2]--;
        emit(r, EXACT);
        emit(r, r->code[prev + 1] + r->code[prev + 2]);
        emit(r, 1);
        prev = r->code_size - 3;
    }
    relocate(r, prev, 2);
    r->code[prev] = op;
    set_jump_offset(r, prev + 1, prev);
}

static void exact_one_char(struct slre *r, int ch)
{
    emit(r, EXACT);
    emit(r, r->data_size);
    emit(r, 1);
    store_char_in_data(r, ch);
}

static void fixup_branch(struct slre *r, int fixup)
{
    if (fixup > 0) {
        emit(r, END);
        set_jump_offset(r, fixup, fixup - 2);
    }
}

static void compile(struct slre *r, const char **re)
{
    int op, esc, branch_start, last_op, fixup, cap_no, level;
    fixup = 0;
    level = r->num_caps;
    branch_start = last_op = r->code_size;
    for (;;)
        switch (*(*re)++) {
        case '\0':
            (*re)--;
            return;
            break;
        case '^':
            emit(r, BOL);
            break;
        case '$':
            emit(r, EOL);
            break;
        case '.':
            last_op = r->code_size;
            emit(r, ANY);
            break;
        case '[':
            last_op = r->code_size;
            anyof(r, re);
            break;
        case '\\':
            last_op = r->code_size;
            esc = get_escape_char(re);
            if (esc & 0xff00) {
                emit(r, esc >> 8);
            } else {
                exact_one_char(r, esc);
            }
            break;
        case '(':
            last_op = r->code_size;
            cap_no = ++r->num_caps;
            emit(r, OPEN);
            emit(r, cap_no);
            compile(r, re);
            if (*(*re)++ != ')') {
                r->error_string = "No closing bracket";
                return;
            }
            emit(r, CLOSE);
            emit(r, cap_no);
            break;
        case ')':
            (*re)--;
            fixup_branch(r, fixup);
            if (level == 0) {
                r->error_string = "Unbalanced brackets";
                return;
            }
            return;
            break;
        case '+':
        case '*':
            op = (*re)[-1] == '*' ? STAR : PLUS;
            if (**re == '?') {
                (*re)++;
                op = op == STAR ? STARQ : PLUSQ;
            }
            quantifier(r, last_op, op);
            break;
        case '?':
            quantifier(r, last_op, QUEST);
            break;
        case '|':
            fixup_branch(r, fixup);
            relocate(r, branch_start, 3);
            r->code[branch_start] = BRANCH;
            set_jump_offset(r, branch_start + 1, branch_start);
            fixup = branch_start + 2;
            r->code[fixup] = 0xff;
            break;
        default:
            (*re)--;
            last_op = r->code_size;
            exact(r, re);
            break;
        }
}

static const char *compile2(struct slre *r, const char *re)
{
    r->error_string = NULL;
    r->code_size = r->data_size = r->num_caps = r->anchored = 0;
    if (*re == '^') {
        r->anchored++;
    }
    emit(r, OPEN);
    emit(r, 0);
    while (*re != '\0') {
        compile(r, &re);
    }
    if (r->code[2] == BRANCH) {
        fixup_branch(r, 4);
    }
    emit(r, CLOSE);
    emit(r, 0);
    emit(r, END);
    return r->error_string;
}

static const char *match(const struct slre *, int, const char *, int, int *,
                         struct cap *);
static void loop_greedy(const struct slre *r, int pc, const char *s, int len, int *ofs)
{
    int saved_offset, matched_offset;
    saved_offset = matched_offset = *ofs;
    while (!match(r, pc + 2, s, len, ofs, NULL)) {
        saved_offset = *ofs;
        if (!match(r, pc + r->code[pc + 1], s, len, ofs, NULL)) {
            matched_offset = saved_offset;
        }
        *ofs = saved_offset;
    }
    *ofs = matched_offset;
}

static void loop_non_greedy(const struct slre *r, int pc, const char *s, int len,
                            int *ofs)
{
    int saved_offset = *ofs;
    while (!match(r, pc + 2, s, len, ofs, NULL)) {
        saved_offset = *ofs;
        if (!match(r, pc + r->code[pc + 1], s, len, ofs, NULL))
            break;
    }
    *ofs = saved_offset;
}

static int is_any_of(const unsigned char *p, int len, const char *s, int *ofs)
{
    int i, ch;
    ch = s[*ofs];
    for (i = 0; i < len; i++)
        if (p[i] == ch) {
            (*ofs)++;
            return 1;
        }
    return 0;
}

static int is_any_but(const unsigned char *p, int len, const char *s, int *ofs)
{
    int i, ch;
    ch = s[*ofs];
    for (i = 0; i < len; i++)
        if (p[i] == ch) {
            return 0;
        }
    (*ofs)++;
    return 1;
}

static int lowercase(const char *s)
{
    return tolower(*(const unsigned char *) s);
}

static int casecmp(const void *p1, const void *p2, size_t len)
{
    const char *s1 = p1, *s2 = p2;
    int diff = 0;
    if (len > 0)
        do {
            diff = lowercase(s1++) - lowercase(s2++);
        } while (diff == 0 && s1[-1] != '\0' && --len > 0);
    return diff;
}

static const char *match(const struct slre *r, int pc, const char *s, int len,
                         int *ofs, struct cap *caps)
{
    int n, saved_offset;
    const char *error_string = NULL;
    int (*cmp)(const void *string1, const void *string2, size_t len0);
    while (error_string == NULL && r->code[pc] != END) {
        assert(pc < r->code_size);
        assert(pc < (int) NELEMS(r->code));
        switch (r->code[pc]) {
        case BRANCH:
            saved_offset = *ofs;
            error_string = match(r, pc + 3, s, len, ofs, caps);
            if (error_string != NULL) {
                *ofs = saved_offset;
                error_string = match(r, pc + r->code[pc + 1], s, len, ofs, caps);
            }
            pc += r->code[pc + 2];
            break;
        case EXACT:
            error_string = error_no_match;
            n = r->code[pc + 2];
            cmp = r->options & SLRE_CASE_INSENSITIVE ? casecmp : memcmp;
            if (n <= len - *ofs && !cmp(s + *ofs, r->data + r->code[pc + 1], n)) {
                (*ofs) += n;
                error_string = NULL;
            }
            pc += 3;
            break;
        case QUEST:
            error_string = NULL;
            saved_offset = *ofs;
            if (match(r, pc + 2, s, len, ofs, caps) != NULL) {
                *ofs = saved_offset;
            }
            pc += r->code[pc + 1];
            break;
        case STAR:
            error_string = NULL;
            loop_greedy(r, pc, s, len, ofs);
            pc += r->code[pc + 1];
            break;
        case STARQ:
            error_string = NULL;
            loop_non_greedy(r, pc, s, len, ofs);
            pc += r->code[pc + 1];
            break;
        case PLUS:
            if ((error_string = match(r, pc + 2, s, len, ofs, caps)) != NULL)
                break;
            loop_greedy(r, pc, s, len, ofs);
            pc += r->code[pc + 1];
            break;
        case PLUSQ:
            if ((error_string = match(r, pc + 2, s, len, ofs, caps)) != NULL)
                break;
            loop_non_greedy(r, pc, s, len, ofs);
            pc += r->code[pc + 1];
            break;
        case SPACE:
            error_string = error_no_match;
            if (*ofs < len && isspace(((unsigned char *) s)[*ofs])) {
                (*ofs)++;
                error_string = NULL;
            }
            pc++;
            break;
        case NONSPACE:
            error_string = error_no_match;
            if (*ofs < len && !isspace(((unsigned char *) s)[*ofs])) {
                (*ofs)++;
                error_string = NULL;
            }
            pc++;
            break;
        case DIGIT:
            error_string = error_no_match;
            if (*ofs < len && isdigit(((unsigned char *) s)[*ofs])) {
                (*ofs)++;
                error_string = NULL;
            }
            pc++;
            break;
        case ANY:
            error_string = error_no_match;
            if (*ofs < len) {
                (*ofs)++;
                error_string = NULL;
            }
            pc++;
            break;
        case ANYOF:
            error_string = error_no_match;
            if (*ofs < len)
                error_string = is_any_of(r->data + r->code[pc + 1], r->code[pc + 2],
                                         s, ofs) ? NULL : error_no_match;
            pc += 3;
            break;
        case ANYBUT:
            error_string = error_no_match;
            if (*ofs < len)
                error_string = is_any_but(r->data + r->code[pc + 1], r->code[pc + 2],
                                          s, ofs) ? NULL : error_no_match;
            pc += 3;
            break;
        case BOL:
            error_string = *ofs == 0 ? NULL : error_no_match;
            pc++;
            break;
        case EOL:
            error_string = *ofs == len ? NULL : error_no_match;
            pc++;
            break;
        case OPEN:
            if (caps != NULL)
                caps[r->code[pc + 1]].ptr = s + *ofs;
            pc += 2;
            break;
        case CLOSE:
            if (caps != NULL)
                caps[r->code[pc + 1]].len = (s + *ofs) - caps[r->code[pc + 1]].ptr;
            pc += 2;
            break;
        case END:
            pc++;
            break;
        default:
            fprintf(stderr, "unknown cmd (%d) at %d\n", r->code[pc], pc);
            assert(0);
            break;
        }
    }
    return error_string;
}

static const char *match2(const struct slre *r, const char *buf, int len,
                          struct cap *caps)
{
    int i, ofs = 0;
    const char *error_string = error_no_match;
    if (r->anchored) {
        error_string = match(r, 0, buf, len, &ofs, caps);
    } else {
        for (i = 0; i < len && error_string != NULL; i++) {
            ofs = i;
            error_string = match(r, 0, buf, len, &ofs, caps);
        }
    }
    return error_string;
}

static const char *capture_float(const struct cap *cap, void *p, size_t len)
{
    const char *fmt;
    char buf[BUF512];
    switch (len) {
    case sizeof(float):
        fmt = "f";
        break;
    case sizeof(double):
        fmt = "lf";
        break;
    default:
        return "SLRE_FLOAT: unsupported size";
    }
    sprintf(buf, "%%%d%s", cap->len, fmt);
    return sscanf(cap->ptr, buf, p) == 1 ? NULL : "SLRE_FLOAT: capture failed";
}

static const char *capture_string(const struct cap *cap, void *p, size_t len)
{
    if ((int) len <= cap->len) {
        return "SLRE_STRING: buffer size too small";
    }
    memcpy(p, cap->ptr, cap->len);
    ((char *) p)[cap->len] = '\0';
    return NULL;
}

static const char *capture_int(const struct cap *cap, void *p, size_t len)
{
    const char *fmt;
    char buf[BUF512];
    switch (len) {
    case sizeof(char):
        fmt = "hh";
        break;
    case sizeof(long int):
        fmt = "ld";
        break;
    default:
        return "SLRE_INT: unsupported size";
    }
    sprintf(buf, "%%%d%s", cap->len, fmt);
    return sscanf(cap->ptr, buf, p) == 1 ? NULL : "SLRE_INT: capture failed";
}

static const char *capture(const struct cap *caps, int num_caps, va_list ap)
{
    int i, type;
    size_t size;
    void *p;
    const char *err = NULL;
    for (i = 0; i < num_caps; i++) {
        type = va_arg(ap, int);
        size = va_arg(ap, size_t);
        p = va_arg(ap, void *);
        switch (type) {
        case SLRE_INT:
            err = capture_int(&caps[i], p, size);
            break;
        case SLRE_FLOAT:
            err = capture_float(&caps[i], p, size);
            break;
        case SLRE_STRING:
            err = capture_string(&caps[i], p, size);
            break;
        default:
            err = "Unknown type, expected SLRE_(INT|FLOAT|STRING)";
            break;
        }
    }
    return err;
}

const char *slre_match(enum slre_option options, const char *re, const char *buf,
                       int buf_len, ...)
{
    struct slre slre;
    struct cap caps[20];
    va_list ap;
    const char *error_string = NULL;
    slre.options = options;
    if ((error_string = compile2(&slre, re)) == NULL &&
        (error_string = match2(&slre, buf, buf_len, caps)) == NULL) {
        va_start(ap, buf_len);
        error_string = capture(caps + 1, slre.num_caps, ap);
        va_end(ap);
    }
    return error_string;
}

int lux_match(enum slre_option options, const char *re, const char *buf)
{
    struct slre slre;
    struct cap caps[32];
    int buf_len = strlen(buf);
    slre.options = options;
    return compile2(&slre, re) == NULL && match2(&slre, buf, buf_len, caps) == NULL;
}

int lux_bcmatch(const char *buf, const char *re)
{
    return lux_match(SLRE_CASE_SENSITIVE, re, buf);
}

int lux_ncmatch(const char *buf, const char *re)
{
    return lux_match(SLRE_CASE_INSENSITIVE, re, buf);
}
