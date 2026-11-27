#include "mbjs_runtime.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>

static MBJSRuntime rt;

static char *js_cstr_len(MBJSContext *ctx, MBJSValueConst v, size_t *out_len)
{
    size_t len = 0;
    const char *p = rt.ToCStringLen(ctx, &len, v);
    char *out;
    if (!p)
        return NULL;
    out = (char *)malloc(len + 1);
    if (!out) {
        rt.FreeCString(ctx, p);
        return NULL;
    }
    memcpy(out, p, len);
    out[len] = 0;
    rt.FreeCString(ctx, p);
    if (out_len)
        *out_len = len;
    return out;
}

static char *js_cstr(MBJSContext *ctx, MBJSValueConst v)
{
    return js_cstr_len(ctx, v, NULL);
}

static MBJSValue js_str(MBJSContext *ctx, const char *s, size_t len)
{
    if (!s)
        return rt.NewStringLen(ctx, "", 0);
    return rt.NewStringLen(ctx, s, len);
}

static MBJSValue js_error(MBJSContext *ctx, const char *what)
{
    rt.ThrowTypeError(ctx, "mochifs: %s", what);
    return MBJS_EXCEPTION; /* contract: a C func that threw MUST return MBJS_EXCEPTION */
}

static void append(char **buf, size_t *cap, size_t *len, const char *s, size_t n)
{
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t next = *cap ? *cap : 128;
        char *tmp;
        while (next < need)
            next *= 2;
        tmp = (char *)realloc(*buf, next);
        if (!tmp)
            return;
        *buf = tmp;
        *cap = next;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void append_json_string(char **buf, size_t *cap, size_t *len, const char *s)
{
    size_t i;
    append(buf, cap, len, "\"", 1);
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char esc[2];
            esc[0] = '\\';
            esc[1] = (char)c;
            append(buf, cap, len, esc, 2);
        } else if (c == '\n') {
            append(buf, cap, len, "\\n", 2);
        } else if (c == '\r') {
            append(buf, cap, len, "\\r", 2);
        } else if (c == '\t') {
            append(buf, cap, len, "\\t", 2);
        } else {
            append(buf, cap, len, (const char *)&c, 1);
        }
    }
    append(buf, cap, len, "\"", 1);
}

static MBJSValue fs_read_text(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    char *path;
    FILE *f;
    long size;
    char *buf;
    size_t got;
    MBJSValue out;
    (void)this_val;
    if (argc < 1)
        return js_error(ctx, "readText needs a path");
    path = js_cstr(ctx, argv[0]);
    if (!path)
        return js_error(ctx, "readText path conversion failed");
    f = fopen(path, "rb");
    if (!f) {
        free(path);
        return js_error(ctx, "readText could not open file");
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        free(path);
        return js_error(ctx, "readText could not stat file");
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        free(path);
        return js_error(ctx, "readText out of memory");
    }
    got = fread(buf, 1, (size_t)size, f);
    buf[got] = 0;
    out = js_str(ctx, buf, got);
    free(buf);
    fclose(f);
    free(path);
    return out;
}

static MBJSValue fs_write_text(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv, int append)
{
    char *path;
    char *data;
    size_t data_len = 0;
    FILE *f;
    int rc = 0;
    (void)this_val;
    if (argc < 2)
        return js_error(ctx, "writeText needs path and data");
    path = js_cstr(ctx, argv[0]);
    data = js_cstr_len(ctx, argv[1], &data_len);
    if (!path || !data) {
        free(path);
        free(data);
        return js_error(ctx, "writeText conversion failed");
    }
    f = fopen(path, append ? "ab" : "wb");
    if (!f) {
        free(path);
        free(data);
        return js_error(ctx, "writeText could not open file");
    }
    if (data_len && fwrite(data, 1, data_len, f) != data_len)
        rc = -1;
    if (fclose(f) != 0)
        rc = -1;
    free(path);
    free(data);
    return MBJS_NewInt32(ctx, rc);
}

