/*
 * dsl_include.c — INCLUDE()/RUN() resolution for DSL scripts.
 *
 * A DSL include path is not relative to the including file: it starts with a
 * runtime layer name that the script host mounts onto some package directory
 * ("common/conf/rinse/rinsing.conf", "reference/rinse/water_level.tbl"). The
 * mapping is a deployment convention, so it is taken from the project's
 * `path_aliases` (.codebase-memory.json, see path_alias.h) when configured;
 * otherwise the leading layer segment is dropped and the rest is looked up in
 * the nearest ancestor directory of the including file, which is where a
 * course-relative layer ("reference/", "active/") points.
 *
 * Unresolvable includes (dynamic paths, layers outside the indexed tree) get
 * no edge rather than a guessed one.
 */
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/path_alias.h"
#include "foundation/constants.h"
#include "foundation/platform.h"

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

const cbm_gbuf_node_t *cbm_pipeline_resolve_dsl_include(const cbm_pipeline_ctx_t *ctx,
                                                        const char *source_rel,
                                                        const char *include_path) {
    if (!ctx || !source_rel || !include_path || !include_path[0]) {
        return NULL;
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
                return hit;
            }
        }
    }

    /* 2. Course-relative layer: drop the layer segment, nearest ancestor wins. */
    const char *rest = strchr(path, '/');
    if (rest && rest[SKIP_ONE]) {
        const cbm_gbuf_node_t *hit = find_in_ancestors(ctx, source_rel, rest + SKIP_ONE);
        if (hit) {
            return hit;
        }
    }

    /* 3. A plain path relative to some ancestor (no layer prefix). */
    return find_in_ancestors(ctx, source_rel, path);
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
