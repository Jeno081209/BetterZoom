/* fastscan.c - mask-aware byte-pattern scanner for huge binaries.
 *
 * Why: the pure-Python scanners (verify_signatures.py / verify_matrix.py) walk
 * a 300MB libminecraftpe.so byte by byte in interpreter code and take minutes
 * per build. This does the same work in C: memchr() locates the pattern's
 * longest fixed byte-run (glibc memchr is SIMD), then only candidate positions
 * get the mask check.
 *
 * Usage:  fastscan <binary> <patternfile>
 *   patternfile lines:  <Name>\t<AA BB ?? C? ...>      ('#'/'!' lines ignored)
 * Output (tab separated, one line per pattern):
 *   <STATUS>\t<Name>\t<count>\t<0xoff> <0xoff> ...
 *   STATUS: UNIQUE | MISSING | AMBIG
 *   count is capped at --max (default 16) occurrences.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MAXPAT 4096
#define MAXHITS 64

typedef struct {
    char name[128];
    unsigned char val[MAXPAT];
    unsigned char msk[MAXPAT];
    size_t n;
    size_t anchor;      /* start of longest fully-fixed run */
    size_t anchor_len;
    unsigned char anchor_first;
} Pattern;

static int parse_hex(const char *tok, unsigned char *v, unsigned char *m)
{
    size_t len = strlen(tok);
    if (len == 0 || len > 2) return -1;
    *v = 0; *m = 0;
    for (size_t i = 0; i < len; i++) {
        char c = tok[i];
        unsigned char nib = 0, nibmask = 0;
        if (c == '?') { nib = 0; nibmask = 0; }
        else if (c >= '0' && c <= '9') { nib = (unsigned char)(c - '0'); nibmask = 0xF; }
        else if (c >= 'a' && c <= 'f') { nib = (unsigned char)(c - 'a' + 10); nibmask = 0xF; }
        else if (c >= 'A' && c <= 'F') { nib = (unsigned char)(c - 'A' + 10); nibmask = 0xF; }
        else return -1;
        *v  |= (unsigned char)(nib << ((1 - i) * 4));
        *m  |= (unsigned char)(nibmask << ((1 - i) * 4));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <binary> <patternfile> [maxhits]\n", argv[0]);
        return 2;
    }
    size_t maxhits = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : MAXHITS;
    if (maxhits == 0 || maxhits > MAXHITS) maxhits = MAXHITS;

    /* ---- load patterns ---- */
    FILE *pf = fopen(argv[2], "r");
    if (!pf) { perror("patternfile"); return 2; }
    static Pattern pats[512];
    size_t npats = 0;
    char line[8192];
    while (fgets(line, sizeof line, pf)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '!') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *hex = tab + 1;
        if (npats >= sizeof pats / sizeof pats[0]) break;
        Pattern *p = &pats[npats];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", line);
        /* tokenize on whitespace */
        char *save = NULL;
        for (char *tok = strtok_r(hex, " \t\r\n", &save); tok;
             tok = strtok_r(NULL, " \t\r\n", &save)) {
            if (p->n >= MAXPAT) { fprintf(stderr, "pattern too long: %s\n", p->name); break; }
            if (parse_hex(tok, &p->val[p->n], &p->msk[p->n]) != 0) {
                fprintf(stderr, "bad token '%s' in pattern %s\n", tok, p->name);
                return 2;
            }
            p->n++;
        }
        if (p->n == 0) continue;
        /* longest fully-fixed run */
        size_t best = 0, beststart = 0;
        for (size_t i = 0; i < p->n; ) {
            if (p->msk[i] == 0xFF) {
                size_t j = i;
                while (j < p->n && p->msk[j] == 0xFF) j++;
                if (j - i > best) { best = j - i; beststart = i; }
                i = j;
            } else i++;
        }
        p->anchor = beststart;
        p->anchor_len = best;
        p->anchor_first = best ? p->val[beststart] : 0;
        npats++;
    }
    fclose(pf);
    if (npats == 0) { fprintf(stderr, "no patterns\n"); return 2; }

    /* ---- map the binary ---- */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); return 2; }
    size_t size = (size_t)st.st_size;
    const unsigned char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); return 2; }
    madvise((void *)data, size, MADV_SEQUENTIAL);

    for (size_t k = 0; k < npats; k++) {
        Pattern *p = &pats[k];
        size_t hits = 0;
        char offs[1024];
        offs[0] = 0;
        if (p->anchor_len >= 1) {
            const unsigned char *cur = data;
            const unsigned char *end = data + size;
            while (cur < end) {
                const unsigned char *found = memchr(cur, p->anchor_first, (size_t)(end - cur));
                if (!found) break;
                size_t pos = (size_t)(found - data);
                if (pos < p->anchor) { cur = found + 1; continue; }
                size_t base = pos - p->anchor;
                if (base + p->n <= size) {
                    size_t i = 0;
                    for (; i < p->n; i++) {
                        if (p->msk[i] && ((data[base + i] & p->msk[i]) != p->val[i])) break;
                    }
                    if (i == p->n) {
                        if (hits < maxhits) {
                            char buf[32];
                            snprintf(buf, sizeof buf, "0x%zx ", base);
                            if (strlen(offs) + strlen(buf) < sizeof offs - 1) strcat(offs, buf);
                        }
                        hits++;
                    }
                }
                cur = found + 1;
            }
        }
        const char *status = hits == 0 ? "MISSING" : (hits == 1 ? "UNIQUE" : "AMBIG");
        printf("%s\t%s\t%zu\t%s\n", status, p->name, hits, offs);
    }
    munmap((void *)data, size);
    close(fd);
    return 0;
}
