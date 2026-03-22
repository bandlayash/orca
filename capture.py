#!/usr/bin/env python3
"""Playwright-based browser log capture for Dolphin WASM crash testing.

Usage:
    python3 capture.py [rom_path] [log_path] [timeout_seconds]

Defaults:
    rom_path  = /home/yb/Downloads/Super Paper Mario [R8PE01].wbfs
    log_path  = /tmp/dolphin_browser.log
    timeout   = 90
"""

import sys
import asyncio
import subprocess
import os
from datetime import datetime
from pathlib import Path

ROM_PATH  = sys.argv[1] if len(sys.argv) > 1 else "/home/yb/Downloads/Super Paper Mario [R8PE01].wbfs"
LOG_PATH  = sys.argv[2] if len(sys.argv) > 2 else "/tmp/dolphin_browser.log"
TIMEOUT   = int(sys.argv[3]) if len(sys.argv) > 3 else 180
PORT      = 8080


VENV_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".venv-capture")


def ensure_playwright():
    venv_python = os.path.join(VENV_DIR, "bin", "python3")

    # Create venv if it doesn't exist
    if not os.path.isfile(venv_python):
        print("Creating venv for capture...", flush=True)
        subprocess.check_call([sys.executable, "-m", "venv", VENV_DIR])

    # Install playwright if not importable from venv
    result = subprocess.run(
        [venv_python, "-c", "import playwright"],
        capture_output=True,
    )
    if result.returncode != 0:
        print("Installing playwright in venv...", flush=True)
        subprocess.check_call([venv_python, "-m", "pip", "install", "playwright", "-q"])

    # Ensure chromium browser binary is present
    result = subprocess.run(
        [venv_python, "-m", "playwright", "install", "chromium"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        sys.exit(1)

    # Re-exec under the venv python so imports resolve correctly
    if sys.executable != venv_python and not sys.prefix.startswith(VENV_DIR):
        os.execv(venv_python, [venv_python] + sys.argv)


async def capture():
    from playwright.async_api import async_playwright

    logs = []
    crash_event = asyncio.Event()

    def log(msg):
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        line = f"[{ts}] {msg}"
        logs.append(line)
        print(line, flush=True)

    async def on_console(msg):
        log(f"[console.{msg.type}] {msg.text}")
        # Try to extract full argument values (may contain stack traces)
        try:
            for i, arg in enumerate(msg.args):
                val = await arg.json_value()
                if val and str(val) != msg.text:
                    log(f"  arg[{i}]: {val}")
        except Exception:
            pass
        loc = msg.location
        if loc and loc.get("url"):
            log(f"  at {loc['url']}:{loc.get('lineNumber', '?')}:{loc.get('columnNumber', '?')}")
        # Dolphin worker errors arrive as console warnings from main.js
        if "worker sent an error" in msg.text or "unreachable" in msg.text.lower():
            crash_event.set()

    def on_pageerror(err):
        err_msg = getattr(err, 'message', str(err))
        err_stack = getattr(err, 'stack', '')
        log(f"[pageerror] {err_msg}")
        if err_stack:
            log(f"[pageerror-stack] {err_stack}")
        crash_event.set()

    def on_worker(worker):
        log(f"[worker] created: {worker.url}")

    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=True,
            args=[
                "--no-sandbox",
                "--disable-dev-shm-usage",
                # SharedArrayBuffer requires COOP/COEP — served by serve.py
            ],
        )

        context = await browser.new_context()
        page = await context.new_page()

        page.on("console",   on_console)
        page.on("pageerror", on_pageerror)
        page.on("worker",    on_worker)

        log(f"Navigating to http://localhost:{PORT}/")
        await page.goto(f"http://localhost:{PORT}/", wait_until="domcontentloaded")

        log("Waiting for Dolphin WASM to initialize (rom-input enabled)...")
        try:
            await page.wait_for_function(
                "() => !document.getElementById('rom-input').disabled",
                timeout=30_000,
            )
        except Exception as e:
            log(f"[TIMEOUT] Dolphin never became ready: {e}")
            await browser.close()
            Path(LOG_PATH).write_text("\n".join(logs))
            print(f"\nLogs saved to {LOG_PATH}")
            sys.exit(2)

        log(f"Dolphin ready. Loading ROM: {ROM_PATH}")
        await page.set_input_files("#rom-input", ROM_PATH)

        log(f"ROM submitted. Waiting up to {TIMEOUT}s for crash or stable run...")
        try:
            await asyncio.wait_for(crash_event.wait(), timeout=TIMEOUT)
            log("=== CRASH DETECTED ===")
            exit_code = 1
            # Capture screenshot on crash
            try:
                screenshot_path = LOG_PATH.replace(".log", "_crash.png")
                await page.screenshot(path=screenshot_path, full_page=True)
                log(f"Screenshot saved to {screenshot_path}")
            except Exception as e:
                log(f"Screenshot failed: {e}")
        except asyncio.TimeoutError:
            log(f"=== TIMEOUT ({TIMEOUT}s) — no crash detected, game may be running ===")
            exit_code = 0

        await browser.close()

    Path(LOG_PATH).write_text("\n".join(logs))
    print(f"\nLogs saved to {LOG_PATH}")
    sys.exit(exit_code)


if __name__ == "__main__":
    ensure_playwright()
    asyncio.run(capture())
