#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# A second launch with --load-extension on a directory the profile already holds reloads that extension while the
# browser starts, and the reload drops the extension's session storage through its store. The store posts its result
# to the event loop, so the browser needs one before it restores the installed extensions.

import argparse
import json
import subprocess
import tempfile

from pathlib import Path

TIMEOUT_SECONDS = 60


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    with tempfile.TemporaryDirectory(prefix="ladybird-reload-unpacked-extension-") as root:
        extension = Path(root) / "extension"
        extension.mkdir()
        (extension / "manifest.json").write_text(
            json.dumps({"manifest_version": 3, "name": "Reloaded", "version": "1.0"})
        )
        profile = Path(root) / "profile"

        for launch in (1, 2):
            result = subprocess.run(
                [
                    ladybird_binary,
                    "--headless=text",
                    f"--profile-path={profile}",
                    f"--load-extension={extension}",
                    "about:blank",
                ],
                capture_output=True,
                text=True,
                timeout=TIMEOUT_SECONDS,
            )
            if result.returncode != 0:
                print(f"FAIL: launch {launch} exited with {result.returncode}")
                print(result.stderr[-3000:])
                return 1

        installed = json.loads((profile / "config" / "Extensions.json").read_text())["extensions"]
        if [entry["path"] for entry in installed] != [str(extension.resolve())]:
            print(f"FAIL: the profile lists {installed} instead of the one reloaded extension")
            return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
