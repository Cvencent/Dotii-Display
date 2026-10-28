# -*- coding: utf-8 -*-
"""QQ Music account bridge: QR login, search and stream URL resolution.

Uses the public web endpoints of y.qq.com with the *user's own* cookies.
Permission follows the account exactly: VIP songs resolve only for a VIP
account, otherwise the resolver reports "unavailable" and the device shows
nothing. No DRM circumvention, no key extraction, no cached stream URLs.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import random
import re
import threading
import time
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

import requests

class QQMusicNotLoggedIn(RuntimeError):
    """Raised when QQ answers a search with its "not logged in" code.

    The device surfaces this as "please sign in again" instead of the far more
    confusing "no such song".
    """


USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)
QR_APPID = "716027609"
QR_PT_AID = "100497308"
SEARCH_URL = "https://u.y.qq.com/cgi-bin/musicu.fcg"
STREAM_URL = "https://u.y.qq.com/cgi-bin/musicu.fcg"


def _hash33(value: str) -> int:
    result = 0
    for char in value:
        result = (result << 5) + result + ord(char)
    return result & 0x7FFFFFFF


class QQMusicAccount:
    """Holds the user's QQ Music session and answers device-side queries."""

    def __init__(self, runtime_folder: Path) -> None:
        self.path = runtime_folder / "qqmusic-session.json"
        self.lock = threading.RLock()
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": USER_AGENT})
        self.pending_qrsig = ""
        self.pending_created = 0.0
        self.login_check_url = ""
        self.cookies: dict[str, str] = {}
        self.uin = ""
        self.nickname = ""
        self.logged_in = False
        self.vip = False
        self.vip_level = 0
        self.vip_super = False
        self.vip_year = False
        self.vip_expires = ""
        # None until this process has confirmed the entitlement against the
        # server; a session file written by an older build carries a vip flag
        # the broken probe always set to False.
        self._vip_checked_at = 0.0
        self._load()

    # ---- persistence ----
    def _load(self) -> None:
        try:
            if self.path.is_file():
                data = json.loads(self.path.read_text(encoding="utf-8"))
                self.cookies = dict(data.get("cookies") or {})
                self.uin = str(data.get("uin") or "")
                self.nickname = str(data.get("nickname") or "")
                self.vip = bool(data.get("vip"))
                self.vip_level = int(data.get("vip_level") or 0)
                self.vip_super = bool(data.get("vip_super"))
                self.vip_year = bool(data.get("vip_year"))
                self.vip_expires = str(data.get("vip_expires") or "")
                self.logged_in = bool(self.cookies)
                if self.logged_in:
                    self.session.cookies.update(self.cookies)
        except (OSError, ValueError, json.JSONDecodeError):
            pass

    def _save(self) -> None:
        exists = self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "cookies": self.cookies,
            "uin": self.uin,
            "nickname": self.nickname,
            "vip": self.vip,
            "vip_level": self.vip_level,
            "vip_super": self.vip_super,
            "vip_year": self.vip_year,
            "vip_expires": self.vip_expires,
            "saved_at": int(time.time()),
        }
        handle = self.path.with_suffix(".tmp")
        handle.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
        os.replace(handle, self.path)

    def cookie_header(self) -> str:
        """Compact `name=value; ...` string handed to the device once."""
        with self.lock:
            if not self.logged_in:
                return ""
            return "; ".join(f"{k}={v}" for k, v in self.cookies.items())

    # How long a confirmed entitlement is trusted before the console re-reads
    # it. Long enough that the 15-second device polling never triggers a VIP
    # round trip, short enough that a renewal shows up without a restart.
    VIP_REFRESH_SECONDS = 600.0

    def _refresh_vip_if_stale(self) -> None:
        """Re-read the entitlement once, then at a slow interval.

        A session file written before this fix stored vip=False from the broken
        probe, so the cached value cannot be trusted when the process starts.
        """
        if not self.logged_in or not self.uin:
            return
        now = time.time()
        if self._vip_checked_at and now - self._vip_checked_at < self.VIP_REFRESH_SECONDS:
            return
        self._vip_checked_at = now
        self._probe_vip()

    def status(self) -> dict[str, Any]:
        with self.lock:
            # A browser login thread may have died (e.g. user closed the
            # window, process killed). Release the lock after the window's
            # maximum lifetime so a retry is always possible.
            if getattr(self, "_login_running", False) and \
               time.time() - getattr(self, "_login_started", 0) > 300:
                self._login_running = False
            self._refresh_vip_if_stale()
            return {
                "logged_in": self.logged_in,
                "nickname": self.nickname,
                "uin": self.uin,
                "vip": self.vip,
                "vip_level": self.vip_level,
                "vip_super": self.vip_super,
                "vip_year": self.vip_year,
                "vip_expires": self.vip_expires,
                "login_running": bool(getattr(self, "_login_running", False)),
                # The firmware searches and streams QQ Music on its own, so it
                # needs the cookie bundle. Without it every device-side request
                # reported "not logged in" even though the console showed a
                # valid session.
                "cookies": self.cookie_header(),
            }

    # ---- QR login ----
    def qr_begin(self) -> dict[str, Any]:
        with self.lock:
            import time as _time

            params = {
                "appid": QR_APPID,
                "e": "2",
                "l": "M",
                "s": "3",
                "d": "72",
                "v": "4",
                "t": str(_time.time()),
                "daid": "383",
                "pt_3rd_aid": QR_PT_AID,
            }
            response = self.session.get(
                "https://ptlogin2.qq.com/ptqrshow", params=params, timeout=15
            )
            response.raise_for_status()
            qrsig = self.session.cookies.get("qrsig") or ""
            self.pending_qrsig = qrsig
            self.pending_created = _time.time()
            self.login_check_url = (
                "https://ptlogin2.qq.com/ptqrlogin?u1=https%3A%2F%2Fy.qq.com%2F"
                "&ptqrtoken={token}&ptredirect=0&h=1&t=1&g=1&from_ui=1"
                "&ptlang=2052&action=0-0-{ts}&js_ver=20102616&js_type=1"
                "&login_sig=&pt_uistyle=40&aid={appid}&daid=383&pt_3rd_aid={aid}"
            ).format(token=_hash33(qrsig), ts=int(_time.time() * 1000),
                     appid=QR_APPID, aid=QR_PT_AID)
            return {
                "image": "data:image/png;base64,"
                + base64.b64encode(response.content).decode("ascii"),
                "expires_in": 120,
            }

    def qr_poll(self) -> dict[str, Any]:
        with self.lock:
            if not self.pending_qrsig:
                return {"state": "idle"}
            if time.time() - self.pending_created > 120:
                self.pending_qrsig = ""
                return {"state": "expired"}
            response = self.session.get(self.login_check_url, timeout=15)
            text = response.text
            if "二维码未失效" in text or "ptuiCB('66'" in text:
                return {"state": "waiting"}
            if "二维码已失效" in text or "ptuiCB('65'" in text:
                self.pending_qrsig = ""
                return {"state": "expired"}
            if "登录成功" in text or "ptuiCB('0'" in text:
                self.pending_qrsig = ""
                self._finish_login(text)
                return {"state": "ok", **self.status()}
            if "已扫描" in text or "ptuiCB('67'" in text:
                return {"state": "scanned"}
            return {"state": "waiting"}

    def _finish_login(self, text: str) -> None:
        self.cookies = {c.name: c.value for c in self.session.cookies}
        self.logged_in = True
        match = re.search(r"uin=o(\d+)", text) or re.search(r"(\d{5,})", text)
        self.uin = match.group(1) if match else ""
        self.nickname = "QQ 音乐用户"
        self.vip = self._probe_vip()
        self._vip_checked_at = time.time()
        self._save()

    def _probe_vip(self) -> bool:
        """Read the account's real membership state.

        SRFVipQuery_V2 is the endpoint the QQ Music profile itself uses. The
        previous probe hit fcg_music_express_mobile3.fcg, which answers with an
        empty body, so json() always raised and every account -- including a
        paying 绿钻豪华版 one -- was cached as non-VIP.
        """
        self.vip_level = 0
        self.vip_super = False
        self.vip_year = False
        self.vip_expires = ""
        if not self.logged_in or not self.uin:
            return False
        body = {
            "comm": {"uin": self._uin(), "format": "json", "ct": 24, "cv": 0},
            "req_0": {
                "module": "userInfo.VipQueryServer",
                "method": "SRFVipQuery_V2",
                "param": {"uin_list": [self.uin]},
            },
        }
        try:
            response = self.session.post(
                "https://u.y.qq.com/cgi-bin/musicu.fcg", json=body, timeout=10
            )
            data = response.json()
            info = ((data.get("req_0") or {}).get("data") or {}).get("infoMap") or {}
            entry = info.get(str(self.uin)) or info.get(self.uin) or {}
            if not entry:
                return False
            self.vip = bool(entry.get("iVipFlag"))
            self.vip_level = int(entry.get("iCurLevel") or 0)
            self.vip_super = bool(entry.get("iSuperVip"))
            self.vip_year = bool(entry.get("iYearFlag"))
            self.vip_expires = str(entry.get("sOverDateTime") or "")
            return self.vip
        except Exception:
            return False

    def browser_login(self, window_seconds: int = 180) -> dict[str, Any]:
        """Open a real browser window so the user can sign in normally.

        Blocks until the session cookies appear or the window times out. The
        management page calls this in a background thread and polls status.
        """
        from qqmusic_login import run_login

        self._login_started = time.time()
        try:
            result = run_login(window_seconds)
        finally:
            pass
        with self.lock:
            if not result.get("logged_in"):
                return {"state": "timeout"}
            cookies = result.get("cookies") or {}
            self.cookies = dict(cookies)
            self.session.cookies.clear()
            self.session.cookies.update(self.cookies)
            self.logged_in = True
            self.uin = ""
            for key in ("qm_uin", "p_uin", "uin"):
                value = self.cookies.get(key, "")
                if value:
                    self.uin = value.lstrip("oO")
                    break
            self.nickname = "QQ 音乐用户"
            self.vip = self._probe_vip()
            self._save()
            return {"state": "ok", **self.status()}

    def logout(self) -> None:
        with self.lock:
            self.logged_in = False
            self.cookies = {}
            self.uin = ""
            self.nickname = ""
            self.vip = False
            try:
                self.path.unlink()
            except OSError:
                pass

    # ---- search / resolve ----
    def _uin(self) -> str:
        return self.uin or "0"

    def search(self, query: str, limit: int = 5) -> list[dict[str, Any]]:
        body = {
            "comm": {"ct": "19", "cv": "1859", "uin": self._uin()},
            "req": {
                "method": "DoSearchForQQMusicDesktop",
                "module": "music.search.SearchCgiService",
                "param": {
                    "num_per_page": max(1, min(limit, 10)),
                    "page_num": 1,
                    "query": query,
                    "search_type": 0,
                },
            },
        }
        response = self.session.post(SEARCH_URL, json=body, timeout=15)
        # The search endpoint answers with ~48 KB because it embeds extra
        # metadata; decode the compact fields first so the device only ever
        # sees mid/title/artist/album.
        try:
            data = response.json()
        except ValueError:
            data = json.loads(response.content.decode("utf-8", errors="replace"))
        req = data.get("req") or {}
        # QQ answers a search made without a live session with code 2001: an
        # empty song list plus a feedbackURL pointing at its login page. Treat
        # that as "sign in again" -- returning [] made the assistant tell the
        # user the song did not exist.
        if req.get("code") == 2001:
            self.logged_in = False
            raise QQMusicNotLoggedIn(
                "QQ Music login expired; sign in again in the management centre."
            )
        songs = ((req.get("data") or {}).get("body") or {}).get("song", {}).get("list") or []
        results = []
        for song in songs:
            singers = "/".join(item.get("name", "") for item in song.get("singer", []))
            results.append({
                "mid": song.get("mid", ""),
                "title": song.get("name", ""),
                "artist": singers,
                "album": (song.get("album") or {}).get("name", ""),
            })

        # A query such as "苦瓜 陈奕迅" is unambiguous, so the device plays
        # result #1 straight away. Search happily returns renditions this
        # account cannot stream (the album cut of 苦瓜 needs a purchase), and
        # the stream endpoint then answers 404 -- which the firmware showed as
        # "连接异常". Ask the vkey server which renditions actually resolve and
        # rank the streamable ones first, so an unambiguous query can never
        # auto-pick a track that is guaranteed to fail.
        verdicts = self._vkey_lookup([item["mid"] for item in results])
        for item in results:
            verdict = verdicts.get(item["mid"])
            # Unknown verdicts stay playable: a lookup failure must not hide
            # songs that would have worked.
            item["playable"] = True if verdict is None else verdict["available"]
        results.sort(key=lambda item: not item["playable"])
        return results

    def _vkey_lookup(self, mids: list[str]) -> dict[str, dict[str, Any]]:
        """Ask the vkey server which of these mids resolve for this session.

        One batched request classifies the whole result page, so ranking costs
        a single round trip instead of one per song. Failures are reported as
        an empty map so callers can fall back to treating songs as playable.
        """
        mids = [mid for mid in mids if mid]
        if not mids:
            return {}
        body = {
            "req_0": {
                "module": "vkey.GetVkeyServer",
                "method": "CgiGetVkey",
                "param": {
                    "guid": str(random.randint(10_000_000, 99_999_999)),
                    "songmid": mids,
                    "songtype": [0] * len(mids),
                    "uin": self._uin(),
                    "loginflag": 1 if self.logged_in else 0,
                    "platform": "20",
                },
            },
            "comm": {"uin": self._uin(), "format": "json", "ct": 24, "cv": 0},
        }
        try:
            response = self.session.post(STREAM_URL, json=body, timeout=15)
            data = response.json()
        except Exception:
            return {}
        req = data.get("req_0") or {}
        info_list = ((req.get("data") or {}).get("midurlinfo") or [])
        host = ((req.get("data") or {}).get("sip") or ["https://dl.stream.qqmusic.qq.com/"])[0]
        verdicts: dict[str, dict[str, Any]] = {}
        for info in info_list:
            song_mid = info.get("songmid") or ""
            purl = info.get("purl") or ""
            verdicts[song_mid] = {
                "available": bool(purl),
                "url": host + purl if purl else "",
                "reason": "" if purl else "vip_or_region",
            }
        return verdicts

    def resolve(self, mid: str) -> dict[str, Any]:
        """Return a short-lived stream URL, or {'available': False, 'reason': ...}."""
        body = {
            "req_0": {
                "module": "vkey.GetVkeyServer",
                "method": "CgiGetVkey",
                "param": {
                    "guid": str(random.randint(10_000_000, 99_999_999)),
                    "songmid": [mid],
                    "songtype": [0],
                    "uin": self._uin(),
                    "loginflag": 1 if self.logged_in else 0,
                    "platform": "20",
                },
            },
            "comm": {"uin": self._uin(), "format": "json", "ct": 24, "cv": 0},
        }
        verdict = self._vkey_lookup([mid]).get(mid)
        if verdict is None:
            # The lookup itself failed; fall back to the direct request so a
            # transient error is not reported to the device as "unavailable".
            response = self.session.post(STREAM_URL, json=body, timeout=15)
            data = response.json()
            req = data.get("req_0") or {}
            info = ((req.get("data") or {}).get("midurlinfo") or [{}])[0]
            purl = info.get("purl") or ""
            host = ((req.get("data") or {}).get("sip")
                    or ["https://dl.stream.qqmusic.qq.com/"])[0]
            if req.get("code") != 0 or not purl:
                return {"available": False, "reason": "vip_or_region"}
            return {"available": True, "url": host + purl, "expires_in": 180}
        if not verdict["available"]:
            return {"available": False, "reason": verdict["reason"]}
        return {"available": True, "url": verdict["url"], "expires_in": 180}

    # ---- playlists ----
    def playlists(self) -> list[dict[str, Any]]:
        """Return the account's playlists, including the special 我喜欢 list."""
        body = {
            "comm": {"ct": 24, "cv": 0, "uin": self._uin(), "format": "json"},
            "req_0": {
                "module": "music.musicasset.PlaylistBaseRead",
                "method": "GetPlaylistByUin",
                "param": {"uin": self._uin()},
            },
        }
        response = self.session.post(SEARCH_URL, json=body, timeout=15)
        data = response.json()
        req = data.get("req_0") or {}
        if req.get("code") not in (0, None):
            self.logged_in = False
            raise QQMusicNotLoggedIn(
                "QQ Music login expired; sign in again in the management centre."
            )
        items = ((req.get("data") or {}).get("v_playlist") or [])
        results = []
        for item in items:
            results.append({
                "name": item.get("dirName", ""),
                "dissid": item.get("tid", 0),
                "dirid": item.get("dirId", 0),
                "count": int(item.get("songNum") or 0),
            })
        return results

    def playlist_songs(
        self, playlist: str, limit: int = 5, offset: int = 0
    ) -> dict[str, Any]:
        """Resolve a playlist by name (我喜欢/我的音乐/...) and return its songs.

        ``playlist`` may be a playlist name, or the literal "我喜欢" / "favorite"
        aliases the voice assistant produces. Matching prefers an exact name,
        then a substring, then falls back to the first playlist.
        """
        wanted = (playlist or "").strip()
        aliases = {
            "我喜欢": "我喜欢",
            "我喜欢的": "我喜欢",
            "我喜欢的音乐": "我喜欢",
            "favorite": "我喜欢",
            "favorites": "我喜欢",
            "liked": "我喜欢",
            "我的歌单": "",
            "歌单": "",
        }
        wanted = aliases.get(wanted, wanted)
        lists = self.playlists()
        chosen = None
        # Special numeric shortcuts the firmware may send.
        for item in lists:
            if item["name"] == wanted:
                chosen = item
                break
        if chosen is None and wanted:
            for item in lists:
                if wanted in item["name"]:
                    chosen = item
                    break
        if chosen is None:
            chosen = next((item for item in lists if item["name"] == "我喜欢"), None)
        if chosen is None and lists:
            chosen = lists[0]
        if chosen is None:
            return {"name": "", "count": 0, "songs": []}

        # 我喜欢 is a special dirId (201); the generic diss endpoint handles
        # every entry returned by GetPlaylistByUin.
        body = {
            "comm": {"ct": 24, "cv": 0, "uin": self._uin(), "format": "json"},
            "req_0": {
                "module": "music.srfDissInfo.DissInfo",
                "method": "CgiGetDiss",
                "param": {
                    "disstid": chosen["dissid"],
                    "dirid": chosen["dirid"],
                    "tag": 1,
                    "song_begin": max(0, offset),
                    "song_num": max(1, min(limit, 10)),
                },
            },
        }
        response = self.session.post(SEARCH_URL, json=body, timeout=15)
        data = (response.json().get("req_0") or {}).get("data") or {}
        raw_songs = data.get("songlist") or []
        songs = []
        for song in raw_songs:
            singers = "/".join(s.get("name", "") for s in (song.get("singer") or []))
            songs.append({
                "mid": song.get("mid", ""),
                "title": song.get("name", ""),
                "artist": singers,
                "album": (song.get("album") or {}).get("name", ""),
            })
        # Rank streamable renditions first, exactly like search().
        verdicts = self._vkey_lookup([item["mid"] for item in songs])
        for item in songs:
            verdict = verdicts.get(item["mid"])
            item["playable"] = True if verdict is None else verdict["available"]
        songs.sort(key=lambda item: not item["playable"])
        return {
            "name": chosen["name"],
            "count": chosen["count"],
            "songs": songs,
        }
