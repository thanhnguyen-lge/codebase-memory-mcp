/*
 * dsl_include.c — INCLUDE()/RUN() resolution for DSL scripts.
 *
 * A DSL include path is not relative to the including file: it starts with a
 * runtime layer name that the script host mounts onto some package directory
 * ("common/conf/rinse/rinsing.conf", "reference/rinse/water_level.tbl"). The
 * mapping is a deployment convention. The project's `path_aliases`
 * (.codebase-memory.json, see path_alias.h) win when configured. Otherwise a
 * file below a script root (`scripts/`) resolves against the runtime mount
 * layers derived from its own path (see "Runtime mount layers" below). A file
 * outside any script root falls back to dropping the leading layer segment and
 * looking the rest up in the nearest ancestor directory.
 *
 * Unresolvable includes (dynamic paths, layers outside the indexed tree) get
 * no edge rather than a guessed one.
 */
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/path_alias.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "foundation/hash_table.h"
#include "graph_buffer/graph_buffer.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DSL_PATH_BUF = 1024,
    /* Upper bound on modules reachable through nested includes. The deepest
     * real include chains are a handful of levels and tens of files; the cap
     * only guards against pathological fan-out. */
    DSL_CLOSURE_MAX = 512,
};

/* The extension-keeping Module node for a repo-relative DSL file, or NULL.
 * The file_path check matters: the keep-ext QN of a non-DSL file can equal a
 * stem-stripped Module QN of another file (foo/bar.py vs foo/bar/py.py are
 * both proj.foo.bar.py), which would misroute that file's imports here. */
static const cbm_gbuf_node_t *find_dsl_module(const cbm_gbuf_t *gbuf, const char *project,
                                              const char *rel) {
    char *qn = cbm_pipeline_fqn_module_keep_ext(project, rel);
    const cbm_gbuf_node_t *n = qn ? cbm_gbuf_find_by_qn(gbuf, qn) : NULL;
    free(qn);
    bool ok = n && n->label && strcmp(n->label, "Module") == 0 && n->file_path &&
              strcmp(n->file_path, rel) == 0;
    return ok ? n : NULL;
}

bool cbm_pipeline_is_dsl_source(const cbm_gbuf_t *gbuf, const char *project, const char *rel) {
    if (!rel || !find_dsl_module(gbuf, project, rel)) {
        return false;
    }
    /* An extension-less file (Makefile, Dockerfile, BUILD) has the same QN
     * under both schemes, so its ordinary Module would pass the check above.
     * DSL files always carry an extension, so only a differing QN is DSL. */
    char *keep = cbm_pipeline_fqn_module_keep_ext(project, rel);
    char *stem = cbm_pipeline_fqn_module(project, rel);
    bool differs = keep && stem && strcmp(keep, stem) != 0;
    free(keep);
    free(stem);
    return differs;
}

/* Look `tail` up under every ancestor directory of source_rel, nearest first. */
static const cbm_gbuf_node_t *find_in_ancestors(const cbm_pipeline_ctx_t *ctx,
                                                const char *source_rel, const char *tail) {
    char dir[DSL_PATH_BUF];
    snprintf(dir, sizeof(dir), "%s", source_rel);
    cbm_normalize_path_sep(dir);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
    } else {
        dir[0] = '\0';
    }
    for (;;) {
        char cand[DSL_PATH_BUF];
        int n = snprintf(cand, sizeof(cand), "%s%s%s", dir, dir[0] ? "/" : "", tail);
        if (n > 0 && (size_t)n < sizeof(cand)) {
            const cbm_gbuf_node_t *hit = find_dsl_module(ctx->gbuf, ctx->project_name, cand);
            if (hit) {
                return hit;
            }
        }
        if (!dir[0]) {
            return NULL;
        }
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
        } else {
            dir[0] = '\0';
        }
    }
}

/* ── Runtime mount layers ──────────────────────────────────────────
 *
 * The script host mounts five layers, all derived from the entry file's place
 * below the script root `{root}/{region}/{country}/{capacity}/{course}/...`:
 *
 *   active    {root}/{region}/{country}/{capacity}/{course}
 *   reference {root}/{region}/{country}/base_spec/{reference course}
 *   lib       {root}/lib
 *   common    {root}/{region}/{country}/base_spec/default
 *   variable  {root}/{region}/{country}/{capacity}/default
 *
 * The reference course is the file's SET_REFERENCE_COURSE('x') literal, else
 * its course segment. This mirrors anchorsOf()/makeResolver() of the
 * ScriptDevExtension dependency graph, so both tools link the same files.
 * Every anchor is kept repo-relative; one outside the indexed tree is unset and
 * a lookup against it misses. */

