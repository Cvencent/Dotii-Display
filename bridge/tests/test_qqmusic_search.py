# -*- coding: utf-8 -*-
"""QQ Music search must only offer renditions this session can actually stream.

The device plays the first result of an unambiguous query. On 2026-10-23 the
album rendition of 苦瓜 (pay_play=1) was first, the bridge's stream endpoint
answered 404 vip_or_region, and the firmware flipped the face to "连接异常".
Ranking by the real vkey answer keeps that from happening.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlparse

BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))

from qqmusic import (  # noqa: E402
    SEARCH_URL,
    STREAM_URL,
    QQMusicAccount,
    QQMusicNotLoggedIn,
)


class _FakeResponse:
    def __init__(self, payload: dict, url: str) -> None:
        self._payload = payload
        self.url = url

    def json(self) -> dict:
        return self._payload

    @property
    def content(self) -> bytes:
        return json.dumps(self._payload).encode("utf-8")

    @property
    def status_code(self) -> int:
        return 200

    def raise_for_status(self) -> None:
        return None


class _FakeSession:
    """Answers search and vkey for a fixed song list plus playability map."""

    def __init__(self, songs: list[dict], playable: set[str]) -> None:
        self.songs = songs
        self.playable = playable
        self.headers: dict[str, str] = {}

    vip_payload: dict | None = None
    search_code: int = 0

    def post(self, url, *args, **kwargs) -> _FakeResponse:  # noqa: ANN002, ANN003
        body = kwargs.get("json") or {}
        module = ((body.get("req_0") or {}).get("module") or "")
        if "VipQueryServer" in module:         # VIP entitlement
            payload = self.vip_payload if self.vip_payload is not None else {"req_0": {"code": 0, "data": {}}}
            return _FakeResponse(payload, url)
        if "req" in body:                      # search
            songs = self.songs if self.search_code == 0 else []
            data = {"code": 0, "body": {"song": {"list": songs}}}
            if self.search_code != 0:
                data["feedbackURL"] = (
                    "https://y.qq.com/wk_v17/common_login.html#/login?QQAppid=100497308")
            payload = {"req": {"code": self.search_code, "data": data}}
            return _FakeResponse(payload, url)
        # The vkey server answers with one entry per requested mid, each
        # carrying its own songmid -- that is how the caller correlates them.
        mids = (((body.get("req_0") or {}).get("param") or {}).get("songmid") or [])
        entries = []
        for mid in mids:
            if mid in self.playable:
                entries.append({"songmid": mid, "purl": "M500%s.mp3" % mid, "result": 0})
            else:
                entries.append({"songmid": mid, "purl": "", "result": 104003})
        host = ["https://dl.stream.qqmusic.qq.com/"] if any(e["purl"] for e in entries) else []
        payload = {"req_0": {"code": 0, "data": {"sip": host, "midurlinfo": entries}}}
        return _FakeResponse(payload, url)


def song(mid: str, name: str = "苦瓜", artist: str = "陈奕迅") -> dict:
    return {"mid": mid, "name": name, "singer": [{"name": artist}],
            "album": {"name": ""}, "pay": {"pay_play": 1}}


ALBUM = "000B9Nho3hgIY7"   # purchase/VIP-only rendition
LIVE = "000UPdbN1GKIda"    # freely streamable rendition
EXTRA = "004Podzq4HZMYP"   # second freely streamable rendition


class QQMusicSearchPlayabilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="dotii-qqmusic-")
        self.account = QQMusicAccount(Path(self.temporary.name))
        self.account.logged_in = True
        self.account.uin = "1306681486"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def serve(self, songs: list[dict], playable: set[str]) -> None:
        self.account.session = _FakeSession(songs, playable)

    def test_unplayable_rendition_is_ranked_behind_a_streamable_one(self) -> None:
        """苦瓜's album cut cannot be streamed by this session. The device
        auto-plays result #1, so a playable rendition has to come first."""
        self.serve([song(ALBUM), song(LIVE), song(EXTRA)],
                   playable={LIVE, EXTRA})

        results = self.account.search("苦瓜 陈奕迅", 3)

        self.assertEqual([item["mid"] for item in results], [LIVE, EXTRA, ALBUM])
        self.assertEqual([item["playable"] for item in results], [True, True, False])

    def test_playable_order_is_preserved_when_everything_streams(self) -> None:
        self.serve([song(ALBUM), song(LIVE)], playable={ALBUM, LIVE})

        results = self.account.search("苦瓜 陈奕迅", 3)

        self.assertEqual([item["mid"] for item in results], [ALBUM, LIVE])

    def test_unplayable_results_are_kept_so_the_caller_can_report_them(self) -> None:
        """When nothing streams, the caller still needs the titles to explain
        why playback is impossible instead of showing a connection error."""
        self.serve([song(ALBUM)], playable=set())

        results = self.account.search("苦瓜 陈奕迅", 3)

        self.assertEqual([item["mid"] for item in results], [ALBUM])
        self.assertFalse(results[0]["playable"])

    def test_resolve_reports_why_a_rendition_is_unavailable(self) -> None:
        self.serve([song(ALBUM)], playable=set())

        resolved = self.account.resolve(ALBUM)

        self.assertFalse(resolved["available"])
        self.assertEqual(resolved["reason"], "vip_or_region")


