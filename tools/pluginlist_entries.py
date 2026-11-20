#!/usr/bin/env python3
"""Print nppPluginList JSON entries (x86 / x64 / arm64) for the release zips.

usage: pluginlist_entries.py DIST_DIR VERSION OWNER/REPO
"""
import hashlib
import json
import os
import sys

DESCRIPTION = (
    "Interactive, collapsible tree view of YANG modules (RFC 6020/7950) opened in your browser: "
    "containers, lists, choices, groupings inlined across imports and submodules, search, schema "
    "paths, types/keys/config badges. Also installs YANG syntax highlighting. "
    "Native DLL, no Python or other runtime needed."
)


def main(dist, ver, repo):
    out = {}
    for arch in ("x86", "x64", "arm64"):
        name = "YangTreeView-%s-%s.zip" % (ver, arch)
        path = os.path.join(dist, name)
        if not os.path.exists(path):
            continue
        with open(path, "rb") as fh:
            sha = hashlib.sha256(fh.read()).hexdigest()
        out["pl.%s.json" % arch] = {
            "folder-name": "YangTreeView",
            "display-name": "YANG Tree View",
            "version": ver,
            "npp-compatible-versions": "[8.0,]",
            "id": sha,
            "repository": "https://github.com/%s/releases/download/v%s/%s" % (repo, ver, name),
            "description": DESCRIPTION,
            "author": "Roger Sacchelli",
            "homepage": "https://github.com/%s" % repo,
        }
    json.dump(out, sys.stdout, indent=4)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main(*sys.argv[1:4])
