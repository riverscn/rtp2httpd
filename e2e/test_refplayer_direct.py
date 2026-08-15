"""Focused E2E coverage for RefPlayer's stdin-only direct M3U protocol."""

from __future__ import annotations

import json
import os
import subprocess

from helpers import R2HProcess, find_free_port, http_get

_CATALOG_MAGIC = "RTP2HTTPD-REFPLAYER-CATALOG/1"
_RESOLVE_MAGIC = "RTP2HTTPD-REFPLAYER-RESOLVE/1"
_BEGIN_EPOCH = 1704110400  # 2024-01-01 12:00:00 UTC
_END_EPOCH = 1704114000  # 2024-01-01 13:00:00 UTC


def _catalog_frame(base_url: str, playlist: str) -> bytes:
    return f"{_CATALOG_MAGIC}\n{base_url}\n{playlist}".encode()


def _resolve_frame(
    base_url: str,
    source_id: str,
    playlist: str,
    timezone_offset_seconds: int = 0,
) -> bytes:
    return (
        f"{_RESOLVE_MAGIC}\n{base_url}\n{source_id}\n{_BEGIN_EPOCH}\n{_END_EPOCH}\n"
        f"{timezone_offset_seconds}\n{playlist}"
    ).encode()


def _run(r2h_binary, command: str, payload: bytes, *extra_args: str) -> tuple[int, dict]:
    completed = subprocess.run(
        [str(r2h_binary), command, *extra_args],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=3,
    )
    assert completed.stderr == b""
    return completed.returncode, json.loads(completed.stdout)


def test_stock_http_playlist_is_direct_and_preserves_web_player_semantics(r2h_binary):
    base_url = "https://iptv.example.test/app/lists/playlist.m3u?token=playlist-token"
    playlist = """\
#EXTM3U x-tvg-url="../guide/epg.xml" catchup-source="../archive/$HD-${(b)yyyyMMddHHmmss}/${(e)yyyyMMddHHmmss}.m3u8?key=archive-token&r2h-seek-offset=3600&r2h-seek-mode=passthrough" catchup-days="7"
#EXTINF:-1 tvg-id="news-id" tvg-name="News Guide" tvg-logo="//cdn.example.test/logo.png" group-title="News;HD" group-title="Featured",Example News
/live/news.m3u8?token=live-token$HD
#EXTINF:-1 group-title="News;HD" group-title="Featured",Example News
https://edge.example.test/news-low.m3u8$SD
#EXTINF:-1 group-title="Other",Scheme Relative
//media.example.test/channel.m3u8
#EXTINF:-1 group-title="Other",Query Relative
?channel=query
#EXTINF:-1 group-title="Other",Parent Relative
../streams/../live/parent.m3u8
"""

    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, playlist),
    )

    assert returncode == 0
    assert payload["schema_version"] == 2
    assert payload["mode"] == "direct"
    assert payload["reason"] is None
    assert payload["proxy_m3u"] is None
    assert payload["epg_url"] == "https://iptv.example.test/app/guide/epg.xml"

    channels = {channel["title"]: channel for channel in payload["channels"]}
    news = channels["Example News"]
    assert news["group_titles"] == ["News", "HD", "Featured"]
    assert news["tvg_id"] == "news-id"
    assert news["tvg_name"] == "News Guide"
    assert news["logo_url"] == "https://cdn.example.test/logo.png"
    assert len(news["sources"]) == 2
    assert [source["label"] for source in news["sources"]] == ["HD", "SD"]
    assert news["sources"][0]["live_url"] == "https://iptv.example.test/live/news.m3u8?token=live-token"
    assert news["sources"][1]["live_url"] == "https://edge.example.test/news-low.m3u8"
    assert news["sources"][0]["live_route"] == "direct"
    assert news["sources"][0]["catchup_route"] == "direct"
    assert news["sources"][0]["catchup"] == {
        "source_id": news["sources"][0]["id"],
        "mode": "default",
        "retention_seconds": 7 * 86400,
    }
    assert news["sources"][1]["catchup"]["mode"] == "default"

    assert channels["Scheme Relative"]["sources"][0]["live_url"] == "https://media.example.test/channel.m3u8"
    assert channels["Query Relative"]["sources"][0]["live_url"] == (
        "https://iptv.example.test/app/lists/playlist.m3u?channel=query"
    )
    assert channels["Parent Relative"]["sources"][0]["live_url"] == ("https://iptv.example.test/app/live/parent.m3u8")

    source_id = news["sources"][0]["id"]
    returncode, resolved = _run(
        r2h_binary,
        "--refplayer-direct-resolve",
        _resolve_frame(base_url, source_id, playlist, timezone_offset_seconds=8 * 3600),
    )
    assert returncode == 0
    assert resolved == {
        "schema_version": 2,
        "source_id": source_id,
        "url": ("https://iptv.example.test/app/archive/$HD-20240101210000/20240101220000.m3u8?key=archive-token"),
    }


