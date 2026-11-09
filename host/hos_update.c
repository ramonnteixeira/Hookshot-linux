/* (hookshot) The update offer: at start, the latest release on GitHub is asked for (a few seconds at most, nothing when offline), and
   when it is not the release this game folder was built with, the player is asked whether to update. Yes starts the Hookshot Builder
   that made the folder (hookshot_version.txt, written by nx2/make_game.py) with --update: it downloads the new release and builds the
   folder again from the game files the folder keeps, and the game closes. A folder with no hookshot_version.txt (built from the
   repository, or before updates came in) is never asked about. HOOKSHOT_NO_UPDATE_CHECK=1 (hookshot_env.txt) turns it off */
#ifdef _WIN32
#include "hos_paths.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_HOST L"api.github.com"
#define UPDATE_PATH L"/repos/NotRexed/Hookshot/releases/latest"
#define RELEASES_PAGE L"https://github.com/NotRexed/Hookshot/releases/latest"

/* (a line "name=value" of a small text file, in UTF-8: its value, trimmed) */
static int read_value(const wchar_t *path, const char *name, char *out, size_t cap) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    char line[1024];
    const size_t n = strlen(name);
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        if (strncmp(s, name, n) || s[n] != '=') continue;
        s += n + 1;
        size_t len = strlen(s);
        while (len && (s[len - 1] == 10 || s[len - 1] == 13 || s[len - 1] == ' ' || s[len - 1] == 9)) s[--len] = 0;
        if (len && len < cap) {
            memcpy(out, s, len + 1);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* (the latest release's tag, from GitHub's API: "tag_name":"...") */
static int latest_tag(char *tag, size_t cap) {
    int ok = 0;
    HINTERNET s = WinHttpOpen(L"Hookshot-update-check", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 2500, 2500, 2500, 3000);
    HINTERNET c = WinHttpConnect(s, UPDATE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", UPDATE_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            const size_t max = 512 << 10;
            char *body = (char *)malloc(max + 1);
            size_t have = 0;
            DWORD got = 0;
            while (body && have < max && WinHttpReadData(r, body + have, (DWORD)(max - have), &got) && got) have += got;
            if (body) {
                body[have] = 0;
                const char *k = strstr(body, "\"tag_name\"");
                if (k && (k = strchr(k + 10, '"'))) {
                    const char *e = strchr(++k, '"');
                    if (e && e > k && (size_t)(e - k) < cap) {
                        memcpy(tag, k, (size_t)(e - k));
                        tag[e - k] = 0;
                        ok = 1;
                    }
                }
                free(body);
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

void hos_update_check(void) {
    if (getenv("HOOKSHOT_NO_UPDATE_CHECK") && strcmp(getenv("HOOKSHOT_NO_UPDATE_CHECK"), "0")) return;
    wchar_t dir[MAX_PATH], file[MAX_PATH + 64];
    DWORD n = hos_game_path_w(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    if (!n) return;
    dir[n - 1] = 0;
    char have[128], builder[MAX_PATH * 3], skipped[128] = "", tag[128];
    _snwprintf(file, MAX_PATH + 64, L"%ls\\hookshot_version.txt", dir);
    if (!read_value(file, "version", have, sizeof have)) return;
    _snwprintf(file, MAX_PATH + 64, L"%ls\\user\\hookshot_update_skip.txt", dir);
    read_value(file, "skip", skipped, sizeof skipped);
    if (!latest_tag(tag, sizeof tag)) {
        fprintf(stderr, "update: the latest release could not be asked for (offline?)\n");
        return;
    }
    fprintf(stderr, "update: this folder is %s, the latest release is %s\n", have, tag);
    if (!strcmp(tag, have) || !strcmp(tag, skipped)) return;

    wchar_t wtag[128], whave[128], wbuilder[MAX_PATH], text[2048];
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, wtag, 128);
    MultiByteToWideChar(CP_UTF8, 0, have, -1, whave, 128);
    _snwprintf(file, MAX_PATH + 64, L"%ls\\hookshot_version.txt", dir);
    const int has_builder = read_value(file, "builder", builder, sizeof builder) &&
                            MultiByteToWideChar(CP_UTF8, 0, builder, -1, wbuilder, MAX_PATH) > 0 &&
                            GetFileAttributesW(wbuilder) != INVALID_FILE_ATTRIBUTES;
    if (has_builder)
        _snwprintf(text, 2048,
                   L"A new version of Hookshot is out: %ls (this game is %ls).\n\n"
                   L"Update now? The game closes, and Hookshot Builder downloads the new version and builds this game folder again from the "
                   L"game files it keeps. That takes about as long as your first build. Your saves and settings stay.\n\n"
                   L"Yes: update now\nNo: not now (asked again next time)\nCancel: skip this version",
                   wtag, whave);
    else
        _snwprintf(text, 2048,
                   L"A new version of Hookshot is out: %ls (this game is %ls).\n\n"
                   L"The Hookshot Builder that made this folder was not found, so it can't update by itself. Open the download page?\n\n"
                   L"Yes: open the page\nNo: not now (asked again next time)\nCancel: skip this version",
                   wtag, whave);
    const int answer = MessageBoxW(NULL, text, L"Hookshot update", MB_YESNOCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    if (answer == IDCANCEL) {
        wchar_t user[MAX_PATH + 16];
        _snwprintf(user, MAX_PATH + 16, L"%ls\\user", dir);
        CreateDirectoryW(user, NULL);
        _snwprintf(file, MAX_PATH + 64, L"%ls\\user\\hookshot_update_skip.txt", dir);
        FILE *f = _wfopen(file, L"wb");
        if (f) fprintf(f, "skip=%s\n", tag), fclose(f);
        return;
    }
    if (answer != IDYES) return;
    if (!has_builder) {
        ShellExecuteW(NULL, L"open", RELEASES_PAGE, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    wchar_t args[MAX_PATH + 256], wdir[MAX_PATH];
    _snwprintf(args, MAX_PATH + 256, L"--update \"%ls\" --tag \"%ls\"", dir, wtag);
    wcsncpy(wdir, wbuilder, MAX_PATH);
    wdir[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(wdir, L'\\');
    if (slash) *slash = 0;
    if ((INT_PTR)ShellExecuteW(NULL, L"open", wbuilder, args, slash ? wdir : NULL, SW_SHOWNORMAL) > 32) {
        fprintf(stderr, "update: Hookshot Builder started to update to %s; the game closes\n", tag);
        fflush(stderr);
        ExitProcess(0);
    }
    MessageBoxW(NULL, L"Hookshot Builder could not be started. The game starts as it is.", L"Hookshot update", MB_OK | MB_ICONWARNING | MB_TOPMOST);
}
#elif defined(__linux__)
#include "hos_paths.h"
#include <ctype.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* (the same offer as Windows, when the player started bin/hookshot itself. The launcher sets HOOKSHOT_NO_UPDATE_CHECK
   and asks in its own window. curl and zenity do the network and the question; without either, the game just starts) */

static int read_value(const char *path, const char *name, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[1024];
    const size_t n = strlen(name);
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line;
        if (strncmp(s, name, n) || s[n] != '=') continue;
        s += n + 1;
        size_t len = strlen(s);
        while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) s[--len] = 0;
        if (len && len < cap) memcpy(out, s, len + 1), found = 1;
    }
    fclose(f);
    return found;
}

static int safe_tag(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 80) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr("._+-", s[i])) return 0;
    return 1;
}

static int pipe_tag(const char *cmd, char *tag, size_t cap) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, p);
    pclose(p);
    buf[n] = 0;
    const char *k = strstr(buf, "\"tag_name\"");
    if (k && (k = strchr(k + 10, '"'))) {
        const char *e = strchr(++k, '"');
        if (e && e > k && (size_t)(e - k) < cap) {
            memcpy(tag, k, (size_t)(e - k));
            tag[e - k] = 0;
            return safe_tag(tag);
        }
    }
    if (!strncmp(buf, "tag ", 4)) {
        const char *s = buf + 4;
        size_t len = strcspn(s, "\r\n");
        if (len && len < cap) {
            memcpy(tag, s, len);
            tag[len] = 0;
            return safe_tag(tag);
        }
    }
    return 0;
}

static int latest_tag(char *tag, size_t cap) {
    if (pipe_tag("curl -fsSL --max-time 4 --retry 2 -A Hookshot-update-check "
                 "-H \"Accept: application/vnd.github+json\" "
                 "https://api.github.com/repos/NotRexed/Hookshot/releases/latest 2>/dev/null", tag, cap))
        return 1;
    return pipe_tag("curl -fsSL --max-time 8 --retry 2 -A Hookshot-update-check "
                    "https://github.com/NotRexed/Hookshot/releases/latest/download/hookshot-linux.txt 2>/dev/null", tag, cap);
}

/* the builder an update installed (builder/linux/linux_update.py): <data>/hookshot/builder/versions/<current>/AppRun. The
   builder= line names the AppImage that built the folder, which the player may have deleted since */
static int installed_builder(char *out, size_t cap) {
    const char *home = getenv("HOOKSHOT_BUILDER_HOME"), *data = getenv("XDG_DATA_HOME"), *user = getenv("HOME");
    char root[MAX_PATH * 2], path[MAX_PATH * 2 + 64], tag[128];
    if (home && *home) snprintf(root, sizeof root, "%s/hookshot/builder", home);
    else if (data && *data) snprintf(root, sizeof root, "%s/hookshot/builder", data);
    else if (user && *user) snprintf(root, sizeof root, "%s/.local/share/hookshot/builder", user);
    else return 0;
    snprintf(path, sizeof path, "%s/current", root);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const int got = fgets(tag, sizeof tag, f) != NULL;
    fclose(f);
    if (!got) return 0;
    tag[strcspn(tag, "\r\n ")] = 0;
    if (!safe_tag(tag)) return 0;
    if (snprintf(out, cap, "%s/versions/%s/AppRun", root, tag) >= (int)cap) return 0;
    return access(out, X_OK) == 0;
}

void hos_update_check(void) {
    const char *off = getenv("HOOKSHOT_NO_UPDATE_CHECK");
    if (off && strcmp(off, "0")) return;
    char path[MAX_PATH + 64], have[128], builder[MAX_PATH * 2], skipped[128] = "", tag[128];
    DWORD n = hos_game_path_a(path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    while (n && path[n - 1] != '/') n--;
    if (!n) return;
    path[n] = 0;
    char dir[MAX_PATH];
    memcpy(dir, path, n);
    dir[n] = 0;
    snprintf(path, sizeof path, "%shookshot_version.txt", dir);
    if (!read_value(path, "version", have, sizeof have) || !safe_tag(have)) return;
    snprintf(path, sizeof path, "%suser/hookshot_update_skip.txt", dir);
    read_value(path, "skip", skipped, sizeof skipped);
    if (!latest_tag(tag, sizeof tag)) {
        fprintf(stderr, "update: the latest release could not be asked for (offline?)\n");
        return;
    }
    fprintf(stderr, "update: this folder is %s, the latest release is %s\n", have, tag);
    if (!strcmp(tag, have) || !strcmp(tag, skipped)) return;
    snprintf(path, sizeof path, "%shookshot_version.txt", dir);
    const int has_builder = installed_builder(builder, sizeof builder) ||
                            (read_value(path, "builder", builder, sizeof builder) && access(builder, X_OK) == 0);
    char text[1600], cmd[4096];
    snprintf(text, sizeof text,
             has_builder
                 ? "A new version of Hookshot is out: %s (this game is %s). Update now? The game closes, and Hookshot Builder downloads only what changed and builds this folder again. Your saves and settings stay."
                 : "A new version of Hookshot is out: %s (this game is %s). The builder that made this folder was not found. Open the download page?",
             tag, have);
    snprintf(cmd, sizeof cmd, "zenity --list --title=Hookshot --hide-header --column= --text='%s' 'Update now' 'Not now' 'Skip this version'", text);
    FILE *pick = popen(cmd, "r");
    if (!pick) return;
    char answer[64] = "";
    if (!fgets(answer, sizeof answer, pick)) answer[0] = 0;
    pclose(pick);
    if (!strncmp(answer, "Skip", 4)) {
        snprintf(path, sizeof path, "%suser", dir);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%suser/hookshot_update_skip.txt", dir);
        FILE *f = fopen(path, "wb");
        if (f) fprintf(f, "skip=%s\n", tag), fclose(f);
        return;
    }
    if (strncmp(answer, "Update", 6)) return;
    if (!has_builder) {
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", "https://github.com/NotRexed/Hookshot/releases/latest", (char *)0);
            _exit(127);
        }
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execl(builder, builder, "--update", dir, "--tag", tag, (char *)0);
        _exit(127);
    }
    if (pid > 0) {
        fprintf(stderr, "update: Hookshot Builder started to update to %s; the game closes\n", tag);
        fflush(stderr);
        _exit(0);
    }
}
#else
void hos_update_check(void) {}
#endif