static MBJSValue fs_write_text_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    return fs_write_text(ctx, this_val, argc, argv, 0);
}

static MBJSValue fs_append_text_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                   int argc, MBJSValueConst *argv)
{
    return fs_write_text(ctx, this_val, argc, argv, 1);
}

static MBJSValue fs_exists(MBJSContext *ctx, MBJSValueConst this_val,
                           int argc, MBJSValueConst *argv)
{
    char *path;
    struct _stat st;
    int found;
    (void)this_val;
    if (argc < 1)
        return MBJS_NewInt32(ctx, 0);
    path = js_cstr(ctx, argv[0]);
    if (!path)
        return MBJS_NewInt32(ctx, 0);
    found = _stat(path, &st) == 0;
    free(path);
    return MBJS_NewInt32(ctx, found ? 1 : 0);
}

static MBJSValue fs_stat(MBJSContext *ctx, MBJSValueConst this_val,
                         int argc, MBJSValueConst *argv)
{
    char *path;
    struct _stat st;
    char buf[256];
    int n;
    (void)this_val;
    if (argc < 1)
        return js_str(ctx, "", 0);
    path = js_cstr(ctx, argv[0]);
    if (!path)
        return js_str(ctx, "", 0);
    if (_stat(path, &st) != 0) {
        free(path);
        return js_str(ctx, "", 0);
    }
    n = snprintf(buf, sizeof(buf),
                 "{\"size\":%lld,\"mtimeMs\":%lld,\"isDir\":%s,\"isFile\":%s}",
                 (long long)st.st_size, (long long)st.st_mtime * 1000,
                 (st.st_mode & S_IFDIR) ? "true" : "false",
                 (st.st_mode & S_IFREG) ? "true" : "false");
    free(path);
    return js_str(ctx, buf, (size_t)(n > 0 ? n : 0));
}

static MBJSValue fs_readdir(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    char *path;
    char *pattern;
    size_t n;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    MBJSValue out;
    (void)this_val;
    if (argc < 1)
        return js_str(ctx, "[]", 2);
    path = js_cstr(ctx, argv[0]);
    if (!path)
        return js_str(ctx, "[]", 2);
    n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        path[--n] = 0;
    pattern = (char *)malloc(n + 3);
    if (!pattern) {
        free(path);
        return js_str(ctx, "[]", 2);
    }
    memcpy(pattern, path, n);
    memcpy(pattern + n, "/*", 3);
    free(path);
    h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return js_str(ctx, "[]", 2);
    append(&buf, &cap, &len, "[", 1);
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        if (len > 1)
            append(&buf, &cap, &len, ",", 1);
        append_json_string(&buf, &cap, &len, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    append(&buf, &cap, &len, "]", 1);
    FindClose(h);
    out = js_str(ctx, buf ? buf : "[]", len);
    free(buf);
    return out;
}

static MBJSValue fs_mkdir(MBJSContext *ctx, MBJSValueConst this_val,
                          int argc, MBJSValueConst *argv)
{
    char *path;
    struct _stat st;
    int rc;
    (void)this_val;
    if (argc < 1)
        return MBJS_NewInt32(ctx, -1);
    path = js_cstr(ctx, argv[0]);
    if (!path)
        return MBJS_NewInt32(ctx, -1);
    if (_stat(path, &st) == 0) {
        free(path);
        return (st.st_mode & S_IFDIR) ? MBJS_NewInt32(ctx, 0) : MBJS_NewInt32(ctx, -1);
    }
    rc = _mkdir(path);
    free(path);
    return MBJS_NewInt32(ctx, rc);
}

static MBJSValue fs_remove(MBJSContext *ctx, MBJSValueConst this_val,
                           int argc, MBJSValueConst *argv)
{
    char *path;
    int rc;
    (void)this_val;
    if (argc < 1)
        return MBJS_NewInt32(ctx, -1);
    path = js_cstr(ctx, argv[0]);
    if (!path)
        return MBJS_NewInt32(ctx, -1);
    rc = remove(path);
    if (rc != 0)
        rc = _rmdir(path);
    free(path);
    return MBJS_NewInt32(ctx, rc);
}

static MBJSValue fs_rename(MBJSContext *ctx, MBJSValueConst this_val,
                           int argc, MBJSValueConst *argv)
{
    char *from;
    char *to;
    int rc;
    (void)this_val;
    if (argc < 2)
        return MBJS_NewInt32(ctx, -1);
    from = js_cstr(ctx, argv[0]);
    to = js_cstr(ctx, argv[1]);
    if (!from || !to) {
        free(from);
        free(to);
        return MBJS_NewInt32(ctx, -1);
    }
    rc = rename(from, to);
    free(from);
    free(to);
    return MBJS_NewInt32(ctx, rc);
}

static MBJSValue fs_copy_file(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    char *from;
    char *to;
    FILE *in;
    FILE *out;
    char buf[8192];
    size_t n;
    int rc = 0;
    (void)this_val;
    if (argc < 2)
        return MBJS_NewInt32(ctx, -1);
    from = js_cstr(ctx, argv[0]);
    to = js_cstr(ctx, argv[1]);
    if (!from || !to) {
        free(from);
        free(to);
        return MBJS_NewInt32(ctx, -1);
    }
    in = fopen(from, "rb");
    if (!in) {
        free(from);
        free(to);
        return MBJS_NewInt32(ctx, -1);
    }
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        free(from);
        free(to);
        return MBJS_NewInt32(ctx, -1);
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in))
        rc = -1;
    if (fclose(out) != 0)
        rc = -1;
    fclose(in);
    free(from);
    free(to);
    return MBJS_NewInt32(ctx, rc);
}