def test_native_rtp_udp_rtsp_are_all_helper_candidates(r2h_binary):
    base_url = "http://iptv.example.test/playlist.m3u"
    raw_live = """\
#EXTM3U
#EXTINF:-1,Raw Multicast
rtp://239.1.1.1:1234
#EXTINF:-1,Raw UDP
udp://239.1.1.2:1234
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, raw_live),
    )
    assert returncode == 0
    assert payload["mode"] == "mixed"
    assert payload["reason"] == "live_source_requires_helper"
    assert [channel["sources"][0]["live_url"] for channel in payload["channels"]] == [
        "rtp://239.1.1.1:1234",
        "udp://239.1.1.2:1234",
    ]
    assert [channel["sources"][0]["live_route"] for channel in payload["channels"]] == ["helper", "helper"]
    assert payload["proxy_m3u"] == (
        '#EXTM3U\n#EXTINF:-1 refplayer-source-id="%s",refplayer-rtsp\n'
        "rtp://239.1.1.1:1234\n"
        '#EXTINF:-1 refplayer-source-id="%s",refplayer-rtsp\n'
        "udp://239.1.1.2:1234\n"
        % (payload["channels"][0]["sources"][0]["id"], payload["channels"][1]["sources"][0]["id"])
    )

    native_catchup = """\
#EXTM3U
#EXTINF:-1 catchup-source="udp://239.1.1.3:1234?start=${(b)timestamp}",Native Catchup
rtp://239.1.1.1:1234
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, native_catchup),
    )
    assert returncode == 0
    assert payload["mode"] == "mixed"
    source = payload["channels"][0]["sources"][0]
    assert source["live_route"] == "helper"
    assert source["catchup_route"] == "helper"
    returncode, resolved = _run(
        r2h_binary,
        "--refplayer-direct-resolve",
        _resolve_frame(base_url, source["id"], native_catchup),
    )
    assert returncode == 0
    assert resolved["url"] == "udp://239.1.1.3:1234?start=%d" % _BEGIN_EPOCH

    raw_rtsp = """\
#EXTM3U
#EXTINF:-1,RTSP Needs Probe
rtsp://iptv.example.test/live
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, raw_rtsp),
    )
    assert returncode == 0
    assert payload["mode"] == "mixed"
    assert payload["reason"] == "live_source_requires_helper"
    source = payload["channels"][0]["sources"][0]
    assert source["live_url"] == "rtsp://iptv.example.test/live"
    assert source["live_route"] == "helper"
    assert source["catchup_route"] is None
    assert payload["proxy_m3u"] == (
        '#EXTM3U\n#EXTINF:-1 refplayer-source-id="%s",refplayer-rtsp\n'
        "rtsp://iptv.example.test/live\n" % source["id"]
    )

    rtsp_append_catchup = """\