enum {
    DSL_LAYER_ACTIVE,
    DSL_LAYER_REFERENCE,
    DSL_LAYER_LIB,
    DSL_LAYER_COMMON,
    DSL_LAYER_VARIABLE,
    DSL_LAYER_COUNT,
};

static const char *const k_dsl_layer_words[DSL_LAYER_COUNT] = {"active", "reference", "lib",
                                                               "common", "variable"};

typedef struct {
    bool found; /* the file sits below a script root */
    /* Repo-relative anchors; has[i] false when unset. REFERENCE is the
     * base_spec folder itself; the course is appended when known. */
    char dir[DSL_LAYER_COUNT][DSL_PATH_BUF];
    bool has[DSL_LAYER_COUNT];
    char course[DSL_PATH_BUF]; /* empty when the reference course is unknown */
    char abs_reference[DSL_PATH_BUF]; /* absolute base_spec folder, for fan-out */
} dsl_anchors_t;

/* A script root folder: "scripts", or "<alnum>_scripts" / "<alnum>-scripts". */
static bool is_script_root_segment(const char *seg, size_t len) {
    static const char k_word[] = "scripts";
    const size_t wlen = sizeof(k_word) - SKIP_ONE;
    if (len < wlen || strncmp(seg + len - wlen, k_word, wlen) != 0) {
        return false;
    }
    if (len == wlen) {
        return true;
    }
    size_t plen = len - wlen - SKIP_ONE; /* alnum prefix before the separator */
    char sep = seg[plen];
    if (plen == 0 || (sep != '_' && sep != '-')) {
        return false;
    }
    for (size_t i = 0; i < plen; i++) {
        char ch = seg[i];
        bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (!alnum) {
            return false;
        }
    }
    return true;
}

/* Store `abs` as a repo-relative anchor when it lies inside the repo root. */
static void set_anchor(dsl_anchors_t *an, int layer, const char *root, const char *abs) {
    size_t rlen = strlen(root);
    if (strncmp(abs, root, rlen) != 0 || (abs[rlen] != '\0' && abs[rlen] != '/')) {
        return;
    }
    const char *rel = abs[rlen] == '/' ? abs + rlen + SKIP_ONE : abs + rlen;
    snprintf(an->dir[layer], sizeof(an->dir[layer]), "%s", rel);
    an->has[layer] = true;
}

static void dsl_anchors_of(const char *repo_path, const char *source_rel,
                           const char *course_literal, dsl_anchors_t *an) {
    memset(an, 0, sizeof(*an));
    char root[CBM_SZ_4K];
    if (!repo_path || !source_rel || !cbm_canonical_path(repo_path, root, sizeof(root))) {
        return;
    }
    cbm_normalize_path_sep(root);
    size_t rlen = strlen(root);
    while (rlen > SKIP_ONE && root[rlen - SKIP_ONE] == '/') {
        root[--rlen] = '\0';
    }
    char abs[CBM_SZ_4K];
    int n = snprintf(abs, sizeof(abs), "%s/%s", root, source_rel);
    if (n <= 0 || (size_t)n >= sizeof(abs)) {
        return;
    }
    cbm_normalize_path_sep(abs);

    /* The last directory segment that names a script root (never the file). */
    const char *file = strrchr(abs, '/');
    size_t root_end = 0;
    for (const char *seg = abs; seg && seg < file;) {
        const char *next = strchr(seg, '/');
        size_t len = next ? (size_t)(next - seg) : strlen(seg);
        if (len > 0 && is_script_root_segment(seg, len)) {
            root_end = (size_t)(seg - abs) + len;
        }
        seg = next ? next + SKIP_ONE : NULL;
    }
    if (root_end == 0) {
        return;
    }
    an->found = true;

    /* Segments below the script root, the file name included. */
    char below[CBM_SZ_4K];
    snprintf(below, sizeof(below), "%s", abs + root_end + SKIP_ONE);
    const char *seg[4] = {NULL, NULL, NULL, NULL}; /* region, country, capacity, course */
    int nseg = 0;
    /* Directories only: the file name is never a layer segment. */
    char *cur = below;
    char *slash;
    while (nseg < 4 && (slash = strchr(cur, '/')) != NULL) {
        *slash = '\0';
        seg[nseg++] = cur;
        cur = slash + SKIP_ONE;
    }
    const char *region = nseg >= 1 ? seg[0] : NULL;
    const char *country = nseg >= 2 ? seg[1] : NULL;
    const char *capacity = nseg >= 3 ? seg[2] : NULL;
    const char *course_seg = nseg >= 4 ? seg[3] : NULL;

    char sroot[CBM_SZ_4K];
    snprintf(sroot, sizeof(sroot), "%.*s", (int)root_end, abs);
    char buf[CBM_SZ_4K];
    snprintf(buf, sizeof(buf), "%s/lib", sroot);
    set_anchor(an, DSL_LAYER_LIB, root, buf);
    if (!region || !country) {
        return;
    }
    snprintf(buf, sizeof(buf), "%s/%s/%s/base_spec", sroot, region, country);
    set_anchor(an, DSL_LAYER_REFERENCE, root, buf);
    snprintf(an->abs_reference, sizeof(an->abs_reference), "%s", buf);
    snprintf(buf, sizeof(buf), "%s/%s/%s/base_spec/default", sroot, region, country);
    set_anchor(an, DSL_LAYER_COMMON, root, buf);
    if (capacity) {
        snprintf(buf, sizeof(buf), "%s/%s/%s/%s/default", sroot, region, country, capacity);
        set_anchor(an, DSL_LAYER_VARIABLE, root, buf);
        if (course_seg) {
            snprintf(buf, sizeof(buf), "%s/%s/%s/%s/%s", sroot, region, country, capacity,
                     course_seg);
            set_anchor(an, DSL_LAYER_ACTIVE, root, buf);
        }
    }
    /* A `default` folder is shared by every course, so it names none. */
    const char *course = course_literal && course_literal[0] ? course_literal : course_seg;
    if (course && strcmp(course, "default") != 0) {
        snprintf(an->course, sizeof(an->course), "%s", course);
    }
}

