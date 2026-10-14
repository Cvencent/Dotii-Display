"""Locate application resources in source and packaged builds."""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

from platforms import current_platform


_CMAKE_VERSION_PATTERN = re.compile(r'set\s*\(\s*PROJECT_VER\s+"?([^"\s\)]+)')


def is_frozen() -> bool:
    """Return whether the bridge is running from a PyInstaller executable."""
    return bool(getattr(sys, "frozen", False))


def macos_app_contents(executable: Path | None = None) -> Path | None:
    """Return the enclosing ``.app/Contents`` directory when one exists."""
    candidate = (executable or Path(sys.executable)).resolve()
    for parent in candidate.parents:
        if parent.name != "Contents":
            continue
        if parent.parent.suffix.lower() == ".app":
            return parent
    return None


def application_resources_root() -> Path:
    """Return persistent read-only resources shipped with the application."""
    if is_frozen() and sys.platform == "darwin":
        contents = macos_app_contents()
        if contents is not None:
            return contents / "Resources"
    if is_frozen():
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


def auxiliary_executables_root() -> Path:
    """Return the directory containing packaged helper executables."""
    if is_frozen() and sys.platform == "darwin":
        contents = macos_app_contents()
        if contents is not None:
            return contents / "MacOS"
    return Path(sys.executable).resolve().parent if is_frozen() else Path(__file__).resolve().parent


def bundle_root() -> Path:
    """Return the read-only root containing bundled data files."""
    if is_frozen():
        extracted = getattr(sys, "_MEIPASS", None)
        if extracted:
            return Path(extracted).resolve()
        return application_resources_root()
    return Path(__file__).resolve().parent


def application_root() -> Path:
    """Return the directory used as the process working root."""
    if is_frozen():
        return application_resources_root()
    return Path(__file__).resolve().parent.parent


def tools_root() -> Path:
    """Return the optional application-private external tools directory."""
    return application_root() / "tools"


def runtime_folder() -> Path:
    """Return the active platform's writable application-data directory."""
    return current_platform().runtime_folder()


def project_root() -> Path:
    """Return the source or packaged root containing firmware build files."""
    return bundle_root() if is_frozen() else application_root()


def resource_path(*parts: str) -> Path:
    return bundle_root().joinpath(*parts)


def sibling_executable(name: str) -> Path:
    """Return a packaged sibling executable, or its source-tree equivalent."""
    if is_frozen():
        return auxiliary_executables_root() / name
    return bundle_root() / name


def _version_from_cmake() -> str | None:
    """Read the project version from source or packaged CMake metadata."""
    candidates = (
        application_root() / "CMakeLists.txt",
        bundle_root() / "CMakeLists.txt",
    )
    for candidate in candidates:
        try:
            text = candidate.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        match = _CMAKE_VERSION_PATTERN.search(text)
        if match:
            return match.group(1)
    return None


def _version_from_windows_executable(executable: Path) -> str | None:
    """Read the numeric file version embedded in a Windows executable."""
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        class VS_FIXEDFILEINFO(ctypes.Structure):
            _fields_ = [
                ("dwSignature", wintypes.DWORD),
                ("dwStrucVersion", wintypes.DWORD),
                ("dwFileVersionMS", wintypes.DWORD),
                ("dwFileVersionLS", wintypes.DWORD),
            ]

        version = ctypes.WinDLL("version", use_last_error=True)
        version.GetFileVersionInfoSizeW.argtypes = [
            wintypes.LPCWSTR,
            ctypes.POINTER(wintypes.DWORD),
        ]
        version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
        version.GetFileVersionInfoW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_void_p,
        ]
        version.GetFileVersionInfoW.restype = wintypes.BOOL
        version.VerQueryValueW.argtypes = [
            ctypes.c_void_p,
            wintypes.LPCWSTR,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(wintypes.UINT),
        ]
        version.VerQueryValueW.restype = wintypes.BOOL

        path = str(executable.resolve())
        size = version.GetFileVersionInfoSizeW(path, None)
        if not size:
            return None
        buffer = ctypes.create_string_buffer(size)
        if not version.GetFileVersionInfoW(path, 0, size, buffer):
            return None
        pointer = ctypes.c_void_p()
        length = wintypes.UINT()
        if not version.VerQueryValueW(
            buffer,
            "\\",
            ctypes.byref(pointer),
            ctypes.byref(length),
        ):
            return None
        fixed = ctypes.cast(pointer, ctypes.POINTER(VS_FIXEDFILEINFO)).contents
        if fixed.dwSignature != 0xFEEF04BD:
            return None
        major = fixed.dwFileVersionMS >> 16
        minor = fixed.dwFileVersionMS & 0xFFFF
        patch = fixed.dwFileVersionLS >> 16
        return f"{major}.{minor}.{patch}"
    except (AttributeError, OSError, ValueError):
        return None


def application_version() -> str:
    """Return the management-center version shown by the tray host."""
    if is_frozen():
        executable_version = _version_from_windows_executable(Path(sys.executable))
        if executable_version:
            return executable_version
    return _version_from_cmake() or "未知"
