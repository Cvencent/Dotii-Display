#!/usr/bin/env python3
"""LAN bridge and loopback-only management UI for Dotii.

The ESP32-facing schema v1 endpoint is intentionally kept compatible with the
current firmware. Administrative data and static UI assets are only available
from the local computer.
"""

from __future__ import annotations

import argparse
import copy
import ipaddress
import json
import logging
import mimetypes
import os
import re
import secrets
import shutil
import socket
import sys
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

from pypinyin import Style, lazy_pinyin

from bluetooth_bridge import BluetoothBridge
from codex_bridge_link import claim_codex_bridge_token
from firmware_flasher import FirmwareFlasher
import requests
from qqmusic import QQMusicAccount, QQMusicNotLoggedIn
from platforms import current_platform
from runtime_paths import (
    application_root,
    application_version,
    is_frozen,
    project_root,
    resource_path,
    runtime_folder,
    sibling_executable,
    tools_root,
)


SCHEMA_VERSION = 1
MAX_BODY = 32 * 1024
MODULE_DEFAULTS = {"xiaozhi": True}

XIAOZHI_DEFAULTS = {
    "enabled": True,
    "server_url": "https://api.tenclass.net/xiaozhi/ota/",
    # Device-facing canonical pinyin list. `wake_words` below is the editable
    # Chinese form shown by the management center.
    "wake_word": "ni hao xiao zhi",
    "wake_words": ["你好小智"],
    "voice": "default",
    "mcp_enabled": True,
    # Tool access point published by the xiaozhi console
    # (wss://api.xiaozhi.me/mcp/?token=...). Tools are registered here.
    "mcp_url": "",
}

WAKE_WORD_MAX_COUNT = 3
WAKE_WORD_INPUT_MAX = 24
WAKE_WORD_PINYIN_MAX = 63
_WAKE_WORD_INPUT_RE = re.compile(r"^[\u3400-\u9fffA-Za-z0-9 ]+$")
_ASCII_PINYIN_RE = re.compile(r"^[a-z0-9 ]+$")


def default_xiaozhi_config() -> dict[str, Any]:
    return copy.deepcopy(XIAOZHI_DEFAULTS)


def _wake_word_to_pinyin(phrase: str, index: int) -> str:
    phrase = " ".join(phrase.split())
    if not phrase:
        raise ValueError(f"xiaozhi.wake_words[{index}] must not be empty")
    if len(phrase) > WAKE_WORD_INPUT_MAX:
        raise ValueError(
            f"xiaozhi.wake_words[{index}] must be at most {WAKE_WORD_INPUT_MAX} characters"
        )
    if not _WAKE_WORD_INPUT_RE.fullmatch(phrase):
        raise ValueError(
            f"xiaozhi.wake_words[{index}] may contain only Chinese, letters, digits and spaces"
        )

    lowered = phrase.lower()
    if _ASCII_PINYIN_RE.fullmatch(lowered):
        parts = lowered.split()
    else:
        rendered = " ".join(lazy_pinyin(lowered, style=Style.NORMAL, errors="default"))
        parts = re.findall(r"[a-z0-9]+", rendered)
    pinyin = " ".join(parts)
    if not pinyin:
        raise ValueError(f"xiaozhi.wake_words[{index}] cannot be converted to pinyin")
    if len(pinyin) > WAKE_WORD_PINYIN_MAX:
        raise ValueError(
            f"xiaozhi.wake_words[{index}] is too long after pinyin conversion"
        )
    return pinyin


def _validate_wake_words(payload: dict[str, Any], defaults: dict[str, Any]) -> tuple[list[str], str]:
    raw_words = payload.get("wake_words")
    if raw_words is None:
        if "wake_word" not in payload:
            raw_words = list(defaults["wake_words"])
        else:
            # Accept files and clients written before multi-word support. A
            # legacy semicolon-separated `wake_word` is already canonical pinyin.
            legacy = payload["wake_word"]
            if not isinstance(legacy, str):
                raise ValueError("xiaozhi.wake_word must be a string")
            if ";" in legacy:
                raw_words = [part.strip() for part in legacy.split(";") if part.strip()]
            else:
                raw_words = [legacy]
    if not isinstance(raw_words, list):
        raise ValueError("xiaozhi.wake_words must be an array")
    if len(raw_words) > WAKE_WORD_MAX_COUNT:
        raise ValueError(f"xiaozhi.wake_words supports at most {WAKE_WORD_MAX_COUNT} words")

    words: list[str] = []
    pinyin_words: list[str] = []
    for index, value in enumerate(raw_words):
        if not isinstance(value, str):
            raise ValueError(f"xiaozhi.wake_words[{index}] must be a string")
        phrase = " ".join(value.split())
        if not phrase:
            continue
        pinyin = _wake_word_to_pinyin(phrase, index)
        if pinyin in pinyin_words:
            continue
        words.append(phrase)
        pinyin_words.append(pinyin)

    if not words:
        # The device keeps WakeNet as a safe fallback when the custom list is
        # empty, but the management form always starts with one usable row.
        words = list(defaults["wake_words"])
        pinyin_words = [_wake_word_to_pinyin(words[0], 0)]
    return words, ";".join(pinyin_words)


