// --probe-shader: the shader service client (gfx_shader.c) against 32 census shaders. Needs build/xlate/totk_xlate.exe and
// build/v121/shaders/ (tools/shader_census.py); uses a throwaway cache directory.
//   1. Translate 32 shaders (mixed stages) through gfx_shader_get; each SPIR-V starts with the magic 0x07230203.
//   2. Drop the memory cache and ask again: 32 hits from disk, 0 service requests. A third pass returns the same pointers.
//   3. Send a malformed line straight to the service: it answers with an error line and still serves a valid request.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/platform.h"
#include "gfx.h"

int gfx_shader_raw_request(const char *line, char *reply, size_t reply_size);
uint64_t gfx_shader_requests(void);
uint64_t gfx_shader_cache_hits(void);
void gfx_shader_drop_memory(void);

#define WANT 32
#define CHECK(cond, ...) do { if (!(cond)) { printf("probe-shader: FAILED: " __VA_ARGS__); printf("\n"); return 1; } } while (0)

typedef struct { char key[65]; int stage; const char *c1; size_t c1_len; uint32_t local[3]; } Entry;

static char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = (size_t)n;
    return b;
}

// Value of `"name": "` inside [from, to), NULL when absent; *len is the value length.
static const char *str_field(const char *from, const char *to, const char *name, size_t *len) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\": \"", name);
    const char *p = strstr(from, pat);
    if (!p || p >= to) return NULL;
    p += strlen(pat);
    const char *e = memchr(p, '"', (size_t)(to - p));
    if (!e) return NULL;
    *len = (size_t)(e - p);
    return p;
}

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

// The repository root: the nearest parent of totk.exe holding build/v121/shaders/jobs.json.
static int find_root(char *root, size_t n) {
    char exe[PLAT_MAX_PATH * 2];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    for (char *cut = strrchr(exe, PLAT_SEP[0]); cut; cut = strrchr(exe, PLAT_SEP[0])) {
        *cut = 0;
        char p[PLAT_MAX_PATH * 2 + 40];
        snprintf(p, sizeof p, "%s" PLAT_SEP "build" PLAT_SEP "v121" PLAT_SEP "shaders" PLAT_SEP "jobs.json", exe);
        if (plat_file_exists(p)) return snprintf(root, n, "%s", exe) < (int)n;
    }
    return 0;
}

static int pick_entries(const char *jobs, Entry *out) {
    enum { MAXN = 20000 };
    Entry *all = malloc(sizeof(Entry) * MAXN);
    int n = 0, per_stage[3] = {0, 0, 0};
    for (const char *p = jobs; n < MAXN && (p = strstr(p, "\"job\": {")); p += 8) {
        const char *end = strstr(p, "\"source\": ");
        if (!end) break;
        size_t kl, sl, cl;
        const char *k = str_field(p, end, "key", &kl), *s = str_field(p, end, "stage", &sl), *c = str_field(p, end, "c1", &cl);
        if (!k || !s || !c || kl != 64) continue;
        Entry *e = &all[n];
        memcpy(e->key, k, 64), e->key[64] = 0;
        e->stage = !strncmp(s, "vs", sl) ? GFX_STAGE_VS : !strncmp(s, "ps", sl) ? GFX_STAGE_FS : GFX_STAGE_CS;
        e->c1 = c, e->c1_len = cl / 2;
        e->local[0] = e->local[1] = e->local[2] = 1;
        const char *l = strstr(p, "\"local\": [");
        if (l && l < end) sscanf(l + 10, "%u , %u , %u", &e->local[0], &e->local[1], &e->local[2]);
        if (l && l < end && e->local[1] == 1 && e->local[2] == 1) sscanf(l + 10, "%u,%u,%u", &e->local[0], &e->local[1], &e->local[2]);
        per_stage[e->stage == GFX_STAGE_VS ? 0 : e->stage == GFX_STAGE_FS ? 1 : 2]++;
        n++;
    }
    // 11 vertex, 11 fragment, 10 compute shaders, spread evenly over each stage's list
    int want[3] = {11, 11, 10}, taken[3] = {0, 0, 0}, seen[3] = {0, 0, 0}, got = 0;
    for (int i = 0; i < n; i++) {
        int s = all[i].stage == GFX_STAGE_VS ? 0 : all[i].stage == GFX_STAGE_FS ? 1 : 2;
        if (per_stage[s] < want[s]) want[s] = per_stage[s];
        if (taken[s] < want[s] && seen[s] == (int)((long long)taken[s] * per_stage[s] / want[s])) out[got++] = all[i], taken[s]++;
        seen[s]++;
    }
    free(all);
    return got;
}

