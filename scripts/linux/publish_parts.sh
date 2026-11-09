#!/bin/bash
# Upload the Linux builder's parts (scripts/linux/make_parts.py) to the linux-parts release, which is not "latest".
# A zip whose name is already there is left (its checksum is in the name). hookshot-linux.txt itself goes on the
# version release, with the AppImage. Run from CI on a tag, with GH_TOKEN and GITHUB_REPOSITORY.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PARTS=${1:-$ROOT/build-linux/parts}
REPO=${GITHUB_REPOSITORY:?}
TOKEN=${GH_TOKEN:?}
API="https://api.github.com/repos/$REPO"
AUTH=(-H "Authorization: Bearer $TOKEN" -H "Accept: application/vnd.github+json" -H "User-Agent: Hookshot")

code=$(curl -s -o /tmp/linux-parts.json -w '%{http_code}' "${AUTH[@]}" "$API/releases/tags/linux-parts")
if [ "$code" = "404" ]; then
    curl -fsSL "${AUTH[@]}" -X POST "$API/releases" -d "$(python3 - <<'PY'
import json, os
print(json.dumps({
    "tag_name": "linux-parts",
    "target_commitish": os.environ["GITHUB_SHA"],
    "name": "Linux builder parts",
    "body": "Parts Hookshot's Linux builder downloads. A file's name is its checksum; a part that did not change is not uploaded again. This release is not a version of the game.",
    "prerelease": True,
    "make_latest": "false",
}))
PY
)" > /tmp/linux-parts.json
fi
python3 - <<'PY'
import json
json.load(open("/tmp/linux-parts.json"))["upload_url"]
PY
upload=$(python3 -c 'import json; print(json.load(open("/tmp/linux-parts.json"))["upload_url"].split("{")[0])')
have=$(python3 -c 'import json; print("\n".join(a["name"] for a in json.load(open("/tmp/linux-parts.json")).get("assets", [])))')
for zip in "$PARTS"/HookshotLinux-*.zip; do
    [ -f "$zip" ] || continue
    name=$(basename "$zip")
    if printf '%s\n' "$have" | grep -qx "$name"; then
        echo "already on linux-parts: $name"
        continue
    fi
    echo "uploading $name"
    curl -fsSL --retry 5 --retry-delay 10 -X POST "${AUTH[@]}" -H "Content-Type: application/zip" \
        --data-binary @"$zip" "$upload?name=$name" >/dev/null
done
echo "parts published"