def validate_xiaozhi_config(payload: Any) -> dict[str, Any]:
    if payload is None:
        payload = {}
    if not isinstance(payload, dict):
        raise ValueError("xiaozhi config must be an object")
    defaults = default_xiaozhi_config()

    def bounded(value: Any, field: str, maximum: int, fallback: str = "") -> str:
        if value is None:
            return fallback
        if not isinstance(value, str):
            raise ValueError(f"xiaozhi.{field} must be a string")
        return value.strip()[:maximum]

    enabled = payload.get("enabled", defaults["enabled"])
    if not isinstance(enabled, bool):
        raise ValueError("xiaozhi.enabled must be a boolean")
    mcp_enabled = payload.get("mcp_enabled", defaults["mcp_enabled"])
    if not isinstance(mcp_enabled, bool):
        raise ValueError("xiaozhi.mcp_enabled must be a boolean")

    mcp_url = bounded(payload.get("mcp_url"), "mcp_url", 512, defaults["mcp_url"])
    if mcp_url:
        parsed_mcp = urlparse(mcp_url)
        if parsed_mcp.scheme not in {"ws", "wss"} or not parsed_mcp.hostname:
            raise ValueError("xiaozhi.mcp_url must be a ws(s):// URL")

    server_url = bounded(payload.get("server_url"), "server_url", 192,
                         defaults["server_url"]) or defaults["server_url"]
    parsed = urlparse(server_url)
    if parsed.scheme not in {"http", "https", "ws", "wss"} or not parsed.hostname:
        raise ValueError("xiaozhi.server_url must be an http(s):// or ws(s):// URL")

    wake_words, wake_word = _validate_wake_words(payload, defaults)
    return {
        "enabled": enabled,
        "server_url": server_url,
        "wake_word": wake_word,
        "wake_words": wake_words,
        "voice": bounded(payload.get("voice"), "voice", 32,
                         defaults["voice"]) or defaults["voice"],
        "mcp_enabled": mcp_enabled,
        "mcp_url": mcp_url,
    }


# The standalone Dotii page is gone. The firmware owns the live expression and
# only consumes this static mapping, so the identifiers below are the whole
# device contract.
DOTII_EXPRESSION_IDS = (
    "idle_breath", "blink", "curious", "sleepy_yawn",
    "touch_response", "connecting", "working", "complete", "failure",
)
DOTII_FIXED_STATE_DEFINITIONS = (
    {"id": "idle", "label": "空闲", "expression": "idle_breath"},
    {"id": "blink", "label": "待机眨眼", "expression": "blink"},
    {"id": "long_idle", "label": "长时间无操作", "expression": "sleepy_yawn"},
    {"id": "touch", "label": "触摸屏幕", "expression": "touch_response"},
    {"id": "connecting", "label": "连接中", "expression": "connecting"},
)
DOTII_DEFAULT_BUSINESS_ASSIGNMENTS = {
    "curious": ("codex_waiting_user",),
    "working": ("codex_working",),
    "complete": ("codex_completed",),
    "failure": ("codex_failure",),
}
DOTII_LOOP_DURATIONS_MS = {
    expression_id: 1200 if expression_id == "touch_response" else 800
    for expression_id in DOTII_EXPRESSION_IDS
}
DISPLAY_DEFAULTS = {
    "revision": 0,
    "codex_ui": "classic",
    "docked_rotation_tenths": 900,
    "screen_off_timeout_seconds": 60,
    "sleep_timeout_seconds": 300,
    "charging_screen_off_timeout_seconds": 60,
    # Docked Dotii keeps its network link (and Codex updates) while the panel
    # still turns off to avoid AMOLED burn-in.
    "charging_sleep_timeout_seconds": 0,
    "screen_off_page": "none",
}
DISPLAY_TIMEOUT_OPTIONS = {0, 10, 30, 60, 180, 300, 600, 1800}
CODEX_UI_OPTIONS = {"classic", "dual_limit"}
SCREEN_OFF_PAGE_OPTIONS = {"none"}
LEGACY_SCREEN_OFF_PAGE_OPTIONS = {"custom", "dotii"}
WEB_ROOT = resource_path("web")
MANAGEMENT_CENTER_EXECUTABLE = (
    "DotiiManagementCenter.exe" if os.name == "nt" else "DotiiManagementCenter"
)
CODEX_CLI_PACKAGE = os.environ.get("STATE_DISPLAY_CODEX_PACKAGE", "@openai/codex@0.151.0")
CODEX_CLI_VERSION = "0.151.0"