bool cbm_pipeline_dsl_folder_course_known(const char *repo_path, const char *rel) {
    dsl_anchors_t an;
    dsl_anchors_of(repo_path, rel, NULL, &an);
    return an.course[0] != '\0';
}

/* Join dir and tail into out, collapsing "." and ".." segments. False when the
 * result climbs above the repo root or does not fit. */
static bool join_normalized(const char *dir, const char *tail, char *out, size_t out_sz) {
    char tmp[DSL_PATH_BUF];
    int n = snprintf(tmp, sizeof(tmp), "%s%s%s", dir, dir[0] ? "/" : "", tail);
    if (n <= 0 || (size_t)n >= sizeof(tmp) || out_sz == 0) {
        return false;
    }
    size_t len = 0;
    out[0] = '\0';
    char *save = NULL;
    for (char *seg = strtok_r(tmp, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (strcmp(seg, ".") == 0) {
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            if (len == 0) {
                return false;
            }
            char *slash = strrchr(out, '/');
            len = slash ? (size_t)(slash - out) : 0;
            out[len] = '\0';
            continue;
        }
        size_t slen = strlen(seg);
        if (len + (len ? SKIP_ONE : 0) + slen >= out_sz) {
            return false;
        }
        if (len) {
            out[len++] = '/';
        }
        memcpy(out + len, seg, slen + SKIP_ONE);
        len += slen;
    }
    return len > 0;
}

static const cbm_gbuf_node_t *find_under(const cbm_pipeline_ctx_t *ctx, const char *dir,
                                         const char *tail) {
    char cand[DSL_PATH_BUF];
    if (!join_normalized(dir, tail, cand, sizeof(cand))) {
        return NULL;
    }
    return find_dsl_module(ctx->gbuf, ctx->project_name, cand);
}

static int cmp_cstr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* `reference/tail` from a file whose course is unknown (a shared `default`
 * folder): every course folder of base_spec that has the file, in name order.
 * Each of them is loaded by some course, so each is a real dependency. */
static int fan_out_reference(const cbm_pipeline_ctx_t *ctx, const dsl_anchors_t *an,
                             const char *tail, const cbm_gbuf_node_t **out, int max) {
    cbm_dir_t *d = cbm_opendir(an->abs_reference);
    if (!d) {
        return 0;
    }
    char **names = NULL;
    int count = 0;
    int cap = 0;
    cbm_dirent_t *e;
    while ((e = cbm_readdir(d)) != NULL) {
        if (e->name[0] == '.' || strcmp(e->name, "default") == 0) {
            continue;
        }
        if (count >= cap) {
            int ncap = cap ? cap * 2 : CBM_SZ_64;
            char **nn = realloc(names, (size_t)ncap * sizeof(char *));
            if (!nn) {
                break;
            }
            names = nn;
            cap = ncap;
        }
        names[count] = strdup(e->name);
        if (names[count]) {
            count++;
        }
    }
    cbm_closedir(d);
    if (count > 1) {
        qsort(names, (size_t)count, sizeof(char *), cmp_cstr);
    }
    int found = 0;
    for (int i = 0; i < count; i++) {
        char dir[DSL_PATH_BUF];
        int n = snprintf(dir, sizeof(dir), "%s%s%s", an->dir[DSL_LAYER_REFERENCE],
                         an->dir[DSL_LAYER_REFERENCE][0] ? "/" : "", names[i]);
        const cbm_gbuf_node_t *hit = NULL;
        if (n > 0 && (size_t)n < sizeof(dir)) {
            hit = find_under(ctx, dir, tail);
        }
        if (hit && found < max) {
            out[found++] = hit;
        }
        free(names[i]);
    }
    free((void *)names);
    return found;
}

/* The layer an include path starts with (`common/...`), or -1. *tail is set to
 * the rest of the path. */
static int layer_prefix_of(const char *path, const char **tail) {
    for (int i = 0; i < DSL_LAYER_COUNT; i++) {
        size_t wlen = strlen(k_dsl_layer_words[i]);
        if (strncmp(path, k_dsl_layer_words[i], wlen) == 0 && path[wlen] == '/') {
            *tail = path + wlen + SKIP_ONE;
            return i;
        }
    }
    return -1;
}

/* Resolve against the mount layers of a file below a script root. */
static int resolve_by_anchors(const cbm_pipeline_ctx_t *ctx, const char *source_rel,
                              const dsl_anchors_t *an, const char *path,
                              const cbm_gbuf_node_t **out, int max, bool *fan_out) {
    char refdir[DSL_PATH_BUF] = {0};
    bool has_ref = false;
    if (an->has[DSL_LAYER_REFERENCE] && an->course[0]) {
        int n = snprintf(refdir, sizeof(refdir), "%s%s%s", an->dir[DSL_LAYER_REFERENCE],
                         an->dir[DSL_LAYER_REFERENCE][0] ? "/" : "", an->course);
        has_ref = n > 0 && (size_t)n < sizeof(refdir);
    }

    /* 1. The target as written, below each mounted folder. */
    const int order[] = {DSL_LAYER_ACTIVE, DSL_LAYER_LIB, DSL_LAYER_COMMON, DSL_LAYER_VARIABLE};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const cbm_gbuf_node_t *hit =
            an->has[order[i]] ? find_under(ctx, an->dir[order[i]], path) : NULL;
        if (hit) {
            out[0] = hit;
            return SKIP_ONE;
        }
    }
    if (has_ref) {
        const cbm_gbuf_node_t *hit = find_under(ctx, refdir, path);
        if (hit) {
            out[0] = hit;
            return SKIP_ONE;
        }
    }

    /* 2. A leading layer word is replaced by its folder. Nothing else is
     * tried, so a layer path never lands on a same-named file elsewhere. */
    const char *tail = NULL;
    int layer = layer_prefix_of(path, &tail);
    if (layer == DSL_LAYER_REFERENCE) {
        if (has_ref) {
            const cbm_gbuf_node_t *hit = find_under(ctx, refdir, tail);
            if (hit) {
                out[0] = hit;
                return SKIP_ONE;
            }
            return 0;
        }
        if (!an->has[DSL_LAYER_REFERENCE]) {
            return 0;
        }
        *fan_out = true;
        return fan_out_reference(ctx, an, tail, out, max);
    }
    if (layer >= 0) {
        const cbm_gbuf_node_t *hit = an->has[layer] ? find_under(ctx, an->dir[layer], tail) : NULL;
        if (hit) {
            out[0] = hit;
            return SKIP_ONE;
        }
        return 0;
    }

    /* 3. No layer word: a path relative to some ancestor folder, e.g.
     * `drumcare/wash/rewater.conf` from inside base_spec/drumcare. */
    const cbm_gbuf_node_t *hit = find_in_ancestors(ctx, source_rel, path);
    if (hit) {
        out[0] = hit;
        return SKIP_ONE;
    }
    return 0;
}