static char root[PLAT_MAX_PATH * 2];

// Builds the program the game would hand NVN for a census entry and translates it.
static const GfxShader *translate(const Entry *e, int *err) {
    char path[PLAT_MAX_PATH * 2 + 64];
    uint8_t c1[8192], control[0x880] = {0};
    if (e->c1_len > sizeof c1) return *err = 1, NULL;
    for (size_t b = 0; b < e->c1_len; b++) c1[b] = (uint8_t)(hexval(e->c1[b * 2]) << 4 | hexval(e->c1[b * 2 + 1]));
    snprintf(path, sizeof path, "%s" PLAT_SEP "build" PLAT_SEP "v121" PLAT_SEP "shaders" PLAT_SEP "code" PLAT_SEP "%s.code", root, e->key);
    size_t body_size;
    char *body = slurp(path, &body_size);
    if (!body) return *err = 1, NULL;
    size_t skip = e->stage == GFX_STAGE_CS ? 0 : 0x30, total = skip + body_size;
    uint8_t *code = calloc(1, total);
    memcpy(code + skip, body, body_size);
    free(body);
    uint32_t c1_off = (uint32_t)(total - e->c1_len), length = (uint32_t)total;
    memcpy(control + 0x700, &c1_off, 4), memcpy(control + 0x704, &length, 4);
    GfxShaderAnswers a = {0};
    if (e->stage == GFX_STAGE_CS) for (int k = 0; k < 3; k++) a.local[k] = (uint16_t)e->local[k];
    const GfxShader *s = gfx_shader_get(e->stage, control, code, total, c1, &a);
    free(code);
    return s;
}

static void remove_dir(const char *dir) {
    PlatDir *d = plat_dir_open(dir);
    PlatDirEntry e;
    while (d && plat_dir_next(d, &e)) {
        char f[PLAT_MAX_PATH * 2];
        snprintf(f, sizeof f, "%s" PLAT_SEP "%s", dir, e.name);
        if (!e.is_dir) plat_file_delete(f);
    }
    plat_dir_close(d);
    plat_dir_remove(dir);
}

#define FAIL_IF(cond, ...) do { if (cond) { printf("probe-shader: FAILED: " __VA_ARGS__); printf("\n"); remove_dir(dir); return 1; } } while (0)