class QQMusicVipProbeTests(unittest.TestCase):
    """The console's VIP badge must reflect the account, not a broken probe.

    The previous probe called fcg_music_express_mobile3.fcg, which answers with
    an empty body, so json() always raised and the flag was hard-wired to
    False -- a paying 绿钻豪华版 account still showed as a free account.
    """

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="dotii-qqmusic-vip-")
        self.account = QQMusicAccount(Path(self.temporary.name))
        self.account.logged_in = True
        self.account.uin = "1306681486"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def serve_vip(self, info: dict) -> None:
        session = _FakeSession([], set())
        session.vip_payload = {"req_0": {"code": 0, "data": {
            "curtime": 1792763836, "infoMap": {"1306681486": info}}}}
        self.account.session = session

    def test_super_vip_account_is_reported_as_vip(self) -> None:
        self.serve_vip({"iVipFlag": 1, "iSuperVip": 1, "iYearFlag": 1,
                        "iCurLevel": 7, "sOverDateTime": "2026-11-25 22:42:43"})

        self.assertTrue(self.account._probe_vip())

    def test_plain_vip_account_is_reported_as_vip(self) -> None:
        self.serve_vip({"iVipFlag": 1, "iSuperVip": 0, "iYearFlag": 0})

        self.assertTrue(self.account._probe_vip())

    def test_account_without_vip_is_not_reported_as_vip(self) -> None:
        self.serve_vip({"iVipFlag": 0, "iSuperVip": 0, "iYearFlag": 0})

        self.assertFalse(self.account._probe_vip())

    def test_missing_entry_is_not_reported_as_vip(self) -> None:
        """An anonymous reply has an empty infoMap; that must not raise."""
        session = _FakeSession([], set())
        session.vip_payload = {"req_0": {"code": 0, "data": {"curtime": 1, "infoMap": {}}}}
        self.account.session = session

        self.assertFalse(self.account._probe_vip())


class QQMusicVipCacheTests(unittest.TestCase):
    """A session file written by the broken probe must self-heal.

    The old probe cached vip=false for every account, and _load() trusted that
    value, so a paying account stayed mislabelled until it logged in again.
    """

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="dotii-qqmusic-cache-")
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_legacy_session(self) -> None:
        """Exactly what the buggy probe persisted: vip=false, no tier fields."""
        (self.root / "qqmusic-session.json").write_text(json.dumps({
            "cookies": {"qqmusic_key": "key", "uin": "1306681486"},
            "uin": "1306681486",
            "nickname": "QQ 音乐用户",
            "vip": False,
            "saved_at": 1792763639,
        }), encoding="utf-8")

    def serve_vip(self, account: QQMusicAccount) -> None:
        session = _FakeSession([], set())
        session.vip_payload = {"req_0": {"code": 0, "data": {
            "curtime": 1792763836, "infoMap": {"1306681486": {
                "iVipFlag": 1, "iSuperVip": 1, "iYearFlag": 1,
                "iCurLevel": 7, "sOverDateTime": "2026-11-25 22:42:43"}}}}}
        account.session = session

    def test_legacy_cached_flag_is_refreshed_on_status(self) -> None:
        self.write_legacy_session()
        account = QQMusicAccount(self.root)
        self.assertFalse(account.vip)          # the stale cached value
        self.serve_vip(account)

        status = account.status()

        self.assertTrue(status["vip"])
        self.assertTrue(status["vip_super"])
        self.assertEqual(status["vip_level"], 7)

    def test_refresh_happens_once_not_on_every_status(self) -> None:
        self.write_legacy_session()
        account = QQMusicAccount(self.root)
        session = _FakeSession([], set())
        session.vip_payload = {"req_0": {"code": 0, "data": {
            "curtime": 1, "infoMap": {"1306681486": {"iVipFlag": 1}}}}}
        account.session = session
        calls = []
        original = session.post

        def counting_post(*args, **kwargs):
            calls.append(1)
            return original(*args, **kwargs)

        session.post = counting_post
        account.status()
        account.status()
        account.status()

        self.assertEqual(len(calls), 1)


class QQMusicSearchAuthTests(unittest.TestCase):
    """The device must be told "not logged in", not handed an empty result.

    QQ answers a search made without a live session with req.code 2001, an
    empty song list, and a feedbackURL pointing at the login page. The old
    bridge returned an empty list, so the assistant told the user the song did
    not exist instead of asking them to sign in again.
    """

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="dotii-qqmusic-auth-")
        self.account = QQMusicAccount(Path(self.temporary.name))
        self.account.logged_in = True
        self.account.uin = "1306681486"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_logged_out_reply_raises_a_clear_error(self) -> None:
        session = _FakeSession([], set())
        session.search_code = 2001
        self.account.session = session

        with self.assertRaises(QQMusicNotLoggedIn):
            self.account.search("苦瓜 陈奕迅", 3)

    def test_successful_search_is_unaffected(self) -> None:
        self.account.session = _FakeSession([song(LIVE)], playable={LIVE})

        results = self.account.search("苦瓜 陈奕迅", 3)

        self.assertEqual([item["mid"] for item in results], [LIVE])

    def test_genuinely_empty_result_is_not_a_login_error(self) -> None:
        """A live session that matches nothing returns code 0 with no songs;
        that is a legitimately empty search, not an auth failure."""
        session = _FakeSession([], set())
        session.search_code = 0
        self.account.session = session

        self.assertEqual(self.account.search("zzzz-no-such-song", 3), [])


if __name__ == "__main__":
    unittest.main()
