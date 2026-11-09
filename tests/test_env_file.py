"""HOS_ENV_FILE names the switches file a run reads instead of hookshot_env.txt beside the game.

A scripted or A/B test run brings its own switches this way, and the player's hookshot_env.txt is neither read nor
changed. Unset or empty, the game reads hookshot_env.txt as before. A named file that does not exist sets nothing: the
run does not fall back to the player's switches.

python -I -B tests/test_env_file.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    fn = block((ROOT / "host" / "hos_main.c").read_text(encoding="utf-8"), "static void load_hookshot_env(void) {")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        game = tmp / "game"
        game.mkdir()
        (game / "hookshot_env.txt").write_text("FROM_BESIDE=1\nSHARED=beside\n", encoding="utf-8")
        (tmp / "test_env.txt").write_text("# a test run's own switches\nFROM_OTHER=1\nSHARED=other\n", encoding="utf-8")
        exe = str(game / "hookshot").replace("\\", "\\\\")
        fixture = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32   /* (the test's own setenv/unsetenv: Windows has _putenv, which the game itself uses there) */
static int setenv(const char *name, const char *value, int overwrite) {
    char s[1024];
    (void)overwrite;
    snprintf(s, sizeof s, "%s=%s", name, value);
    return _putenv(s);
}
static int unsetenv(const char *name) { return setenv(name, "", 1); }
#endif
#define MAX_PATH 260
typedef unsigned long DWORD;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static DWORD hos_game_path_a(char *out, DWORD size) {   /* (host/hos_paths.h: the game folder's path) */
    snprintf(out, size, "%s", "''' + exe + r'''");
    return (DWORD)strlen(out);
}
''' + fn + r'''
static int is(const char *name, const char *want) {
    const char *v = getenv(name);
    return want ? v && !strcmp(v, want) : !v;
}
static void clear(void) { unsetenv("FROM_BESIDE"); unsetenv("FROM_OTHER"); unsetenv("SHARED"); }
int main(int argc, char **argv) {
    (void)argc;
    unsetenv("HOS_ENV_FILE");                      /* out of the box: the file beside the game */
    load_hookshot_env();
    CHECK(is("FROM_BESIDE", "1") && is("SHARED", "beside") && is("FROM_OTHER", NULL));

    clear();
    setenv("HOS_ENV_FILE", "", 1);                 /* empty: as if unset */
    load_hookshot_env();
    CHECK(is("FROM_BESIDE", "1") && is("FROM_OTHER", NULL));

    clear();
    setenv("HOS_ENV_FILE", argv[1], 1);            /* a test run's own file, and only that one */
    load_hookshot_env();
    CHECK(is("FROM_OTHER", "1") && is("SHARED", "other") && is("FROM_BESIDE", NULL));

    clear();
    setenv("HOS_ENV_FILE", argv[2], 1);            /* a file that is not there: nothing, not the player's switches */
    load_hookshot_env();
    CHECK(is("FROM_BESIDE", NULL) && is("FROM_OTHER", NULL) && is("SHARED", NULL));
    printf("PASS: HOS_ENV_FILE replaces hookshot_env.txt; unset or empty reads it as before\n");
    return 0;
}
'''
        src = tmp / "fixture.c"
        src.write_text(fixture, encoding="utf-8")
        out = tmp / "fixture"
        subprocess.run([a.cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Werror", str(src), "-o", str(out)],
                       check=True)
        before = (game / "hookshot_env.txt").read_text(encoding="utf-8")
        subprocess.run([str(out), str(tmp / "test_env.txt"), str(tmp / "missing.txt")], check=True)
        if (game / "hookshot_env.txt").read_text(encoding="utf-8") != before:
            raise SystemExit("the player's hookshot_env.txt was changed")


if __name__ == "__main__":
    main()
