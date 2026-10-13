"""Windows implementation of the Dotii management-center platform boundary."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

from .base import RunCommand


DOTII_USB_VID = "303A"
DOTII_USB_PID = "1001"
PORT_PATTERN = re.compile(r"^COM(?:[1-9]|[1-9][0-9]|[12][0-9]{2})$", re.IGNORECASE)
STARTUP_REGISTRY_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
STARTUP_REGISTRY_VALUE = "DotiiManagementCenter"


def parse_serial_ports(payload: str) -> list[dict[str, object]]:
    """Normalize a Win32_SerialPort JSON result without exposing CIM upstream."""
    try:
        raw = json.loads(payload or "[]")
    except json.JSONDecodeError:
        return []
    if isinstance(raw, dict):
        raw = [raw]
    output: list[dict[str, object]] = []
    for item in raw if isinstance(raw, list) else []:
        if not isinstance(item, dict):
            continue
        port = str(item.get("DeviceID") or "").upper()
        pnp = str(item.get("PNPDeviceID") or "").upper()
        if not PORT_PATTERN.fullmatch(port):
            continue
        output.append({
            "port": port,
            "name": str(item.get("Name") or port)[:120],
            "pnp_id": pnp[:180],
            "dotii": f"VID_{DOTII_USB_VID}" in pnp and f"PID_{DOTII_USB_PID}" in pnp,
        })
    output.sort(key=lambda item: (not bool(item["dotii"]), int(str(item["port"])[3:])))
    return output


class WindowsPlatformAdapter:
    name = "windows"
    startup_available = True
    bluetooth_available = True
    bluetooth_pair_on_connect = True
    serial_flash_available = True

    def runtime_folder(self) -> Path:
        local_app_data = os.environ.get("LOCALAPPDATA")
        return (Path(local_app_data) if local_app_data else Path.home() / ".state-display") / "StateDisplay"

    def hidden_creation_flags(self) -> int:
        return getattr(subprocess, "CREATE_NO_WINDOW", 0)

    async def prepare_bluetooth_pairing(self, address: str) -> None:
        await pair_bluetooth_device(address)

    def startup_command(self, python_executable: Path, app_script: Path) -> str:
        return f'"{python_executable.resolve()}" -B "{app_script.resolve()}" --startup'

    def packaged_startup_command(self, management_center: Path) -> str:
        return f'"{management_center.resolve()}" --startup'

    def current_startup_command(
        self,
        *,
        frozen: bool,
        executable: Path,
        app_script: Path,
        management_center: Path,
    ) -> str:
        if frozen:
            return self.packaged_startup_command(management_center)
        return self.startup_command(executable.with_name("pythonw.exe"), app_script)

    def startup_enabled(self) -> bool:
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, STARTUP_REGISTRY_KEY) as key:
                winreg.QueryValueEx(key, STARTUP_REGISTRY_VALUE)
                return True
        except (FileNotFoundError, OSError):
            return False

    def set_startup(self, enabled: bool, command: str) -> bool:
        import winreg

        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, STARTUP_REGISTRY_KEY) as key:
            if enabled:
                winreg.SetValueEx(key, STARTUP_REGISTRY_VALUE, 0, winreg.REG_SZ, command)
            else:
                try:
                    winreg.DeleteValue(key, STARTUP_REGISTRY_VALUE)
                except FileNotFoundError:
                    pass
        return self.startup_enabled()

    def codex_cli_candidates(self, runtime_folder: Path, application_root: Path) -> list[Path]:
        return [
            runtime_folder / "codex-cli" / "node_modules" / "@openai" / "codex" / "bin" / "codex.js",
            application_root / "tools" / "codex-cli" / "node_modules" / "@openai" / "codex" / "bin" / "codex.js",
            Path(os.environ.get("APPDATA", "")) / "npm" / "node_modules" / "@openai" / "codex" / "bin" / "codex.js",
        ]

    def bundled_node_candidates(self, application_root: Path) -> list[Path]:
        return [application_root / "tools" / "node" / "node.exe"]

    def esptool_python_candidates(self) -> list[Path]:
        output = [Path(sys.executable)]
        configured = os.environ.get("IDF_PYTHON_ENV_PATH")
        if configured:
            output.append(Path(configured) / "Scripts" / "python.exe")
        configured_tools = os.environ.get("IDF_TOOLS_PATH")
        if configured_tools:
            tools_python = Path(configured_tools) / "python"
            if tools_python.is_dir():
                output.extend(sorted(tools_python.glob("v*/venv/Scripts/python.exe"), reverse=True))
        search_roots = [
            Path.home() / ".espressif" / "python_env",
            Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Espressif" / "python_env",
            Path(os.environ.get("ProgramData", "")) / "Espressif" / "python_env",
        ]
        system_drive = os.environ.get("SystemDrive")
        if system_drive:
            search_roots.append(Path(f"{system_drive}\\Espressif") / "tools" / "python")
        for root in search_roots:
            if not root.is_dir():
                continue
            output.extend(sorted(root.glob("*/venv/Scripts/python.exe"), reverse=True))
            output.extend(sorted(root.glob("*/Scripts/python.exe"), reverse=True))
        unique: list[Path] = []
        for candidate in output:
            resolved = candidate.resolve() if candidate.exists() else candidate
            if resolved not in unique:
                unique.append(resolved)
        return unique

    def scan_serial_ports(self, run: RunCommand) -> list[dict[str, object]]:
        command = (
            "Get-CimInstance Win32_SerialPort | "
            "Select-Object DeviceID,Name,PNPDeviceID | ConvertTo-Json -Compress"
        )
        try:
            powershell = shutil.which("powershell.exe") or shutil.which("pwsh.exe") or "powershell.exe"
            result = run(
                [powershell, "-NoProfile", "-NonInteractive", "-Command", command],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                encoding="utf-8-sig",
                errors="replace",
                timeout=8,
                creationflags=self.hidden_creation_flags(),
                check=False,
            )
            return parse_serial_ports(result.stdout) if result.returncode == 0 else []
        except (OSError, subprocess.SubprocessError):
            return []

    def valid_serial_port(self, port: str) -> bool:
        return bool(PORT_PATTERN.fullmatch(port.upper()))


async def pair_bluetooth_device(address: str) -> None:
    """Pair a Dotii over BLE at the *encryption* protection level.

    Windows asks for encryption-and-authentication by default.  Dotii pairs
    with Just Works (no PIN), which cannot satisfy that request, and the
    failed attempt leaves the OS pairing unusable on the next connect.  A
    bond created explicitly with the encryption level is enough to reach the
    encrypted configuration characteristic.
    """
    from winrt.windows.devices.bluetooth import BluetoothLEDevice
    from winrt.windows.devices.enumeration import (
        DevicePairingKinds,
        DevicePairingProtectionLevel,
        DevicePairingResultStatus,
    )

    digits = address.replace(":", "").replace("-", "").strip()
    if len(digits) != 12:
        raise ValueError("蓝牙地址无效，请重新扫描 Dotii")
    device = await BluetoothLEDevice.from_bluetooth_address_async(int(digits, 16))
    if device is None:
        raise OSError("未找到蓝牙设备，请确认 Dotii 正在广播")
    pairing = device.device_information.pairing
    if pairing.is_paired:
        return
    if not pairing.can_pair:
        raise OSError("系统不支持与该设备配对，请在系统设置中移除旧的 Dotii 配对后重试")
    custom = pairing.custom

    def accept(sender: Any, args: Any) -> None:
        args.accept()

    token = custom.add_pairing_requested(accept)
    try:
        result = await custom.pair_with_protection_level_async(
            DevicePairingKinds.CONFIRM_ONLY,
            DevicePairingProtectionLevel.ENCRYPTION,
        )
    finally:
        custom.remove_pairing_requested(token)
    if result.status not in (
        DevicePairingResultStatus.PAIRED,
        DevicePairingResultStatus.ALREADY_PAIRED,
    ):
        raise OSError(f"蓝牙配对未完成（{result.status}），请在系统设置中移除旧的 Dotii 配对后重试")