static MBJSValue fs_cwd(MBJSContext *ctx, MBJSValueConst this_val,
                        int argc, MBJSValueConst *argv)
{
    char buf[MAX_PATH];
    (void)this_val; (void)argc; (void)argv;
    if (!_getcwd(buf, sizeof(buf)))
        return js_str(ctx, "", 0);
    return js_str(ctx, buf, strlen(buf));
}

static int fs_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("readText", 1, fs_read_text),
        MBJS_CFUNC_DEF("writeText", 2, fs_write_text_fn),
        MBJS_CFUNC_DEF("appendText", 2, fs_append_text_fn),
        MBJS_CFUNC_DEF("exists", 1, fs_exists),
        MBJS_CFUNC_DEF("stat", 1, fs_stat),
        MBJS_CFUNC_DEF("readdir", 1, fs_readdir),
        MBJS_CFUNC_DEF("mkdir", 1, fs_mkdir),
        MBJS_CFUNC_DEF("remove", 1, fs_remove),
        MBJS_CFUNC_DEF("rename", 2, fs_rename),
        MBJS_CFUNC_DEF("copyFile", 2, fs_copy_file),
        MBJS_CFUNC_DEF("cwd", 0, fs_cwd),
    };
    return rt.SetModuleExportList(ctx, m, tab, 11);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                    const char *name)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("readText", 1, fs_read_text),
        MBJS_CFUNC_DEF("writeText", 2, fs_write_text_fn),
        MBJS_CFUNC_DEF("appendText", 2, fs_append_text_fn),
        MBJS_CFUNC_DEF("exists", 1, fs_exists),
        MBJS_CFUNC_DEF("stat", 1, fs_stat),
        MBJS_CFUNC_DEF("readdir", 1, fs_readdir),
        MBJS_CFUNC_DEF("mkdir", 1, fs_mkdir),
        MBJS_CFUNC_DEF("remove", 1, fs_remove),
        MBJS_CFUNC_DEF("rename", 2, fs_rename),
        MBJS_CFUNC_DEF("copyFile", 2, fs_copy_file),
        MBJS_CFUNC_DEF("cwd", 0, fs_cwd),
    };
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;
    if (missing)
        return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochifs", fs_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, tab, 11) != 0)
        return NULL;
    return m;
}