def _bounded_text(value: Any, maximum: int, field: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be text")
    return value.encode("utf-8")[:maximum].decode("utf-8", errors="ignore")


def startup_command(python_executable: Path, app_script: Path) -> str:
    return current_platform().startup_command(python_executable, app_script)


def packaged_startup_command(management_center: Path) -> str:
    """Build the login command for the tray host, never the bridge backend."""
    return current_platform().packaged_startup_command(management_center)


def current_startup_command() -> str:
    return current_platform().current_startup_command(
        frozen=is_frozen(),
        executable=Path(sys.executable),
        app_script=application_root() / "bridge" / "bridge_app.py",
        management_center=sibling_executable(MANAGEMENT_CENTER_EXECUTABLE),
    )


def startup_enabled() -> bool:
    return current_platform().startup_enabled()


def set_startup(enabled: bool) -> bool:
    command = current_startup_command() if enabled else ""
    return current_platform().set_startup(enabled, command)


def watch_parent(parent_pid: int, server: ThreadingHTTPServer, stop: threading.Event) -> None:
    """Stop a packaged backend if its native host disappears unexpectedly."""
    while not stop.wait(2.0):
        try:
            os.kill(parent_pid, 0)
        except ProcessLookupError:
            logging.warning("macOS 菜单栏宿主已退出，正在停止后台服务")
            server.shutdown()
            return
        except PermissionError:
            continue


def load_or_create_token(path: Path) -> str:
    try:
        token = path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeDecodeError):
        token = ""
    if token:
        return token
    token = secrets.token_urlsafe(32)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(token, encoding="utf-8")
    return token


def _is_loopback(address: str) -> bool:
    try:
        return ipaddress.ip_address(address).is_loopback
    except ValueError:
        return False


