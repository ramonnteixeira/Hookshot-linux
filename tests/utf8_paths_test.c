/* Windows: the game's narrow paths in a folder named outside every ANSI code page (tests/test_utf8_paths.py builds this beside
   a copy of the game's manifest, nx2/win_manifest.py, and runs it from such a folder). host/hos_main.c's narrow_paths_ok is
   pasted in for the start-up check. Exit 0: all good; the expected code page is argv[1] ("utf8", or "ansi" without the manifest). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ACTUAL_NARROW_PATHS_OK */

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)   /* (stdout: stderr becomes the log below) */

/* (UTF-8 <-> UTF-16, as the game's char paths meet the W functions) */
static void round_trip(void) {
    static const char u8[] = "C:\\\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\\Z\xc3\xa9lda \xf0\x9f\x8e\xae\\";   /* C:\ゲーム\Zélda 🎮\ */
    static const wchar_t u16[] = {L'C', L':', L'\\', 0x30B2, 0x30FC, 0x30E0, L'\\', L'Z', 0x00E9, L'l', L'd', L'a', L' ', 0xD83C, 0xDFAE, L'\\', 0};
    wchar_t w[64];
    char back[64];
    const int nw = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8, -1, w, 64);
    CHECK(nw == (int)(sizeof u16 / sizeof u16[0]));
    CHECK(!wcscmp(w, u16));
    const int nb = WideCharToMultiByte(CP_UTF8, 0, w, -1, back, sizeof back, NULL, NULL);
    CHECK(nb == (int)sizeof u8);
    CHECK(!strcmp(back, u8));
}

int main(int argc, char **argv) {
    const int utf8 = argc > 1 && !strcmp(argv[1], "utf8");
    round_trip();
    if (!utf8) {   /* (the negative control: without the manifest no ANSI code page spells the folder's emoji) */
        if (GetACP() == CP_UTF8) {
            printf("utf8 paths: this Windows uses UTF-8 for every program already, no negative control\n");
            return failures ? 1 : 0;
        }
        CHECK(!narrow_paths_ok(argc, argv));
        if (!failures) printf("utf8 paths: without the manifest the folder does not open (code page %u), as before\n", GetACP());
        return failures ? 1 : 0;
    }
    CHECK(GetACP() == CP_UTF8);
    CHECK(narrow_paths_ok(argc, argv));
    /* (the exe's path through the A function is the W one in UTF-8) */
    wchar_t wexe[MAX_PATH];
    char exe[MAX_PATH], exe8[MAX_PATH * 3];
    CHECK(GetModuleFileNameW(NULL, wexe, MAX_PATH) > 0);
    CHECK(GetModuleFileNameA(NULL, exe, MAX_PATH) > 0);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, wexe, -1, exe8, sizeof exe8, NULL, NULL) > 0);
    CHECK(!strcmp(exe, exe8));
    /* (a file beside it, by its narrow path: fopen and CreateFileA write it, the W functions find the same name) */
    char *slash = strrchr(exe, '\\');
    CHECK(slash != NULL);
    if (!slash) return 1;
    strcpy(slash + 1, "hookshot_log \xf0\x9f\x8e\xae.txt");
    FILE *f = fopen(exe, "w");
    CHECK(f != NULL);
    if (f) fputs("ok\n", f), fclose(f);
    wchar_t wlog[MAX_PATH];
    wcscpy(wlog, wexe);
    wcscpy(wcsrchr(wlog, L'\\') + 1, L"hookshot_log \xD83C\xDFAE.txt");
    CHECK(GetFileAttributesW(wlog) != INVALID_FILE_ATTRIBUTES);
    HANDLE h = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(DeleteFileW(wlog));
    /* (argv: the tester's folder "Hookshot-DLSS ゼルダ" came in as "Hookshot-DLSS ???", and the game printed its usage line) */
    CHECK(argc > 2 && !strcmp(argv[2], "Hookshot-DLSS \xe3\x82\xbc\xe3\x83\xab\xe3\x83\x80 Z\xc3\xa9lda"));
    /* (the log: freopen'd stderr on the UTF-8 path, and the path printed into it as hos_log's "fs: mounted" lines do, in UTF-8) */
    strcpy(slash + 1, "hookshot_log.txt");
    if (freopen(exe, "w", stderr)) {
        fprintf(stderr, "fs: mounted rom: -> %s\n", exe);
        fflush(stderr);
        wcscpy(wcsrchr(wlog, L'\\') + 1, L"hookshot_log.txt");
        FILE *in = _wfopen(wlog, L"rb");
        char got[MAX_PATH * 3 + 64] = "", want[MAX_PATH * 3 + 64];
        CHECK(in != NULL);
        if (in) got[fread(got, 1, sizeof got - 1, in)] = 0, fclose(in);
        {   /* (stderr is a text stream: its line ends reached the file as CR LF on Windows) */
            char *o = got;
            for (const char *i = got; *i; i++)
                if (*i != '\r') *o++ = *i;
            *o = 0;
        }
        WideCharToMultiByte(CP_UTF8, 0, wlog, -1, exe8, sizeof exe8, NULL, NULL);
        snprintf(want, sizeof want, "fs: mounted rom: -> %s\n", exe8);
        CHECK(!strcmp(got, want));
    } else CHECK(!"freopen of the log");
    /* (the environment through getenv, as hookshot_env.txt's switches and LOCALAPPDATA-style paths come in) */
    _wputenv(L"HOOKSHOT_UTF8_PROBE=\x30B2\x30FC\x30E0 \xD83C\xDFAE");
    const char *e = getenv("HOOKSHOT_UTF8_PROBE");
    CHECK(e && !strcmp(e, "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0 \xf0\x9f\x8e\xae"));
    if (!failures) printf("utf8 paths: ok (code page %u)\n", GetACP());
    return failures ? 1 : 0;
}