/* Resolve one include of source_rel, whose mount layers are `an`. Writes up
 * to max Modules to out and returns how many (0: unresolved). *fan_out is set
 * when they are every course's copy of a `reference/` include. */
static int resolve_includes(const cbm_pipeline_ctx_t *ctx, const char *source_rel,
                            const dsl_anchors_t *an, const char *include_path,
                            const cbm_gbuf_node_t **out, int max, bool *fan_out) {
    *fan_out = false;
    if (!ctx || !source_rel || !include_path || !include_path[0] || !out || max <= 0) {
        return 0;
    }
    const char *path = include_path;
    while (path[0] == '.' && path[SKIP_ONE] == '/') {
        path += 2;
    }

    /* 1. Configured layer mapping (repo-relative target). */
    if (ctx->path_aliases) {
        const cbm_path_alias_map_t *amap = cbm_path_alias_find_for_file(ctx->path_aliases,
                                                                        source_rel);
        char *aliased = amap ? cbm_path_alias_resolve(amap, path) : NULL;
        if (aliased) {
            const cbm_gbuf_node_t *hit =
                find_dsl_module(ctx->gbuf, ctx->project_name, aliased);
            free(aliased);
            if (hit) {
                out[0] = hit;
                return SKIP_ONE;
            }
        }
    }

    /* 2. Runtime mount layers, when the file sits below a script root. */
    if (an->found) {
        return resolve_by_anchors(ctx, source_rel, an, path, out, max, fan_out);
    }

    /* 3. No script root: drop the layer segment, nearest ancestor wins. */
    const char *rest = strchr(path, '/');
    if (rest && rest[SKIP_ONE]) {
        const cbm_gbuf_node_t *hit = find_in_ancestors(ctx, source_rel, rest + SKIP_ONE);
        if (hit) {
            out[0] = hit;
            return SKIP_ONE;
        }
    }

    /* 4. A plain path relative to some ancestor (no layer prefix). */
    const cbm_gbuf_node_t *hit = find_in_ancestors(ctx, source_rel, path);
    if (hit) {
        out[0] = hit;
        return SKIP_ONE;
    }
    return 0;
}

