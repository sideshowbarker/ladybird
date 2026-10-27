#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import json
import runpy
import subprocess
import sys
import tempfile
import time

from pathlib import Path


def run_test(webdriver_binary):
    helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        config = root / "config" / "Settings.json"
        config.parent.mkdir()
        good_id = "11111111-1111-4111-8111-111111111111"
        missing_id = "22222222-2222-4222-8222-222222222222"
        removed_id = "33333333-3333-4333-8333-333333333333"
        good = root / "good"
        missing = root / "missing"
        removed = root / "removed"
        good.mkdir()
        manifest = {
            "manifest_version": 3,
            "name": "<b>Extension name</b>",
            "version": "1.2",
            "description": "<img src=x>plain text",
            "permissions": ["storage"],
            "host_permissions": ["https://example.test/*"],
        }
        (good / "manifest.json").write_text(json.dumps(manifest))
        entries = [
            {
                "id": identifier,
                "path": str(directory),
                "enabled": True,
                "allowed_in_private": False,
                "granted_permissions": ["storage"],
            }
            for identifier, directory in [(good_id, good), (missing_id, missing), (removed_id, removed)]
        ]
        config.write_text(json.dumps({"extensions": entries, "firstRunComplete": True}))
        storage_files = {}
        for identifier in (good_id, removed_id):
            storage = root / "data" / "extensions" / identifier / "storage-local.json"
            storage.parent.mkdir(parents=True)
            storage.write_text(json.dumps({"formatVersion": 1, "items": {"kept": True}}))
            storage_files[identifier] = storage

        port = helpers["unused_port"]()
        process = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port), "--profile-path", str(root)]
        )
        session = None
        try:
            helpers["wait_for_port"](port)
            session = helpers["create_session"](port)

            def request(method, path, body=None):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def command(path, body):
                return request("POST", path, body)

            def script(source, *args):
                return command("execute/sync", {"script": source, "args": list(args)})

            def wait(predicate):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if predicate():
                        return
                    time.sleep(0.05)
                raise AssertionError("Timed out waiting for the extension operation")

            def click(selector):
                element = command("element", {"using": "css selector", "value": selector})
                command(f"element/{element['element-6066-11e4-a52e-4f735466cecf']}/click", {})

            def row(identifier):
                return f'[data-extension-id="{identifier}"]'

            def snapshot():
                return command(
                    "execute/async",
                    {
                        "script": """
                            const done = arguments[0];
                            document.addEventListener('WebUIMessage', function listener(event) {
                                if (event.detail.name !== 'extensionsSnapshot') return;
                                document.removeEventListener('WebUIMessage', listener);
                                done(event.detail.data);
                            });
                            ladybird.sendMessage('loadExtensions');
                        """,
                        "args": [],
                    },
                )

            def installed(identifier):
                return next(
                    (entry for entry in json.loads(config.read_text())["extensions"] if entry["id"] == identifier), None
                )

            def installed_entry(identifier):
                entry = installed(identifier)
                assert entry is not None, identifier
                return entry

            command("url", {"url": "about:extensions"})
            wait(lambda: script("return document.querySelectorAll('[data-extension-id]').length === 3;"))
            first_window = request("GET", "window")
            values = snapshot()
            assert [entry["id"] for entry in values] == [good_id, missing_id, removed_id], values
            assert values[0] == {
                "id": good_id,
                "name": manifest["name"],
                "version": "1.2",
                "description": manifest["description"],
                "hasOptions": False,
                "directory": str(good),
                "enabled": True,
                "allowedInPrivate": False,
                "permissions": ["storage", "https://example.test/*"],
                "error": "",
            }, values[0]
            assert values[1]["name"] == "missing" and values[1]["error"], values[1]
            assert values[1]["version"] == values[1]["description"] == "", values[1]
            assert (
                script("return document.querySelector(arguments[0] + ' .extension-name').textContent;", row(good_id))
                == manifest["name"]
            )
            assert not script(
                "return !!document.querySelector(arguments[0] + ' b, ' + arguments[0] + ' img');", row(good_id)
            )
            print("page_lists_snapshot_fields_and_plain_text: PASS", flush=True)

            second_window = command("window/new", {"type": "tab"})["handle"]
            command("window", {"handle": second_window})
            command("url", {"url": "about:extensions"})
            wait(lambda: script("return document.querySelectorAll('[data-extension-id]').length === 3;"))
            command("window", {"handle": first_window})
            click(row(good_id) + " .extension-enabled")
            wait(lambda: installed_entry(good_id)["enabled"] is False)
            wait(
                lambda: script(
                    "return !document.querySelector(arguments[0]).checked;", row(good_id) + " .extension-enabled"
                )
            )
            assert storage_files[good_id].exists()
            command("window", {"handle": second_window})
            wait(
                lambda: script(
                    "return !document.querySelector(arguments[0]).checked;", row(good_id) + " .extension-enabled"
                )
            )
            print("second_page_sees_the_change: PASS", flush=True)
            command("window", {"handle": first_window})
            click(row(good_id) + " .extension-enabled")
            wait(lambda: installed_entry(good_id)["enabled"] is True)
            wait(
                lambda: script(
                    "return document.querySelector(arguments[0]).checked;", row(good_id) + " .extension-enabled"
                )
            )
            print("disable_and_enable_update_settings_and_row: PASS", flush=True)
            click(row(good_id) + " .extension-private")
            wait(lambda: installed_entry(good_id)["allowed_in_private"] is True)
            wait(
                lambda: script(
                    "return document.querySelector(arguments[0]).checked;", row(good_id) + " .extension-private"
                )
            )
            print("allow_in_private_updates_settings: PASS", flush=True)

            click(row(removed_id) + " .extension-remove")
            wait(lambda: script("return document.querySelector('#remove-dialog').open;"))
            assert installed(removed_id) and storage_files[removed_id].exists()
            click("#cancel-remove")
            assert installed(removed_id)
            click(row(removed_id) + " .extension-remove")
            click("#confirm-remove")
            wait(lambda: installed(removed_id) is None and not storage_files[removed_id].exists())
            wait(lambda: script("return !document.querySelector(arguments[0]);", row(removed_id)))
            print("failed_entry_is_listed_and_removable: PASS", flush=True)

            missing.mkdir()
            (missing / "manifest.json").write_text(json.dumps({**manifest, "name": "Recovered"}))
            click(row(missing_id) + " .extension-reload")
            wait(
                lambda: script(
                    "return document.querySelector(arguments[0] + ' .extension-name').textContent === 'Recovered';",
                    row(missing_id),
                )
            )
            repaired = next(entry for entry in snapshot() if entry["id"] == missing_id)
            assert repaired["error"] == "" and repaired["version"] == "1.2", repaired
            print("reload_of_a_fixed_entry_loads_it: PASS", flush=True)

            click(row(good_id) + " .extension-remove")
            wait(lambda: script("return document.querySelector('#remove-dialog').open;"))
            assert "data" in script("return document.querySelector('#remove-dialog').textContent;")
            click("#confirm-remove")
            wait(lambda: installed(good_id) is None and not storage_files[good_id].exists())
            assert (good / "manifest.json").exists()
            print("remove_confirmation_deletes_entry_and_storage: PASS", flush=True)

            command("window", {"handle": second_window})
            wait(lambda: script("return document.querySelectorAll('[data-extension-id]').length === 1;"))
            command("url", {"url": "resource://ladybird/about-pages/extensions.html"})
            assert script("return typeof ladybird === 'undefined' || typeof ladybird.sendMessage === 'undefined';")
            print("resource_page_has_no_management_channel: PASS", flush=True)
        finally:
            if session:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    run_test(sys.argv[1])