#EXTM3U
#EXTINF:-1 catchup="append" catchup-source="?playseek=${(b)timestamp}-${(e)timestamp}",RTSP Append
rtsp://iptv.example.test/live
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, rtsp_append_catchup),
    )
    assert returncode == 0
    source = payload["channels"][0]["sources"][0]
    assert source["live_route"] == "helper"
    assert source["catchup_route"] == "helper"
    assert payload["proxy_m3u"] == (
        '#EXTM3U\n#EXTINF:-1 refplayer-source-id="%s" catchup="default" '
        'catchup-source="rtsp://iptv.example.test/live?playseek=${(b)timestamp}-${(e)timestamp}",refplayer-rtsp\n'
        "rtsp://iptv.example.test/live\n" % source["id"]
    )
    returncode, resolved = _run(
        r2h_binary,
        "--refplayer-direct-resolve",
        _resolve_frame(base_url, source["id"], rtsp_append_catchup),
    )
    assert returncode == 65
    assert resolved["error"]["code"] == "resolve_failed"

    raw_catchup = """\
#EXTM3U
#EXTINF:-1 catchup-source="rtsp://archive.example.test/replay/${(b)timestamp}",HTTP Live
https://media.example.test/live.m3u8
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, raw_catchup),
    )
    assert returncode == 0
    assert payload["mode"] == "mixed"
    assert payload["reason"] == "catchup_source_requires_helper"
    source = payload["channels"][0]["sources"][0]
    assert source["live_route"] == "direct"
    assert source["catchup_route"] == "helper"
    assert source["catchup"] == {
        "source_id": source["id"],
        "mode": "default",
        "retention_seconds": None,
    }
    assert payload["proxy_m3u"] == (
        '#EXTM3U\n#EXTINF:-1 refplayer-source-id="%s" catchup="default" '
        'catchup-source="rtsp://archive.example.test/replay/${(b)timestamp}",refplayer-rtsp\n'
        "rtsp://archive.example.test/replay/${(b)timestamp}\n" % source["id"]
    )

    invalid_catchup = """\
#EXTM3U
#EXTINF:-1 catchup-source="ftp://archive.example.test/replay",Still Direct
https://media.example.test/live.m3u8
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, invalid_catchup),
    )
    assert returncode == 0
    assert payload["mode"] == "direct"
    assert payload["channels"][0]["sources"][0]["catchup"] is None


def test_proxy_snapshot_does_not_copy_untrusted_metadata_or_accept_m3u_delimiters_in_rtsp_urls(r2h_binary):
    playlist = """\
#EXTM3U
#EXTINF:-1 group-title="Injected\\\" attribute=\\\"value" source-label="also-untrusted",Injected Title \\" \\\\x
rtsp://iptv.example.test/live
#EXTINF:-1,Bad Quote URL
rtsp://iptv.example.test/live"injected
#EXTINF:-1,Bad Backslash URL
rtsp://iptv.example.test/live\\injected
#EXTINF:-1,Direct Survivor
https://media.example.test/live.m3u8
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame("https://iptv.example.test/playlist.m3u", playlist),
    )

    assert returncode == 0
    candidate_sources = [
        source
        for channel in payload["channels"]
        for source in channel["sources"]
        if source["live_route"] == "helper"
    ]
    assert len(candidate_sources) == 1
    proxy_m3u = payload["proxy_m3u"]
    assert proxy_m3u == (
        '#EXTM3U\n#EXTINF:-1 refplayer-source-id="%s",refplayer-rtsp\n'
        "rtsp://iptv.example.test/live\n" % candidate_sources[0]["id"]
    )
    assert "Injected" not in proxy_m3u
    assert "attribute=" not in proxy_m3u
    assert "live\\injected" not in proxy_m3u
    assert 'live"injected' not in proxy_m3u


def test_mixed_http_live_rtsp_catchup_snapshot_round_trips_source_id(r2h_binary):
    """The Host can embed the opaque snapshot without parsing or rewriting M3U."""
    playlist = """\