const cbm_gbuf_node_t *cbm_pipeline_resolve_dsl_include(const cbm_pipeline_ctx_t *ctx,
                                                        const char *source_rel,
                                                        const char *include_path) {
    if (!ctx || !source_rel) {
        return NULL;
    }
    dsl_anchors_t an;
    dsl_anchors_of(ctx->repo_path, source_rel, NULL, &an);
    const cbm_gbuf_node_t *hit = NULL;
    bool fan_out = false;
    return resolve_includes(ctx, source_rel, &an, include_path, &hit, SKIP_ONE, &fan_out) > 0
               ? hit
               : NULL;
}

/* The course folder of a fan-out target: the path segment right below the
 * base_spec folder `ref_base`. Empty when the target is not below it. */
static void fan_out_course_of(const char *target_rel, const char *ref_base, char *out,
                              size_t out_sz) {
    out[0] = '\0';
    size_t blen = strlen(ref_base);
    const char *rest = target_rel;
    if (blen > 0) {
        if (strncmp(target_rel, ref_base, blen) != 0 || target_rel[blen] != '/') {
            return;
        }
        rest = target_rel + blen + SKIP_ONE;
    }
    const char *slash = strchr(rest, '/');
    if (slash) {
        snprintf(out, out_sz, "%.*s", (int)(slash - rest), rest);
    }
}

int cbm_pipeline_dsl_insert_include_edges(cbm_pipeline_ctx_t *ctx, const char *source_rel,
                                          const CBMFileResult *result, const CBMImport *imp,
                                          const cbm_gbuf_node_t *source_node) {
    if (!ctx || !source_rel || !imp || !imp->module_path || !source_node) {
        return 0;
    }
    /* The anchors carry the file's reference course even when an alias,
     * not a mount layer, resolves this include. */
    dsl_anchors_t an;
    dsl_anchors_of(ctx->repo_path, source_rel, result ? result->dsl_reference_course : NULL, &an);
    const cbm_gbuf_node_t *targets[CBM_DSL_INCLUDE_MAX_TARGETS];
    bool fan_out = false;
    int n = resolve_includes(ctx, source_rel, &an, imp->module_path, targets,
                             CBM_DSL_INCLUDE_MAX_TARGETS, &fan_out);

    char esc_ln[CBM_SZ_128];
    cbm_json_escape(esc_ln, sizeof(esc_ln), imp->local_name ? imp->local_name : "");
    char esc_course[CBM_SZ_128];
    cbm_json_escape(esc_course, sizeof(esc_course), an.course);
    int count = 0;
    for (int t = 0; t < n; t++) {
        if (targets[t]->id == source_node->id) {
            continue;
        }
        char props[CBM_SZ_512];
        int len = snprintf(props, sizeof(props), "{\"local_name\":\"%s\"", esc_ln);
        if (esc_course[0] && len > 0 && (size_t)len < sizeof(props)) {
            len += snprintf(props + len, sizeof(props) - (size_t)len,
                            ",\"reference_course\":\"%s\"", esc_course);
        }
        if (fan_out && len > 0 && (size_t)len < sizeof(props)) {
            char course[CBM_SZ_128];
            char esc_fan[CBM_SZ_128];
            fan_out_course_of(targets[t]->file_path ? targets[t]->file_path : "",
                              an.dir[DSL_LAYER_REFERENCE], course, sizeof(course));
            cbm_json_escape(esc_fan, sizeof(esc_fan), course);
            len += snprintf(props + len, sizeof(props) - (size_t)len, ",\"fan_out\":\"%s\"",
                            esc_fan);
        }
        if (len <= 0 || (size_t)len + SKIP_ONE >= sizeof(props)) {
            continue; /* never emit a truncated property object */
        }
        props[len] = '}';
        props[len + SKIP_ONE] = '\0';
        cbm_gbuf_insert_edge(ctx->gbuf, source_node->id, targets[t]->id, "IMPORTS", props);
        count++;
    }
    return count;
}

