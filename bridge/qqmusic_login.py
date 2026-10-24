# -*- coding: utf-8 -*-
"""QQ Music browser-assisted login.

Tencent blocks plain HTTP QR polling (403). Instead we open the official
y.qq.com login page in a real Edge window driven by Playwright; the user signs
in normally with their own QR scan. When the session cookies appear we copy
them into the local account store. No anti-bot bypass, no credential capture.
"""

from __future__ import annotations

import threading
import time
from typing import Any

from playwright.sync_api import sync_playwright

LOGIN_URL = "https://y.qq.com/"
NEEDED = ("qm_keyst", "qqmusic_key", "p_skey", "skey", "uin", "qm_uin", "p_uin")


def _harvest(context) -> dict[str, str]:
    cookies = {}
    for domain in ("y.qq.com", "qq.com", "ptlogin2.qq.com"):
        try:
            for cookie in context.cookies("https://%s" % domain):
                cookies[cookie["name"]] = cookie["value"]
        except Exception:
            pass
    return cookies


def _has_session(cookies: dict[str, str]) -> bool:
    return any(key in cookies for key in ("qm_keyst", "qqmusic_key", "p_skey"))


def run_login(window_seconds: int = 180) -> dict[str, Any]:
    """Blocking: opens a visible login window until signed in or timeout."""
    with sync_playwright() as pw:
        browser = pw.chromium.launch(channel="msedge", headless=False)
        context = browser.new_context()
        page = context.new_page()
        page.goto(LOGIN_URL, timeout=45000)
        deadline = time.time() + window_seconds
        result: dict[str, Any] = {"logged_in": False}
        while time.time() < deadline:
            cookies = _harvest(context)
            if _has_session(cookies):
                result = {"logged_in": True, "cookies": cookies}
                break
            time.sleep(1.5)
        browser.close()
        return result
