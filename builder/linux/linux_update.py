"""Linux builder updates, the same way Windows installs (builder/setup/setup.c, scripts/make_setup_parts.py).

A release publishes hookshot-linux.txt. Each part is a zip named by its SHA-256 and kept on the linux-parts release, so a
part whose files did not change is not downloaded again. The player's copy lives at
$XDG_DATA_HOME/hookshot/builder/versions/<tag> (AppRun at its root, the same layout as the AppImage). An unchanged part
is copied from the builder that is running now (the mounted AppImage, or the version installed last time).

    tag <tag>
    part <sha256> <bytes> <dir under the install> <url>
    tree                         the whole directory
    file <name>                  one file in that directory (repeat; instead of tree)
"""
import hashlib
import os
import re
import shutil
import stat
import time
import urllib.error
import urllib.request
import zipfile

LIST_NAME = "hookshot-linux.txt"
LATEST_LIST = "https://github.com/NotRexed/Hookshot/releases/latest/download/" + LIST_NAME
LATEST_API = "https://api.github.com/repos/NotRexed/Hookshot/releases/latest"


class Part(object):
    def __init__(self, sha, size, dest, url, files):
        self.sha, self.size, self.dest, self.url, self.files = sha, size, dest, url, files  # files None: the whole directory

    def same(self, other):
        return other is not None and other.sha == self.sha and other.dest == self.dest and other.files == self.files


class Manifest(object):
    def __init__(self, tag, parts):
        self.tag, self.parts = tag, parts

    def by_sha(self, sha):
        for part in self.parts:
            if part.sha == sha:
                return part
        return None


def data_root():
    base = os.environ.get("HOOKSHOT_BUILDER_HOME") or os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "hookshot", "builder")


def parse_list(text):
    """The setup list. A part owns the following tree line, or the file lines, until the next part."""
    tag, parts, cur = "", [], None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("tag "):
            tag = line[4:].strip()
        elif line.startswith("part "):
            bits = line.split(" ")
            if len(bits) != 5:
                raise ValueError("a part line needs sha, size, directory and url")
            cur = Part(bits[1], int(bits[2]), bits[3], bits[4], [])
            parts.append(cur)
        elif line == "tree":
            if cur is None:
                raise ValueError("tree with no part")
            cur.files = None
        elif line.startswith("file "):
            if cur is None or cur.files is None:
                raise ValueError("file with no part")
            cur.files.append(line[5:].strip())
        else:
            raise ValueError("unknown list line")
    if not tag or not parts:
        raise ValueError("the setup list has no release")
    return Manifest(tag, parts)


def release_key(tag):
    """A version to compare: a release tag (v2.0.5, v2.1.0-rc.1) or git describe's v2.0.5-3-g1a2b3c4[-dirty] (a build from
    the Actions tab or a checkout: after v2.0.5, before any later release). None for a build that names no release: a bare
    commit (digits alone too: no v and no dot), dev, or no version at all."""
    m = re.match(r"^(?=v|\d+\.)v?(\d+(?:\.\d+)*)(?:-(?!\d+-g|dirty$)([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?(-\d+-g[0-9a-f]+)?(-dirty)?$",
                 (tag or "").strip())
    if not m:
        return None
    # (a prerelease before its release; commits after a tag, or changes to it, after that tag and before the next)
    return tuple(int(n) for n in m.group(1).split(".")), m.group(2) is None, bool(m.group(3) or m.group(4))


def update_target(latest, tools):
    """The newest version this builder knows of: the latest release when it is newer than these tools, else the tools'
    own. (issue 902) Any version other than the latest release was made to update before it could build, so a test
    build from the Actions tab, newer than the release, was taken back to it. One made after an older release is still
    offered the newer one."""
    if not latest or not tools:
        return latest or tools
    have, out = release_key(tools), release_key(latest)
    return latest if have and out and out > have else tools


def choose_mode(game, tools, latest, source_picked, kept):
    """What the builder's button does. The same order as the Windows builder (MainWindow.RefreshMode): an older game
    that still has its files is updated, a game at the newest known release is played, and an older builder updates
    itself before a new build. latest None: GitHub did not answer, so the builder's own version is the newest known.
    Newest is update_target's: a builder that is not an older release is not made to update."""
    target = update_target(latest, tools)
    if kept and game and target and game != target and not source_picked:
        return "update-game"
    if game and not source_picked:
        return "up-to-date"
    if latest and tools and target != tools:
        return "update-builder"
    return "build"


def tools_tag(root):
    """The release these tools are, from the file a package writes (VERSION, or version.txt). A source checkout has neither."""
    if not root:
        return ""
    for rel in ("usr/share/hookshot/VERSION", "usr/share/hookshot/version.txt", "VERSION", "version.txt"):
        path = os.path.join(root, rel)
        try:
            with open(path, encoding="utf-8") as f:
                line = f.readline().strip()
        except OSError:
            continue
        if line:
            return line
    return ""


