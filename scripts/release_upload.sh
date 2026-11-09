#!/bin/bash
# A tag's GitHub release with these files attached. The Linux AppImage and Windows builder workflows both call it: the
# first to get there makes the release (from the tag, with GitHub's generated notes), the other adds its files to it, so
# the two never race to make two releases. A release candidate (a tag with -rc: v0.2.0-rc.1) is a prerelease and never
# "latest". Needs gh, with GH_TOKEN (contents: write) and GH_REPO set. No game data is involved.
#
#   scripts/release_upload.sh <tag> <file> ...
set -euo pipefail
tag=$1
shift
if [[ $tag == *-rc* ]]; then
    kind=(--prerelease --latest=false)
else
    kind=(--latest)
fi
if ! gh release view "$tag" >/dev/null 2>&1; then
    # (the other workflow can make it between the view and the create: its release is then the one used)
    gh release create "$tag" --verify-tag --title "$tag" --generate-notes "${kind[@]}" ||
        gh release view "$tag" >/dev/null
fi
gh release upload "$tag" --clobber "$@"