#EXTM3U
#EXTINF:-1 catchup-source="rtsp://archive.example.test/replay/${(b)timestamp}",HTTP Live
https://media.example.test/live.m3u8
"""
    returncode, direct = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame("https://iptv.example.test/playlist.m3u", playlist),
    )
    assert returncode == 0
    source = direct["channels"][0]["sources"][0]
    assert source["live_route"] == "direct"
    assert source["catchup_route"] == "helper"

    port = find_free_port()
    config = f"""\
[global]
verbosity = 4
maxclients = 10

[bind]
* {port}

[services]
{direct["proxy_m3u"]}"""
    env = os.environ.copy()
    env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
    process = R2HProcess(r2h_binary, port, config_content=config, env=env)
    try:
        process.start()
        status, _, body = http_get("127.0.0.1", port, "/api/refplayer/v1/catalog")
        assert status == 200, body.decode(errors="replace")
        server = json.loads(body)
        assert server["schema_version"] == 2
        assert len(server["channels"]) == 1
        candidate = server["channels"][0]
        assert candidate["client_source_id"] == source["id"]
        assert candidate["catchup"] is not None
        assert "r2h-refplayer-source=" in candidate["catchup"]["url"]
    finally:
        process.stop()


def test_append_catchup_is_resolved_with_the_shared_url_template_engine(r2h_binary):
    base_url = "https://iptv.example.test/playlist.m3u"
    playlist = """\
#EXTM3U
#EXTINF:-1 catchup="append" catchup-source="&from=${(b)timestamp}&to=${(e)timestamp}",Append Catchup
https://media.example.test/live.m3u8?token=live-token
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, playlist),
    )
    assert returncode == 0
    source = payload["channels"][0]["sources"][0]
    assert source["catchup"]["mode"] == "append"

    returncode, resolved = _run(
        r2h_binary,
        "--refplayer-direct-resolve",
        _resolve_frame(base_url, source["id"], playlist),
    )
    assert returncode == 0
    assert resolved["url"] == (
        f"https://media.example.test/live.m3u8?token=live-token&from={_BEGIN_EPOCH}&to={_END_EPOCH}"
    )


def test_invalid_entry_does_not_break_later_channels_and_unsafe_userinfo_is_rejected(r2h_binary):
    playlist = """\
#EXTM3U
#EXTINF:-1,Bad Userinfo
https://user:password@media.example.test/live.m3u8
#EXTINF:-1,Good One
/good-one.m3u8
#EXTINF:-1,Also Good
../good-two.m3u8
"""
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame("https://iptv.example.test/app/playlist.m3u", playlist),
    )
    assert returncode == 0
    assert payload["mode"] == "direct"
    assert [channel["title"] for channel in payload["channels"]] == ["Good One", "Also Good"]


def test_protocol_bounds_unknown_source_and_arguments_are_structured_errors(r2h_binary):
    playlist = "#EXTM3U\n#EXTINF:-1,Channel\n/live.m3u8\n"
    base_url = "https://iptv.example.test/playlist.m3u"

    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-resolve",
        _resolve_frame(base_url, "0" * 32, playlist),
    )
    assert returncode == 65
    assert payload["error"]["code"] == "unknown_source"

    oversized = b"#" * (10 * 1024 * 1024 + 1)
    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        f"{_CATALOG_MAGIC}\n{base_url}\n".encode() + oversized,
    )
    assert returncode == 64
    assert payload["error"]["code"] == "invalid_frame"

    returncode, payload = _run(
        r2h_binary,
        "--refplayer-direct-catalog",
        _catalog_frame(base_url, playlist),
        "secret-must-not-be-an-argument",
    )
    assert returncode == 64
    assert payload["error"]["code"] == "invalid_arguments"