def running_root():
    """The AppDir that is running: the AppImage's mount, or an installed version (its AppRun beside usr/)."""
    appdir = os.environ.get("APPDIR")
    if appdir and os.path.isfile(os.path.join(appdir, "AppRun")):
        return appdir
    tools = os.environ.get("HOOKSHOT_TOOLS")
    if tools:
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(tools))))   # (<root>/usr/share/hookshot)
        if os.path.isfile(os.path.join(root, "AppRun")):
            return root
    return None


def read_manifest(root):
    path = os.path.join(root, LIST_NAME) if root else ""
    try:
        with open(path, encoding="utf-8") as f:
            return parse_list(f.read())
    except (OSError, ValueError):
        return None


def game_version(folder):
    """The release a folder was built with, when it still has the game files an update builds from."""
    if not folder:
        return None, False
    kept = os.path.isdir(os.path.join(folder, ".hookshot_exefs")) and os.path.isdir(os.path.join(folder, "romfs"))
    version = None
    try:
        with open(os.path.join(folder, "hookshot_version.txt"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("version="):
                    version = line[8:].strip() or None
                    break
    except OSError:
        pass
    return version, kept


def game_exe(folder):
    """bin/hookshot under the launcher, or hookshot in a folder built before the launcher."""
    if not folder:
        return None
    nested = os.path.join(folder, "bin", "hookshot")
    if os.path.isfile(nested) and os.path.isfile(os.path.join(folder, "image.bin")):
        return nested
    flat = os.path.join(folder, "hookshot")
    if os.path.isfile(flat) and os.path.isfile(os.path.join(folder, "image.bin")):
        return flat
    return None


def _request(url, timeout):
    req = urllib.request.Request(url, headers={"User-Agent": "Hookshot", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.read()


def _api_download(url):
    """A GitHub release link that failed: the same file through the API (scripts/linux/deps.sh does this for 504s)."""
    prefix = "https://github.com/"
    mark = "/releases/download/"
    if not url.startswith(prefix) or mark not in url:
        return None
    repo, _, rest = url[len(prefix):].partition(mark)
    tag, _, name = rest.partition("/")
    if not repo or not tag or not name or "/" in name:
        return None
    info = _request("https://api.github.com/repos/%s/releases/tags/%s" % (repo, tag), 30)
    import json
    for asset in json.loads(info.decode("utf-8")).get("assets", []):
        if asset.get("name") == name:
            req = urllib.request.Request(asset["url"], headers={
                "User-Agent": "Hookshot", "Accept": "application/octet-stream"})
            with urllib.request.urlopen(req, timeout=120) as response:
                return response.read()
    return None


def fetch_bytes(url, timeout=60):
    """The url's bytes. A GitHub download that the server fails (5xx) is tried again, then through the API."""
    delay, last = 2, "the download failed"
    for attempt in range(4):
        try:
            return _request(url, timeout)
        except urllib.error.HTTPError as error:
            last = "GitHub answered %s" % error.code
            if error.code < 500 and error.code != 429:
                break
        except Exception as error:
            last = str(error) or error.__class__.__name__
        if attempt < 3:
            time.sleep(delay)
            delay = min(delay * 2, 20)
    try:
        got = _api_download(url)
    except Exception as error:
        got = None
        last = str(error) or last
    if got is not None:
        return got
    raise RuntimeError(last)


def latest_tag(get=None):
    """The latest release's tag, or None when GitHub did not answer. The API first (60 an hour); the setup list's tag
    otherwise, which is a file download and not that limit. Same fallback as the Windows builder."""
    get = get or (lambda url: fetch_bytes(url, 8))
    try:
        import json
        body = json.loads(get(LATEST_API).decode("utf-8"))
        tag = body.get("tag_name")
        if tag:
            return tag
    except Exception:
        pass
    try:
        text = get(LATEST_LIST).decode("utf-8")
        return parse_list(text).tag
    except Exception:
        return None


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _clone(src, dst):
    """A copy of src at dst. A hard link when the two are on one disk (the installed copy); a real copy from the
    AppImage's mount, which does not link."""
    if os.path.islink(src):
        os.symlink(os.readlink(src), dst)
        return
    try:
        os.link(src, dst)
        return
    except OSError:
        shutil.copy2(src, dst)


def _clone_tree(src, dst):
    for dirpath, dirnames, filenames in os.walk(src, followlinks=False):
        rel = os.path.relpath(dirpath, src)
        out = dst if rel == os.curdir else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        kept = []
        for name in dirnames:
            link = os.path.join(dirpath, name)
            if os.path.islink(link):
                _clone(link, os.path.join(out, name))
            else:
                kept.append(name)
        dirnames[:] = kept
        for name in filenames:
            _clone(os.path.join(dirpath, name), os.path.join(out, name))


def _safe_name(name):
    if name.startswith("/") or ".." in name.split("/"):
        raise ValueError("the zip has a path outside its folder")
    return name


def _unpack(data, dest, files):
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(__import__("io").BytesIO(data)) as zipped:
        for info in zipped.infolist():
            name = _safe_name(info.filename)
            if not name or name.endswith("/"):
                continue
            if files is not None and name not in files:
                raise ValueError("the zip has %s, which this part does not list" % name)
            target = os.path.join(dest, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode):
                os.symlink(zipped.read(info).decode("utf-8"), target)
            else:
                with open(target, "wb") as out:
                    out.write(zipped.read(info))
                if mode & 0o777:
                    os.chmod(target, mode & 0o777)


def _place(part, source_root, source, dest_root, get, note):
    dest = os.path.join(dest_root, part.dest)
    old = source.by_sha(part.sha) if source else None
    src = os.path.join(source_root, part.dest) if old and old.same(part) and source_root else ""
    ready = src and os.path.isdir(src) and (part.files is None or all(
        os.path.lexists(os.path.join(src, name)) for name in part.files))
    if ready:
        note("Keeping %s (unchanged)" % part.dest)
        if part.files is None:
            _clone_tree(src, dest)
        else:
            os.makedirs(dest, exist_ok=True)
            for name in part.files:
                _clone(os.path.join(src, name), os.path.join(dest, name))
        return
    note("Downloading %s (%d MB)" % (part.dest, part.size >> 20))
    data = get(part.url)
    if len(data) != part.size or _sha256(data) != part.sha:
        raise RuntimeError("%s did not download correctly (its checksum differs)" % part.dest)
    _unpack(data, dest, part.files)


def _finish_tree(dest):
    """AppRun at the root, from the tools just installed, so this folder starts the way the AppImage does."""
    tools = os.path.join(dest, "usr", "share", "hookshot")
    apprun = os.path.join(dest, "AppRun")
    shutil.copyfile(os.path.join(tools, "builder", "linux", "AppRun"), apprun)
    os.chmod(apprun, 0o755)
    icon = os.path.join(tools, "assets", "hookshot.png")
    if os.path.isfile(icon):
        shutil.copyfile(icon, os.path.join(dest, "hookshot.png"))
    desktop = os.path.join(tools, "builder", "linux", "hookshot-builder.desktop")
    if os.path.isfile(desktop):
        shutil.copyfile(desktop, os.path.join(dest, "hookshot-builder.desktop"))
    return apprun


def install(manifest, source_root, source, note=None, get=None):
    """versions/<tag> from the list. Unchanged parts come from source_root. The new folder replaces nothing until it
    is complete, and the version installed before stays until this one is."""
    note = note or (lambda message: None)
    get = get or fetch_bytes
    root = data_root()
    versions = os.path.join(root, "versions")
    dest = os.path.join(versions, manifest.tag)
    partial = dest + ".part"
    if os.path.isdir(partial):
        shutil.rmtree(partial)
    os.makedirs(versions, exist_ok=True)
    try:
        for part in manifest.parts:
            _place(part, source_root, source, partial, get, note)
        with open(os.path.join(partial, LIST_NAME), "w", encoding="utf-8", newline="\n") as handle:
            handle.write("tag %s\n" % manifest.tag)
            for part in manifest.parts:
                handle.write("part %s %d %s %s\n" % (part.sha, part.size, part.dest, part.url))
                handle.write("tree\n" if part.files is None else "".join("file %s\n" % name for name in part.files))
        apprun = _finish_tree(partial)
    except Exception:
        shutil.rmtree(partial, ignore_errors=True)
        raise
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.rename(partial, dest)
    current = os.path.join(root, "current")
    tmp = current + ".new"
    with open(tmp, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(manifest.tag + "\n")
    os.replace(tmp, current)
    for name in os.listdir(versions):
        if name != manifest.tag:
            shutil.rmtree(os.path.join(versions, name), ignore_errors=True)
    return os.path.join(dest, "AppRun") if os.path.isfile(os.path.join(dest, "AppRun")) else apprun


def installed_apprun():
    root = data_root()
    try:
        with open(os.path.join(root, "current"), encoding="utf-8") as handle:
            tag = handle.read().strip()
    except OSError:
        return ""
    path = os.path.join(root, "versions", tag, "AppRun")
    return path if tag and os.path.isfile(path) else ""


def fetch_manifest(get=None):
    get = get or (lambda url: fetch_bytes(url, 30))
    override = os.environ.get("HOOKSHOT_LINUX_LIST")
    if override:
        with open(override, encoding="utf-8") as handle:
            return parse_list(handle.read())
    return parse_list(get(LATEST_LIST).decode("utf-8"))


def startup_manifest(root, get=None):
    """The list begin_update installs on launch: the latest release, except when these tools ship a list of their own
    that is that same release and no older than it (a self-contained -rc or Actions tab AppImage). Reinstalling an
    older release's parts over those tools replaces files the working tools had (issue 990: a -rc shipped
    gpu/vk/oot_cheat_items.h, the release's zip did not, and every launch lost it). The comparison is update_target's:
    a builder that is not an older release still updates. A HOOKSHOT_LINUX_LIST is a deliberate override: it wins."""
    latest = fetch_manifest(get)
    if not os.environ.get("HOOKSHOT_LINUX_LIST"):
        own = read_manifest(root)
        if own and own.tag == tools_tag(root) and update_target(latest.tag, own.tag) == own.tag:
            return own
    return latest
