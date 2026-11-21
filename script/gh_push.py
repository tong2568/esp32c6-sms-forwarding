#!/usr/bin/env python3
"""Fork + commit + push this project to GitHub using only the REST API.

Why a script: the environment may not have git installed, and we must not
require it. It uploads the current working tree as one commit on top of the
fork's existing master (history is preserved), then optionally publishes a
Release with the files in ./release.

Required environment:
  GH_TOKEN   GitHub token. Classic PAT with `public_repo` (fork + write), or a
             fine-grained token with Contents: Read/Write on the target repo
             (forking still needs a classic token unless the fork already exists).
  GH_USER    Your GitHub username, e.g. "octocat".

Optional environment:
  GH_REPO        target repository name (default: esp32c6-sms-forwarding)
  GH_UPSTREAM    upstream owner/name (default: maxming2333/esp32-sms-forwarding)
  GH_BRANCH      branch to update (default: repo default branch, usually master)
  GH_MESSAGE     commit message
  GH_TAG         if set, create this tag + release and upload ./release/*
  GH_DRY_RUN=1   print what would be done, do not write

Usage:
  GH_TOKEN=xxx GH_USER=yyy python3 script/gh_push.py
  GH_TOKEN=xxx GH_USER=yyy GH_TAG=v1.0.0 python3 script/gh_push.py
"""

import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request

API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"

TOKEN = os.environ.get("GH_TOKEN", "").strip()
USER = os.environ.get("GH_USER", "").strip()
REPO = os.environ.get("GH_REPO", "esp32c6-sms-forwarding").strip()
UPSTREAM = os.environ.get("GH_UPSTREAM", "maxming2333/esp32-sms-forwarding").strip()
BRANCH = os.environ.get("GH_BRANCH", "").strip()
MESSAGE = os.environ.get("GH_MESSAGE", "feat: port SMS forwarder to ESP32-C6 SuperMini").strip()
TAG = os.environ.get("GH_TAG", "").strip()
DRY = os.environ.get("GH_DRY_RUN", "") == "1"

SKIP_DIRS = {".git", ".pio", "release", ".platformio", "__pycache__", ".codegraph"}
SKIP_FILES = {"sms-forwarding.tar.gz", "get-pip.py"}
SKIP_EXT = {".png", ".jpg", ".jpeg", ".gif", ".ico", ".bin", ".elf", ".gz", ".zip", ".pdf", ".tar"}


def die(msg):
    print("ERROR:", msg, file=sys.stderr)
    sys.exit(1)