/* The string value of "key" in an edge's flat properties object, still
 * JSON-escaped (both sides of every comparison below are escaped the same
 * way). False when absent or too long for out. */
static bool edge_prop(const char *props, const char *key, char *out, size_t out_sz) {
    char pat[CBM_SZ_64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = props ? strstr(props, pat) : NULL;
    if (!p) {
        return false;
    }
    p += strlen(pat);
    size_t len = 0;
    while (p[len] && p[len] != '"') {
        len += (p[len] == '\\' && p[len + SKIP_ONE]) ? PAIR_LEN : SKIP_ONE;
    }
    if (len >= out_sz) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* The reference course a DSL file runs with: the reference_course property
 * its IMPORTS edges carry. Empty when unknown (a shared `default` file). */
static void file_reference_course(const cbm_gbuf_t *gbuf, const cbm_gbuf_node_t *file_node,
                                  char *out, size_t out_sz) {
    out[0] = '\0';
    const cbm_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    if (cbm_gbuf_find_edges_by_source_type(gbuf, file_node->id, "IMPORTS", &edges,
                                           &edge_count) != 0) {
        return;
    }
    for (int i = 0; i < edge_count; i++) {
        if (edge_prop(edges[i]->properties_json, "reference_course", out, out_sz)) {
            return;
        }
    }
}

/* Append `s` to a growable borrowed-pointer list unless already present. */
static bool closure_add(const char ***vals, int *count, int *cap, const char *s) {
    for (int i = 0; i < *count; i++) {
        if ((*vals)[i] == s) {
            return false;
        }
    }
    if (*count >= *cap) {
        int ncap = *cap ? *cap * 2 : CBM_SZ_16;
        const char **nv = realloc((void *)*vals, (size_t)ncap * sizeof(const char *));
        if (!nv) {
            return false;
        }
        *vals = nv;
        *cap = ncap;
    }
    (*vals)[(*count)++] = s;
    return true;
}

int cbm_pipeline_dsl_import_closure(const cbm_gbuf_t *gbuf, const char *project,
                                    const char *rel_path, const char ***out_keys,
                                    const char ***out_vals, int *out_count) {
    *out_keys = NULL;
    *out_vals = NULL;
    *out_count = 0;

    /* BFS over IMPORTS edges, File -> Module, hopping from each included
     * Module to its File. INCLUDE is textual, so everything a nested include
     * defines is in scope of the outermost includer. Values are the reachable
     * Module QNs (borrowed from gbuf), which is what the registry's
     * import-reachability filter matches candidate QNs against. */
    const char **vals = NULL;
    int count = 0;
    int cap = 0;
    const char **queue = NULL; /* file_path of each File still to expand */
    int qhead = 0;
    int qcount = 0;
    int qcap = 0;
    closure_add(&queue, &qcount, &qcap, rel_path);

    /* The runtime mounts `reference/` once per session, from the entry file.
     * A shared file reached on the way links every course's copy of its
     * `reference/` includes (fan_out edges); only the entry's course is
     * loaded, so the other copies are not in scope. */
    char course[CBM_SZ_128] = {0};
    char *entry_qn = cbm_pipeline_fqn_file(project, rel_path);
    const cbm_gbuf_node_t *entry = entry_qn ? cbm_gbuf_find_by_qn(gbuf, entry_qn) : NULL;
    free(entry_qn);
    if (entry) {
        file_reference_course(gbuf, entry, course, sizeof(course));
    }

    while (qhead < qcount && count < DSL_CLOSURE_MAX) {
        const char *cur = queue[qhead++];
        char *file_qn = cbm_pipeline_fqn_file(project, cur);
        const cbm_gbuf_node_t *file_node = file_qn ? cbm_gbuf_find_by_qn(gbuf, file_qn) : NULL;
        free(file_qn);
        if (!file_node) {
            continue;
        }
        const cbm_gbuf_edge_t **edges = NULL;
        int edge_count = 0;
        if (cbm_gbuf_find_edges_by_source_type(gbuf, file_node->id, "IMPORTS", &edges,
                                               &edge_count) != 0) {
            continue;
        }
        for (int i = 0; i < edge_count && count < DSL_CLOSURE_MAX; i++) {
            char fan[CBM_SZ_128];
            if (course[0] && edge_prop(edges[i]->properties_json, "fan_out", fan, sizeof(fan)) &&
                strcmp(fan, course) != 0) {
                continue; /* another course's copy */
            }
            const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, edges[i]->target_id);
            if (!target || !target->qualified_name || !target->file_path) {
                continue;
            }
            if (closure_add(&vals, &count, &cap, target->qualified_name)) {
                closure_add(&queue, &qcount, &qcap, target->file_path);
            }
        }
    }
    free((void *)queue);

    if (count == 0) {
        free((void *)vals);
        return 0;
    }
    /* Keys are never matched for DSL (calls are bare names, resolved through
     * reachability, not alias.member), but callers free them per entry. */
    const char **keys = calloc((size_t)count, sizeof(const char *));
    if (!keys) {
        free((void *)vals);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        keys[i] = strdup(vals[i]);
    }
    *out_keys = keys;
    *out_vals = vals;
    *out_count = count;
    return 0;
}

/* ── Fan-out calls ─────────────────────────────────────────────── */

typedef struct {
    int64_t source_id;
    int64_t target_id;
    char *props;
} dsl_pending_call_t;

/* props with its "strategy":"include_scope" replaced by the fan-out
 * strategy. Heap string, or NULL. */
static char *fan_out_call_props(const char *props) {
    static const char k_from[] = "\"strategy\":\"include_scope\"";
    static const char k_to[] = "\"strategy\":\"include_scope_fan_out\"";
    const char *at = strstr(props, k_from);
    if (!at) {
        return NULL;
    }
    size_t head = (size_t)(at - props);
    const char *tail = at + sizeof(k_from) - SKIP_ONE;
    size_t len = head + sizeof(k_to) - SKIP_ONE + strlen(tail);
    char *out = malloc(len + SKIP_ONE);
    if (out) {
        snprintf(out, len + SKIP_ONE, "%.*s%s%s", (int)head, props, k_to, tail);
    }
    return out;
}

static bool closure_has(const char **vals, int count, const char *qn) {
    for (int i = 0; i < count; i++) {
        if (vals[i] && strcmp(vals[i], qn) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_calls_edge(const cbm_gbuf_t *gbuf, int64_t source_id, int64_t target_id) {
    const cbm_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    if (cbm_gbuf_find_edges_by_source_type(gbuf, source_id, "CALLS", &edges, &edge_count) != 0) {
        return false;
    }
    for (int i = 0; i < edge_count; i++) {
        if (edges[i]->target_id == target_id) {
            return true;
        }
    }
    return false;
}

/* What one calling file can see, computed once per file for the whole pass:
 * a file with a known reference course gets no fan-out (the closure already
 * drops the other courses), so its closure is never built. */
typedef struct {
    bool course_known;
    const char **keys; /* closure, as cbm_pipeline_dsl_import_closure */
    const char **vals;
    int count;
} dsl_call_scope_t;

static const dsl_call_scope_t *call_scope_of(CBMHashTable *cache, const cbm_gbuf_t *gbuf,
                                             const char *project, const char *file_path) {
    dsl_call_scope_t *sc = cbm_ht_get(cache, file_path);
    if (sc) {
        return sc;
    }
    sc = calloc(1, sizeof(*sc));
    if (!sc) {
        return NULL;
    }
    char course[CBM_SZ_128] = {0};
    char *file_qn = cbm_pipeline_fqn_file(project, file_path);
    const cbm_gbuf_node_t *file_node = file_qn ? cbm_gbuf_find_by_qn(gbuf, file_qn) : NULL;
    free(file_qn);
    if (file_node) {
        file_reference_course(gbuf, file_node, course, sizeof(course));
    }
    sc->course_known = course[0] != '\0';
    if (!sc->course_known) {
        cbm_pipeline_dsl_import_closure(gbuf, project, file_path, &sc->keys, &sc->vals,
                                        &sc->count);
    }
    cbm_ht_set(cache, file_path, sc); /* key borrowed from the gbuf node */
    return sc;
}

static void free_call_scope(const char *key, void *value, void *userdata) {
    (void)key;
    (void)userdata;
    dsl_call_scope_t *sc = value;
    for (int i = 0; i < sc->count; i++) {
        free((void *)sc->keys[i]);
    }
    free((void *)sc->keys);
    free((void *)sc->vals);
    free(sc);
}

/* Link `call` to the same function in the other courses of every fan_out
 * include that reached the callee's Module. */
static int fan_out_one_call(cbm_gbuf_t *gbuf, const char *project, CBMHashTable *scopes,
                            const dsl_pending_call_t *call) {
    const cbm_gbuf_node_t *callee = cbm_gbuf_find_by_id(gbuf, call->target_id);
    const cbm_gbuf_node_t *caller = cbm_gbuf_find_by_id(gbuf, call->source_id);
    if (!callee || !callee->file_path || !callee->qualified_name || !caller ||
        !caller->file_path) {
        return 0;
    }
    const cbm_gbuf_node_t *module = find_dsl_module(gbuf, project, callee->file_path);
    if (!module || !module->qualified_name) {
        return 0;
    }
    size_t mlen = strlen(module->qualified_name);
    if (strncmp(callee->qualified_name, module->qualified_name, mlen) != 0 ||
        callee->qualified_name[mlen] != '.') {
        return 0;
    }
    const char *suffix = callee->qualified_name + mlen; /* ".name" */

    const cbm_gbuf_edge_t **in = NULL;
    int in_count = 0;
    if (cbm_gbuf_find_edges_by_target_type(gbuf, module->id, "IMPORTS", &in, &in_count) != 0 ||
        in_count == 0) {
        return 0;
    }
    bool any_fan = false;
    for (int i = 0; i < in_count && !any_fan; i++) {
        char fan[CBM_SZ_128];
        any_fan = edge_prop(in[i]->properties_json, "fan_out", fan, sizeof(fan));
    }
    if (!any_fan) {
        return 0;
    }
    /* Only siblings the caller can see. */
    const dsl_call_scope_t *scope = call_scope_of(scopes, gbuf, project, caller->file_path);
    if (!scope || scope->course_known) {
        return 0;
    }
    char *props = fan_out_call_props(call->props);
    if (!props) {
        return 0;
    }

    int added = 0;
    for (int i = 0; i < in_count; i++) {
        char fan[CBM_SZ_128];
        char local[CBM_SZ_256];
        if (!edge_prop(in[i]->properties_json, "fan_out", fan, sizeof(fan)) ||
            !edge_prop(in[i]->properties_json, "local_name", local, sizeof(local))) {
            continue;
        }
        const cbm_gbuf_edge_t **sib = NULL;
        int sib_count = 0;
        if (cbm_gbuf_find_edges_by_source_type(gbuf, in[i]->source_id, "IMPORTS", &sib,
                                               &sib_count) != 0) {
            continue;
        }
        for (int j = 0; j < sib_count; j++) {
            char sib_local[CBM_SZ_256];
            if (sib[j]->target_id == module->id ||
                !edge_prop(sib[j]->properties_json, "fan_out", fan, sizeof(fan)) ||
                !edge_prop(sib[j]->properties_json, "local_name", sib_local,
                           sizeof(sib_local)) ||
                strcmp(sib_local, local) != 0) {
                continue;
            }
            const cbm_gbuf_node_t *other = cbm_gbuf_find_by_id(gbuf, sib[j]->target_id);
            if (!other || !other->qualified_name ||
                !closure_has(scope->vals, scope->count, other->qualified_name)) {
                continue;
            }
            char qn[CBM_SZ_1K];
            int n = snprintf(qn, sizeof(qn), "%s%s", other->qualified_name, suffix);
            const cbm_gbuf_node_t *twin =
                n > 0 && (size_t)n < sizeof(qn) ? cbm_gbuf_find_by_qn(gbuf, qn) : NULL;
            if (!twin || twin->id == caller->id || twin->id == callee->id ||
                has_calls_edge(gbuf, caller->id, twin->id)) {
                continue; /* an existing edge keeps its own strategy */
            }
            cbm_gbuf_insert_edge(gbuf, caller->id, twin->id, "CALLS", props);
            added++;
        }
    }
    free(props);
    return added;
}

int cbm_pipeline_dsl_fan_out_calls(cbm_gbuf_t *gbuf, const char *project) {
    if (!gbuf || !project) {
        return 0;
    }
    /* Snapshot first: inserting CALLS edges grows the array being read. Only
     * DSL resolves through the include scope (registry.c), so the strategy
     * alone selects DSL calls. */
    const cbm_gbuf_edge_t **calls = NULL;
    int call_count = 0;
    if (cbm_gbuf_find_edges_by_type(gbuf, "CALLS", &calls, &call_count) != 0 ||
        call_count == 0) {
        return 0;
    }
    dsl_pending_call_t *pending = NULL;
    int npending = 0;
    int cap = 0;
    for (int i = 0; i < call_count; i++) {
        const char *p = calls[i]->properties_json;
        if (!p || !strstr(p, "\"strategy\":\"include_scope\"")) {
            continue;
        }
        if (npending >= cap) {
            int ncap = cap ? cap * 2 : CBM_SZ_64;
            dsl_pending_call_t *np = realloc(pending, (size_t)ncap * sizeof(*np));
            if (!np) {
                break;
            }
            pending = np;
            cap = ncap;
        }
        char *copy = strdup(p);
        if (!copy) {
            break;
        }
        pending[npending++] =
            (dsl_pending_call_t){calls[i]->source_id, calls[i]->target_id, copy};
    }
    CBMHashTable *scopes = cbm_ht_create(CBM_SZ_64);
    int added = 0;
    for (int i = 0; i < npending; i++) {
        if (scopes) {
            added += fan_out_one_call(gbuf, project, scopes, &pending[i]);
        }
        free(pending[i].props);
    }
    free(pending);
    if (scopes) {
        cbm_ht_foreach(scopes, free_call_scope, NULL);
        cbm_ht_free(scopes);
    }
    return added;
}