int probe_shader(void) {
    char path[PLAT_MAX_PATH * 2 + 64];
    CHECK(find_root(root, sizeof root), "build/v121/shaders/jobs.json not found (run tools/shader_census.py)");
    snprintf(path, sizeof path, "%s" PLAT_SEP "build" PLAT_SEP "v121" PLAT_SEP "shaders" PLAT_SEP "jobs.json", root);
    size_t jobs_size;
    char *jobs = slurp(path, &jobs_size);
    CHECK(jobs, "cannot read %s", path);
    static Entry entries[WANT];
    int count = pick_entries(jobs, entries);
    CHECK(count == WANT, "only %d census shaders usable", count);

    char dir[PLAT_MAX_PATH], temp[PLAT_MAX_PATH / 2];
    snprintf(dir, sizeof dir, "%s" PLAT_SEP "totk-shader-probe-%u", plat_temp_dir(temp, sizeof temp) ? temp : ".", plat_process_id());
    CHECK(plat_setenv("TOTK_SHADER_CACHE", dir), "cannot set TOTK_SHADER_CACHE");

    // pass 1: translate through the service
    const GfxShader *first[WANT];
    uint64_t req0 = gfx_shader_requests();
    for (int i = 0; i < WANT; i++) {
        int err = 0;
        first[i] = translate(&entries[i], &err);
        FAIL_IF(!first[i], "shader %s did not translate", entries[i].key);
        uint64_t words;
        const uint32_t *w = gfx_shader_spirv(first[i], &words);
        FAIL_IF(words < 5 || w[0] != 0x07230203, "shader %s: bad SPIR-V magic", entries[i].key);
    }
    FAIL_IF(gfx_shader_requests() - req0 != WANT, "pass 1 sent %llu requests, expected %d",
            (unsigned long long)(gfx_shader_requests() - req0), WANT);

    // pass 2: memory cache dropped, so every answer comes from the disk cache
    gfx_shader_drop_memory();
    req0 = gfx_shader_requests();
    uint64_t hits0 = gfx_shader_cache_hits();
    const GfxShader *second[WANT];
    for (int i = 0; i < WANT; i++) {
        int err = 0;
        second[i] = translate(&entries[i], &err);
        FAIL_IF(!second[i], "shader %s missing from the disk cache", entries[i].key);
        uint64_t words;
        const uint32_t *w = gfx_shader_spirv(second[i], &words);
        FAIL_IF(words < 5 || w[0] != 0x07230203, "shader %s: bad SPIR-V magic from disk", entries[i].key);
    }
    int hits = (int)(gfx_shader_cache_hits() - hits0);
    FAIL_IF(gfx_shader_requests() != req0, "pass 2 contacted the service");
    FAIL_IF(hits != WANT, "pass 2 had %d cache hits", hits);

    // pass 3: memory hits return the same pointer
    for (int i = 0; i < WANT; i++) {
        int err = 0;
        FAIL_IF(translate(&entries[i], &err) != second[i], "repeat call for %s returned another pointer", entries[i].key);
    }
    FAIL_IF(gfx_shader_requests() != req0, "pass 3 contacted the service");
    FAIL_IF(gfx_shader_failures() != 0, "%llu translation failures", (unsigned long long)gfx_shader_failures());

    // the service survives a malformed line and still translates a valid one
    char reply[4096];
    FAIL_IF(!gfx_shader_raw_request("this is not json", reply, sizeof reply), "service gone after the malformed line");
    FAIL_IF(strncmp(reply, "fail ", 5), "malformed line answered with \"%s\"", reply);
    FAIL_IF(!gfx_shader_raw_request("{\"job\":{\"stage\":\"ps\"},\"out\":\"x\"}", reply, sizeof reply) || strncmp(reply, "fail ", 5),
            "job without key not rejected");
    char dir_json[PLAT_MAX_PATH * 2], line[PLAT_MAX_PATH * 4 + 1024], *o = dir_json;
    for (const char *s = dir; *s; s++) { if (*s == '\\') *o++ = '\\'; *o++ = *s; }
    *o = 0;
    char esc_root[PLAT_MAX_PATH * 2];
    o = esc_root;
    for (const char *s = root; *s; s++) { if (*s == '\\') *o++ = '\\'; *o++ = *s; }
    *o = 0;
    const Entry *e = &entries[0];
    snprintf(line, sizeof line,
             "{\"job\":{\"key\":\"%.64sa\",\"stage\":\"%s\",\"debug\":false,\"c1\":\"%.*s\",\"code\":\"%s\\\\build\\\\v121\\\\shaders\\\\code\\\\%.64s.code\"},\"out\":\"%s\"}",
             e->key, e->stage == GFX_STAGE_VS ? "vs" : e->stage == GFX_STAGE_FS ? "ps" : "cs", (int)(e->c1_len * 2), e->c1,
             esc_root, e->key, dir_json);
    FAIL_IF(!gfx_shader_raw_request(line, reply, sizeof reply), "service did not answer a valid request after the malformed one");
    FAIL_IF(strncmp(reply, "ok ", 3), "valid request after the malformed one answered \"%s\"", reply);

    remove_dir(dir);
    printf("probe-shader: passed (%d shaders, %d cache hits, service survived malformed request)\n", count, hits);
    return 0;
}