def api(method, path, data=None, base=API, raw=False):
    url = base + path
    body = None
    headers = {
        "Authorization": "Bearer " + TOKEN,
        "Accept": "application/vnd.github+json",
        "User-Agent": "sms-forwarding-c6-porter",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if data is not None:
        body = json.dumps(data).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            payload = resp.read()
            return resp.status, (payload if raw else (json.loads(payload) if payload else {}))
    except urllib.error.HTTPError as e:
        text = e.read().decode("utf-8", "replace")
        return e.code, text


def collect_files(root):
    entries = []
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        parts = set(p.relative_to(root).parts)
        if parts & SKIP_DIRS:
            continue
        if rel in SKIP_FILES or p.suffix.lower() in SKIP_EXT:
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue  # 二进制或不可读文件跳过（保留 fork 中已有版本）
        entries.append({"path": rel, "mode": "100644", "type": "blob", "content": text})
    return entries


def main():
    if not TOKEN or not USER:
        die("请设置 GH_TOKEN 和 GH_USER 环境变量")

    root = pathlib.Path(__file__).resolve().parent.parent
    files = collect_files(root)
    print(f"[push] {len(files)} text files -> github.com/{USER}/{REPO}")

    # 1) fork（若目标仓库已存在则直接复用；否则 fork 后改名）
    fork_name = UPSTREAM.split("/", 1)[1]
    if not DRY:
        st, info = api("GET", f"/repos/{USER}/{REPO}")
        if st != 200:
            st, _ = api("POST", f"/repos/{UPSTREAM}/forks", {})
            if st not in (202, 204, 422):
                print(f"[push] fork returned HTTP {st} (continue if the fork already exists)")

            # 等 fork 就绪（fork 默认沿用上游名）
            for _ in range(30):
                st, _ = api("GET", f"/repos/{USER}/{fork_name}")
                if st == 200:
                    break
                time.sleep(2)
            else:
                die(f"fork {USER}/{fork_name} 未就绪，请稍后重试")

            # 重命名为目标仓库名
            if REPO != fork_name:
                st, r = api("PATCH", f"/repos/{USER}/{fork_name}", {"name": REPO})
                if st not in (200, 201):
                    die(f"重命名仓库失败: {r}")
                for _ in range(30):
                    st, _ = api("GET", f"/repos/{USER}/{REPO}")
                    if st == 200:
                        break
                    time.sleep(2)
                else:
                    die(f"重命名后 {USER}/{REPO} 未就绪")
    else:
        st, info = api("GET", f"/repos/{USER}/{REPO}")
        if st != 200:
            die(f"dry-run: 仓库 {USER}/{REPO} 不存在（HTTP {st}）")

    branch = BRANCH or info.get("default_branch", "master")

    # 2) 当前 HEAD
    st, ref = api("GET", f"/repos/{USER}/{REPO}/git/ref/heads/{branch}")
    if st != 200:
        die(f"读取分支 {branch} 失败: {ref}")
    head_sha = ref["object"]["sha"]

    st, commit = api("GET", f"/repos/{USER}/{REPO}/git/commits/{head_sha}")
    if st != 200:
        die(f"读取 HEAD commit 失败: {commit}")
    base_tree = commit["tree"]["sha"]

    # 3) 新 tree + commit
    if DRY:
        print(f"[push] dry-run: would create commit on {branch} ({head_sha[:7]}) with {len(files)} files")
        return
    st, tree = api("POST", f"/repos/{USER}/{REPO}/git/trees", {"base_tree": base_tree, "tree": files})
    if st not in (200, 201):
        die(f"创建 tree 失败: {tree}")
    st, new_commit = api(
        "POST",
        f"/repos/{USER}/{REPO}/git/commits",
        {"message": MESSAGE, "tree": tree["sha"], "parents": [head_sha]},
    )
    if st not in (200, 201):
        die(f"创建 commit 失败: {new_commit}")
    new_sha = new_commit["sha"]

    # 4) 更新分支
    st, updated = api(
        "PATCH",
        f"/repos/{USER}/{REPO}/git/refs/heads/{branch}",
        {"sha": new_sha, "force": False},
    )
    if st not in (200, 201):
        die(f"更新分支失败: {updated}")
    print(f"[push] OK https://github.com/{USER}/{REPO}/commit/{new_sha}")

    # 5) 可选 Release
    if TAG:
        st, _ = api("POST", f"/repos/{USER}/{REPO}/git/refs", {"ref": f"refs/tags/{TAG}", "sha": new_sha})
        if st not in (200, 201, 422):
            print(f"[push] tag {TAG} returned HTTP {st}")
        st, rel = api(
            "POST",
            f"/repos/{USER}/{REPO}/releases",
            {"tag_name": TAG, "name": TAG, "body": f"ESP32-C6 SuperMini build {TAG}", "draft": False},
        )
        if st not in (200, 201):
            die(f"创建 Release 失败: {rel}")
        rel_id = rel["id"]
        for f in sorted((root / "release").glob("*")):
            if not f.is_file():
                continue
            data = f.read_bytes()
            req = urllib.request.Request(
                f"{UPLOADS}/repos/{USER}/{REPO}/releases/{rel_id}/assets?name={urllib.parse.quote(f.name)}",
                data=data,
                headers={
                    "Authorization": "Bearer " + TOKEN,
                    "Content-Type": "application/octet-stream",
                    "User-Agent": "sms-forwarding-c6-porter",
                },
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=300) as resp:
                print(f"[push] uploaded {f.name} ({len(data)} bytes, HTTP {resp.status})")
        print(f"[push] release https://github.com/{USER}/{REPO}/releases/tag/{TAG}")


if __name__ == "__main__":
    import urllib.parse  # noqa: E402  (used for asset names)

    main()