def local_ipv4() -> str:
    """Return the LAN address used in the device connection hint."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("10.255.255.255", 1))
        address = sock.getsockname()[0]
        return address if address and not _is_loopback(address) else "127.0.0.1"
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def configure_bluetooth(server: Any, payload: dict[str, Any]) -> bool:
    if not isinstance(payload, dict):
        raise ValueError("配置请求无效")
    device_id = server.bluetooth.device_status.get("device_id")
    if not isinstance(device_id, str) or not device_id:
        raise ValueError("未读取到 device_id，请重新扫描 Dotii")
    pairing = claim_codex_bridge_token(device_id)
    address = local_ipv4()
    return server.bluetooth.start_configure(
        address=payload.get("address"),
        ssid=payload.get("ssid"),
        password=payload.get("password"),
        bridge_url=f"http://{address}:{server.server_port}/api/v1/snapshot",
        bridge_token=server.token,
        codex_bridge_token=pairing.token,
        current_token=payload.get("current_token", ""),
    )


def _unbounded_text(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be text")
    return value


def _non_negative_int(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a number")
    result = int(value)
    if result < 0:
        raise ValueError(f"{field} must be non-negative")
    return result


def validate_module_config(payload: Any) -> dict[str, bool]:
    if payload is None:
        return dict(MODULE_DEFAULTS)
    if not isinstance(payload, dict):
        raise ValueError("modules must be an object")
    result: dict[str, bool] = {}
    for module_id, default in MODULE_DEFAULTS.items():
        enabled = payload.get(module_id, default)
        if not isinstance(enabled, bool):
            raise ValueError(f"modules.{module_id} must be a boolean")
        result[module_id] = enabled
    return result


def default_dotii_config() -> dict[str, Any]:
    """Static expression mapping published to the device.

    The management center no longer edits this. The firmware resolves the live
    expression from CodexBridge state, the xiaozhi session and touch input, and
    only needs the mapping itself.
    """
    return {
        "revision": 0,
        "character": "auto",
        "animations": {
            expression_id: {
                "states": list(DOTII_DEFAULT_BUSINESS_ASSIGNMENTS.get(expression_id, ())),
                "state_duration_ms": 0,
            }
            for expression_id in DOTII_EXPRESSION_IDS
        },
        "fixed_states": [
            {
                **definition,
                "duration_ms": DOTII_LOOP_DURATIONS_MS[definition["expression"]],
            }
            for definition in DOTII_FIXED_STATE_DEFINITIONS
        ],
    }


def _validate_timeout_pair(payload: dict[str, Any], screen_off_key: str, sleep_key: str,
                           fallback_screen_off: int, fallback_sleep: int) -> tuple[int, int]:
    if screen_off_key not in payload and sleep_key not in payload:
        return fallback_screen_off, fallback_sleep
    sleep_timeout = payload.get(sleep_key, fallback_sleep)
    if isinstance(sleep_timeout, bool) or not isinstance(sleep_timeout, int):
        raise ValueError(f"display.{sleep_key} must be an integer")
    if sleep_timeout == 5:
        sleep_timeout = 10
    if sleep_timeout not in DISPLAY_TIMEOUT_OPTIONS:
        raise ValueError(f"display.{sleep_key} must be one of 0, 10, 30, 60, 180, 300, 600, 1800")
    screen_off_timeout = payload.get(screen_off_key)
    if screen_off_timeout is None:
        if sleep_timeout == 0:
            screen_off_timeout = fallback_screen_off
        else:
            earlier_options = [value for value in DISPLAY_TIMEOUT_OPTIONS if 0 < value <= sleep_timeout]
            screen_off_timeout = max(earlier_options) if earlier_options else 10
            if not earlier_options:
                sleep_timeout = 10
    if isinstance(screen_off_timeout, bool) or not isinstance(screen_off_timeout, int):
        raise ValueError(f"display.{screen_off_key} must be an integer")
    if screen_off_timeout == 5:
        screen_off_timeout = 10
    if screen_off_timeout not in DISPLAY_TIMEOUT_OPTIONS:
        raise ValueError(f"display.{screen_off_key} must be one of 0, 10, 30, 60, 180, 300, 600, 1800")
    if screen_off_timeout == 0 and sleep_timeout != 0:
        raise ValueError(f"display.{sleep_key} must be never when {screen_off_key} is never")
    if screen_off_timeout != 0 and sleep_timeout != 0 and sleep_timeout < screen_off_timeout:
        raise ValueError(f"display.{sleep_key} must be greater than or equal to {screen_off_key}")
    return screen_off_timeout, sleep_timeout


def validate_display_config(payload: Any) -> dict[str, Any]:
    if payload is None:
        return dict(DISPLAY_DEFAULTS)
    if not isinstance(payload, dict):
        raise ValueError("display must be an object")
    codex_ui = payload.get("codex_ui", DISPLAY_DEFAULTS["codex_ui"])
    if codex_ui not in CODEX_UI_OPTIONS:
        raise ValueError("display.codex_ui must be classic or dual_limit")
    angle = payload.get("docked_rotation_tenths", DISPLAY_DEFAULTS["docked_rotation_tenths"])
    if isinstance(angle, bool) or not isinstance(angle, int) or not 800 <= angle <= 1000:
        raise ValueError("display.docked_rotation_tenths must be an integer between 800 and 1000")
    # 84.0 deg was the pre-1.1.5 default. Config files written by older
    # management centers still carry it and would re-enable the blurry
    # bilinear rotation, so migrate that single legacy value on load.
    if angle == 840:
        angle = DISPLAY_DEFAULTS["docked_rotation_tenths"]
    screen_off_page = payload.get("screen_off_page", DISPLAY_DEFAULTS["screen_off_page"])
    if screen_off_page in LEGACY_SCREEN_OFF_PAGE_OPTIONS:
        # The standalone custom and Dotii pages were removed from the firmware.
        # Migrate old profiles to a normal black screen instead of refusing to
        # load the entire display configuration.
        screen_off_page = "none"
    if screen_off_page not in SCREEN_OFF_PAGE_OPTIONS:
        raise ValueError("display.screen_off_page must be none")
    screen_off_timeout, sleep_timeout = _validate_timeout_pair(
        payload, "screen_off_timeout_seconds", "sleep_timeout_seconds",
        DISPLAY_DEFAULTS["screen_off_timeout_seconds"], DISPLAY_DEFAULTS["sleep_timeout_seconds"],
    )
    charging_screen_off_timeout, charging_sleep_timeout = _validate_timeout_pair(
        payload, "charging_screen_off_timeout_seconds", "charging_sleep_timeout_seconds",
        screen_off_timeout, sleep_timeout,
    )
    return {
        "revision": _non_negative_int(payload.get("revision", 0), "display.revision"),
        "codex_ui": codex_ui,
        "docked_rotation_tenths": angle,
        "screen_off_timeout_seconds": screen_off_timeout,
        "sleep_timeout_seconds": sleep_timeout,
        "charging_screen_off_timeout_seconds": charging_screen_off_timeout,
        "charging_sleep_timeout_seconds": charging_sleep_timeout,
        "screen_off_page": screen_off_page,
    }


def validate_snapshot(payload: Any) -> dict[str, Any]:
    """Validate the device-facing management envelope.

    Codex data is owned by CodexBridge and travels over the device protocol,
    so this snapshot intentionally carries only display, xiaozhi and the static
    Dotii expression mapping.
    """
    if not isinstance(payload, dict) or payload.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("schema_version must be 1")
    if "bambu" in payload or "bambu" in (payload.get("modules") or {}):
        raise ValueError("bambu is no longer supported")
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_at_epoch": _non_negative_int(
            payload.get("generated_at_epoch", int(time.time())), "generated_at_epoch"
        ),
        "preview_data": bool(payload.get("preview_data", False)),
        "source": _bounded_text(payload.get("source", "manual"), 32, "source"),
    }


class StateStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.lock = threading.RLock()

    def read(self) -> dict[str, Any]:
        with self.lock:
            return validate_snapshot(json.loads(self.path.read_text(encoding="utf-8")))

    def write(self, snapshot: dict[str, Any]) -> None:
        with self.lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            handle, temporary_name = tempfile.mkstemp(prefix="state-", suffix=".json", dir=self.path.parent)
            try:
                with os.fdopen(handle, "w", encoding="utf-8") as stream:
                    json.dump(snapshot, stream, ensure_ascii=False, indent=2)
                    stream.write("\n")
                os.replace(temporary_name, self.path)
            finally:
                if os.path.exists(temporary_name):
                    os.unlink(temporary_name)


class ModuleConfigStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.lock = threading.RLock()

    def read(self) -> dict[str, bool]:
        with self.lock:
            if not self.path.is_file():
                return dict(MODULE_DEFAULTS)
            return validate_module_config(json.loads(self.path.read_text(encoding="utf-8")))

    def write(self, config: dict[str, Any]) -> dict[str, bool]:
        validated = validate_module_config(config)
        with self.lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            handle, temporary_name = tempfile.mkstemp(prefix="modules-", suffix=".json", dir=self.path.parent)
            try:
                with os.fdopen(handle, "w", encoding="utf-8") as stream:
                    json.dump(validated, stream, ensure_ascii=False, indent=2)
                    stream.write("\n")
                os.replace(temporary_name, self.path)
            finally:
                if os.path.exists(temporary_name):
                    os.unlink(temporary_name)
        return validated


class XiaozhiConfigStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.lock = threading.RLock()

    def read(self) -> dict[str, Any]:
        with self.lock:
            if not self.path.is_file():
                return default_xiaozhi_config()
            return validate_xiaozhi_config(json.loads(self.path.read_text(encoding="utf-8")))

    def write(self, config: dict[str, Any]) -> dict[str, Any]:
        validated = validate_xiaozhi_config(config)
        with self.lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            handle, temporary_name = tempfile.mkstemp(prefix="xiaozhi-", suffix=".json", dir=self.path.parent)
            try:
                with os.fdopen(handle, "w", encoding="utf-8") as stream:
                    json.dump(validated, stream, ensure_ascii=False, indent=2)
                    stream.write("\n")
                os.replace(temporary_name, self.path)
            finally:
                if os.path.exists(temporary_name):
                    os.unlink(temporary_name)
        return validated


class DisplayConfigStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.lock = threading.RLock()

    def read(self) -> dict[str, Any]:
        with self.lock:
            if not self.path.is_file():
                return dict(DISPLAY_DEFAULTS)
            return validate_display_config(json.loads(self.path.read_text(encoding="utf-8")))

    def write(self, config: dict[str, Any]) -> dict[str, Any]:
        validated = validate_display_config(config)
        with self.lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            handle, temporary_name = tempfile.mkstemp(prefix="display-", suffix=".json", dir=self.path.parent)
            try:
                with os.fdopen(handle, "w", encoding="utf-8") as stream:
                    json.dump(validated, stream, ensure_ascii=False, indent=2)
                    stream.write("\n")
                os.replace(temporary_name, self.path)
            finally:
                if os.path.exists(temporary_name):
                    os.unlink(temporary_name)
        return validated


class RuntimeStatus:
    """Non-Codex runtime metadata exposed in the management overview."""

    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.started_at_epoch = int(time.time())

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return {"started_at_epoch": self.started_at_epoch}


class BridgeHandler(BaseHTTPRequestHandler):
    server_version = "StateDisplayBridge/1.1"

    @property
    def bridge(self) -> "BridgeServer":
        return self.server  # type: ignore[return-value]

    def _authorized(self) -> bool:
        return secrets.compare_digest(self.headers.get("X-Bridge-Token", ""), self.bridge.token)

    def _local_admin(self) -> bool:
        return _is_loopback(self.client_address[0])

    def _send_bytes(self, status: int, body: bytes, content_type: str) -> None:
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header(
                "Content-Security-Policy",
                "default-src 'self'; img-src 'self' blob: data:; style-src 'self'; script-src 'self'; "
                "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'",
            )
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            logging.debug("客户端在响应写入完成前断开：%s", self.client_address[0])

    def _send_json(self, status: int, payload: Any) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send_bytes(status, body, "application/json; charset=utf-8")

    def _deny_nonlocal(self) -> bool:
        if self._local_admin():
            return False
        self._send_json(HTTPStatus.FORBIDDEN, {"error": "management interface is local only"})
        return True

    def _read_json(self) -> Any:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_BODY:
            raise ValueError("invalid request size")
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def _read_bytes(self, maximum: int, *, exact: int | None = None) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > maximum or (exact is not None and length != exact):
            raise ValueError("invalid request size")
        body = self.rfile.read(length)
        if len(body) != length:
            raise ValueError("incomplete request body")
        return body

    def _read_admin_action(self) -> dict[str, Any]:
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            raise ValueError("management actions require application/json")
        payload = self._read_json()
        if not isinstance(payload, dict):
            raise ValueError("request body must be an object")
        return payload

    def _serve_static(self, request_path: str) -> None:
        if self._deny_nonlocal():
            return
        relative = "index.html" if request_path == "/" else unquote(request_path).lstrip("/")
        target = (WEB_ROOT / relative).resolve()
        try:
            target.relative_to(WEB_ROOT.resolve())
        except ValueError:
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        if not target.is_file():
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        content_type = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        if content_type.startswith("text/") or content_type in {"application/javascript", "image/svg+xml"}:
            content_type += "; charset=utf-8"
        self._send_bytes(HTTPStatus.OK, target.read_bytes(), content_type)

    def _overview(self) -> dict[str, Any]:
        snapshot = self.bridge.snapshot()
        snapshot["qqmusic"] = self.bridge.qqmusic.status()
        address = local_ipv4()
        runtime = self.bridge.runtime.snapshot()
        return {
            "app": {"name": "Dotii 管理中心", "version": application_version()},
            "bridge": {
                "online": True,
                "local_url": f"http://127.0.0.1:{self.bridge.server_port}",
                "device_url": f"http://{address}:{self.bridge.server_port}/api/v1/snapshot",
                "token": self.bridge.token,
                "port": self.bridge.server_port,
                "auto_start": startup_enabled(),
                "startup_available": current_platform().startup_available,
                **runtime,
            },
            "snapshot": snapshot,
            "display_config": self.bridge.display.read(),
            "xiaozhi_config": self.bridge.xiaozhi.read(),
            "bluetooth": self.bridge.bluetooth.snapshot(),
            "firmware": self.bridge.firmware.snapshot(),
            "qqmusic": self.bridge.qqmusic.status(),
            "modules": [
                {"id": "xiaozhi", "name": "小智", "enabled": snapshot["modules"]["xiaozhi"], "available": True, "locked": False},
            ],
        }

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/health":
            self._send_json(HTTPStatus.OK, {
                "ok": True,
                "schema_version": SCHEMA_VERSION,
                "parent_pid": self.bridge.host_parent_pid,
            })
        elif path == "/api/v1/snapshot":
            if not self._authorized():
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid bridge token"})
                return
            try:
                self._send_json(HTTPStatus.OK, self.bridge.snapshot())
            except (OSError, ValueError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(error)})
        elif path == "/api/v1/device/music/stream":
            if not self._authorized():
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid bridge token"})
                return
            try:
                mid = str(parse_qs(urlparse(self.path).query).get("mid", [""])[0]).strip()
                if not mid or not mid.isalnum():
                    raise ValueError("invalid mid")
                resolved = self.bridge.qqmusic.resolve(mid)
                if not resolved.get("available"):
                    self._send_json(HTTPStatus.NOT_FOUND, {"error": resolved.get("reason", "unavailable")})
                    return
                upstream = requests.get(resolved["url"], stream=True, timeout=(10, 30))
                if upstream.status_code >= 400:
                    self._send_json(HTTPStatus.BAD_GATEWAY, {"error": "upstream error"})
                    return
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", upstream.headers.get("Content-Type", "audio/mp4"))
                length = upstream.headers.get("Content-Length")
                if length:
                    self.send_header("Content-Length", length)
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                # Windows buffers small socket writes; a music stream must not
                # sit in the sender's buffer, or the device hears bursts of
                # audio followed by silence.
                self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                for chunk in upstream.iter_content(chunk_size=32 * 1024):
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
                pass  # device stopped or seeked; nothing to clean up here
            except Exception as error:
                try:
                    self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
                except Exception:
                    pass
            return
        elif path == "/api/v1/admin/qqmusic/status":
            if self._deny_nonlocal():
                return
            self._send_json(HTTPStatus.OK, self.bridge.qqmusic.status())
        elif path == "/api/v1/admin/overview":
            if self._deny_nonlocal():
                return
            try:
                self._send_json(HTTPStatus.OK, self._overview())
            except (OSError, ValueError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(error)})
        else:
            self._serve_static(path)

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/api/v1/admin/bluetooth/install":
            if self._deny_nonlocal():
                return
            try:
                self._read_admin_action()
                started = self.bridge.bluetooth.start_install()
                status = HTTPStatus.ACCEPTED if started else HTTPStatus.CONFLICT
                self._send_json(status, {"ok": started, "bluetooth": self.bridge.bluetooth.snapshot()})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/bluetooth/scan":
            if self._deny_nonlocal():
                return
            try:
                self._read_admin_action()
                started = self.bridge.bluetooth.start_scan()
                status = HTTPStatus.ACCEPTED if started else HTTPStatus.CONFLICT
                self._send_json(status, {"ok": started})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/bluetooth/configure":
            if self._deny_nonlocal():
                return
            try:
                payload = self._read_admin_action()
                started = configure_bluetooth(self.bridge, payload)
                status = HTTPStatus.ACCEPTED if started else HTTPStatus.CONFLICT
                self._send_json(status, {"ok": started})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/firmware/refresh":
            if self._deny_nonlocal():
                return
            try:
                self._read_admin_action()
                self.bridge.firmware.refresh()
                self._send_json(HTTPStatus.OK, {"ok": True, "firmware": self.bridge.firmware.snapshot()})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/firmware/flash":
            if self._deny_nonlocal():
                return
            try:
                payload = self._read_admin_action()
                if payload.get("confirmation") != "FLASH_DOTII":
                    raise ValueError("需要确认后才能烧录")
                started = self.bridge.firmware.start_flash(payload.get("port"))
                if not started:
                    self._send_json(HTTPStatus.CONFLICT, {"error": "已有烧录任务正在运行"})
                else:
                    self._send_json(HTTPStatus.ACCEPTED, {"ok": True})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/qqmusic/browser-login":
            if self._deny_nonlocal():
                return
            try:
                self._read_admin_action()
                import time as _time
                started = getattr(self.bridge.qqmusic, "_login_started", 0)
                if getattr(self.bridge.qqmusic, "_login_running", False) and \
                   _time.time() - started <= 300:
                    self._send_json(HTTPStatus.CONFLICT, {"error": "login already running"})
                    return
                self.bridge.qqmusic._login_running = True

                def worker():
                    try:
                        self.bridge.qqmusic.browser_login()
                    finally:
                        self.bridge.qqmusic._login_running = False

                import threading as _threading
                _threading.Thread(target=worker, daemon=True).start()
                self._send_json(HTTPStatus.ACCEPTED, {"ok": True, "state": "opening"})
            except Exception as error:
                self.bridge.qqmusic._login_running = False
                self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
            return
        if path == "/api/v1/admin/qqmusic/qr":
            if self._deny_nonlocal():
                return
            try:
                self._read_admin_action()
                self._send_json(HTTPStatus.OK, self.bridge.qqmusic.qr_begin())
            except Exception as error:
                self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
            return
        if path == "/api/v1/admin/qqmusic/poll":
            if self._deny_nonlocal():
                return
            try:
                self._read_admin_action()
                self._send_json(HTTPStatus.OK, self.bridge.qqmusic.qr_poll())
            except Exception as error:
                self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
            return
        if path == "/api/v1/admin/qqmusic/logout":
            if self._deny_nonlocal():
                return
            try:
                self._read_admin_action()
                self.bridge.qqmusic.logout()
                self._send_json(HTTPStatus.OK, {"ok": True, **self.bridge.qqmusic.status()})
            except Exception as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/device/music/search":
            if not self._authorized():
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid bridge token"})
                return
            try:
                payload = self._read_admin_action()
                query = str(payload.get("q", "")).strip()
                if not query:
                    raise ValueError("missing query")
                results = self.bridge.qqmusic.search(query, int(payload.get("limit", 3)))
                self._send_json(HTTPStatus.OK, {"ok": True, "results": results})
            except QQMusicNotLoggedIn as error:
                # The device relays this text to the user, so distinguish "the
                # QQ login needs refreshing" from a genuine upstream failure.
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": str(error)})
            except Exception as error:
                self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
            return
        if path == "/api/v1/device/music/playlist":
            if not self._authorized():
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid bridge token"})
                return
            try:
                payload = self._read_admin_action()
                playlist = str(payload.get("playlist", "")).strip()
                limit = int(payload.get("limit", 5))
                offset = int(payload.get("offset", 0))
                result = self.bridge.qqmusic.playlist_songs(playlist, limit, offset)
                self._send_json(HTTPStatus.OK, {"ok": True, **result})
            except QQMusicNotLoggedIn as error:
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": str(error)})
            except Exception as error:
                self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
            return
        if path == "/api/v1/device/music/resolve":
            if not self._authorized():
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid bridge token"})
                return
            try:
                payload = self._read_admin_action()
                mid = str(payload.get("mid", "")).strip()
                if not mid:
                    raise ValueError("missing mid")
                self._send_json(HTTPStatus.OK, self.bridge.qqmusic.resolve(mid))
            except Exception as error:
                self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
            return
        if path == "/api/v1/admin/xiaozhi":
            if self._deny_nonlocal():
                return
            try:
                payload = self._read_admin_action()
                current = self.bridge.xiaozhi.read()
                candidate = {**current, **payload}
                if payload.get("enabled") is True:
                    candidate["enabled"] = True
                config = self.bridge.xiaozhi.write(candidate)
                self._send_json(HTTPStatus.OK, {"ok": True, "xiaozhi_config": config})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/modules":
            if self._deny_nonlocal():
                return
            try:
                payload = self._read_admin_action()
                module_id = payload.get("id")
                enabled = payload.get("enabled")
                if module_id not in MODULE_DEFAULTS or not isinstance(enabled, bool):
                    raise ValueError("id must be xiaozhi and enabled must be boolean")
                modules = self.bridge.modules.read()
                candidate = {**modules, module_id: enabled}
                saved = self.bridge.modules.write(candidate)
                if module_id == "xiaozhi":
                    xiaozhi = self.bridge.xiaozhi.read()
                    self.bridge.xiaozhi.write({**xiaozhi, "enabled": enabled})
                self._send_json(HTTPStatus.OK, {"ok": True, "modules": saved})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/display":
            if self._deny_nonlocal():
                return
            try:
                payload = self._read_admin_action()
                current = self.bridge.display.read()
                candidate = {**current, **payload, "revision": current["revision"] + 1}
                display = self.bridge.display.write(candidate)
                self._send_json(HTTPStatus.OK, {"ok": True, "display_config": display})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path == "/api/v1/admin/settings":
            if self._deny_nonlocal():
                return
            try:
                payload = self._read_json()
                if not isinstance(payload, dict) or not isinstance(payload.get("auto_start"), bool):
                    raise ValueError("auto_start must be a boolean")
                enabled = set_startup(payload["auto_start"])
                self._send_json(HTTPStatus.OK, {"ok": True, "auto_start": enabled})
            except (OSError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if path != "/api/v1/snapshot":
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        if not self._authorized():
            self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid bridge token"})
            return
        try:
            snapshot = validate_snapshot(self._read_json())
            self.bridge.store.write(snapshot)
            self._send_json(HTTPStatus.OK, {"ok": True, "generated_at_epoch": snapshot["generated_at_epoch"]})
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})

    def log_message(self, format_string: str, *args: Any) -> None:
        logging.info("%s %s", self.client_address[0], format_string % args)


class BridgeServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(
        self,
        address: tuple[str, int],
        store: StateStore,
        token: str,
        runtime: RuntimeStatus,
        modules: ModuleConfigStore,
        display: DisplayConfigStore,
        xiaozhi: XiaozhiConfigStore,
        qqmusic: QQMusicAccount,
        bluetooth: BluetoothBridge,
        firmware: FirmwareFlasher,
        runtime_folder: Path,
        host_parent_pid: int = 0,
    ) -> None:
        super().__init__(address, BridgeHandler)
        self.store = store
        self.token = token
        self.runtime = runtime
        self.modules = modules
        self.display = display
        self.xiaozhi = xiaozhi
        self.qqmusic = qqmusic
        self.bluetooth = bluetooth
        self.firmware = firmware
        self.runtime_folder = runtime_folder
        self.host_parent_pid = host_parent_pid

    def snapshot(self) -> dict[str, Any]:
        snapshot = self.store.read()
        snapshot["modules"] = self.modules.read()
        dotii_config = default_dotii_config()
        snapshot["dotii"] = dotii_config
        snapshot["character"] = dotii_config["character"]
        snapshot["display"] = self.display.read()
        snapshot["xiaozhi"] = self.xiaozhi.read()
        qq = self.qqmusic.status()
        # The device stores this cookie bundle so it can search and stream on
        # its own without the management center running.
        qq["cookies"] = self.qqmusic.cookie_header()
        snapshot["qqmusic"] = qq
        return snapshot


def main() -> None:
    parser = argparse.ArgumentParser(description="Dotii 管理中心")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8787, type=int)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--token", default=os.environ.get("STATE_DISPLAY_BRIDGE_TOKEN"))
    parser.add_argument("--parent-pid", type=int, default=0, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    writable_root = runtime_folder()
    writable_root.mkdir(parents=True, exist_ok=True)
    state_path = arguments.state or writable_root / "state.json"
    if not state_path.exists():
        shutil.copyfile(resource_path("state.json"), state_path)

    token = arguments.token or load_or_create_token(writable_root / "bridge-token")
    modules = ModuleConfigStore(writable_root / "modules.json")
    bluetooth = BluetoothBridge(writable_root)
    firmware = FirmwareFlasher(project_root(), writable_root)
    server = BridgeServer(
        (arguments.host, arguments.port),
        StateStore(state_path),
        token,
        RuntimeStatus(),
        modules,
        DisplayConfigStore(writable_root / "display.json"),
        XiaozhiConfigStore(writable_root / "xiaozhi.json"),
        QQMusicAccount(writable_root),
        bluetooth,
        firmware,
        writable_root,
        arguments.parent_pid,
    )
    bluetooth.start()
    stop = threading.Event()
    if arguments.parent_pid > 1:
        threading.Thread(
            target=watch_parent,
            args=(arguments.parent_pid, server, stop),
            name="dotii-parent-watchdog",
            daemon=True,
        ).start()

    address = local_ipv4()
    logging.info("Dotii 管理中心：http://127.0.0.1:%s", server.server_port)
    logging.info("ESP32 地址：http://%s:%s/api/v1/snapshot", address, server.server_port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.bluetooth.stop()
        server.firmware.stop()
        server.server_close()


if __name__ == "__main__":
    if is_frozen() and len(sys.argv) > 1 and sys.argv[1] == "--esptool":
        import esptool

        sys.argv = ["esptool", *sys.argv[2:]]
        raise SystemExit(esptool.main())
    main()
