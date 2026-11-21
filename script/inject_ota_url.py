"""Inject OTA release URL from the build environment.

CI (GitHub Actions) sets OTA_GITHUB_REPO=${{ github.repository }}, so every
firmware built in the fork points its online-upgrade check at that fork's own
Releases page. Local builds can override it the same way:

    OTA_GITHUB_REPO=owner/repo pio run -e esp32c6_supermini

If unset, src/ota/ota_manager.h keeps its default (upstream repository).
"""

Import("env")  # noqa: F821 — SCons injects this

import os

repo = os.environ.get("OTA_GITHUB_REPO", "").strip()
if not repo:
    # 不在 CI 中时不做覆盖，保留 ota_manager.h 里的默认值（上游仓库）
    print("[inject_ota_url] OTA_GITHUB_REPO not set, keep default OTA URL")
else:
    url = "https://github.com/%s/releases" % repo
    env.Append(CPPDEFINES=[("OTA_RELEASES_BASE_URL", '\\"' + url + '\\"')])  # noqa: F821
    print("[inject_ota_url] OTA_RELEASES_BASE_URL=%s" % url)
