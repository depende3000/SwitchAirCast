/* Unit test for player.c's HLS helpers (included directly to reach static functions). */
#include "../source/player.c"

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc((size_t) n + 1);
    if (fread(d, 1, (size_t) n, f) != (size_t) n) n = 0;
    d[n] = '\0';
    fclose(f);
    return d;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <playlist> <target-seconds> | --master <master> <max-height>\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--master")) {
        char *m = slurp(argv[2]);
        char *out = m ? pick_variant(m, atoi(argv[3])) : NULL;
        printf("%s", out ? out : "(no variants)\n");
        return 0;
    }
    char *pl = slurp(argv[1]);
    char *out = pl ? trim_media_playlist(pl, atof(argv[2])) : NULL;
    printf("%s", out ? out : "(no segment)\n");
    return 0;
}
