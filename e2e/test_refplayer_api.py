"""Native RefPlayer catalog and epoch-based catchup API coverage."""

from __future__ import annotations

import json
import re
import time
from urllib.parse import urlsplit

from helpers import MockHTTPUpstream, R2HProcess, find_free_port, http_get


_BEGIN_EPOCH = 1704110400  # 2024-01-01 12:00:00 UTC
_END_EPOCH = 1704114000  # 2024-01-01 13:00:00 UTC


def _catalog(port: int, path: str = "/api/refplayer/v1/catalog", headers: dict | None = None) -> dict:
    status, response_headers, body = http_get("127.0.0.1", port, path, headers=headers)
    assert status == 200, body.decode(errors="replace")
    assert response_headers["Content-Type"] == "application/json"
    return json.loads(body)


def _request_path(url: str) -> str:
    parts = urlsplit(url)
    assert parts.scheme == "http"
    return parts.path + (("?" + parts.query) if parts.query else "")


def _append_range(url: str, start: int = _BEGIN_EPOCH, end: int = _END_EPOCH) -> str:
    separator = "&" if "?" in url else "?"
    return "%s%sr2h-refplayer-start=%d&r2h-refplayer-end=%d" % (url, separator, start, end)


def _base_config(port: int, services: str, global_lines: str = "") -> str:
    return f"""\
[global]
verbosity = 4
maxclients = 10
{global_lines}

[bind]
* {port}

[services]
{services}
"""


class TestRefPlayerCatalog:
    def test_schema_absolute_host_multi_source_and_stable_ids(self, r2h_binary):
        port = find_free_port()
        entries = [
            """#EXTINF:-1 group-title="News" catchup="default" catchup-days="7" catchup-source="http://10.0.0.1/archive/${(b)yyyyMMddHHmmss}/${(e)yyyyMMddHHmmss}.ts",Example News
rtp://239.1.1.1:1234$HD""",
            """#EXTINF:-1 group-title="News" catchup="default" catchup-days="7" catchup-source="rtsp://10.0.0.2/replay?tvdr=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}",Example News
rtsp://10.0.0.2/live$SD""",
        ]
        entries.insert(1, entries[0])

        def read_ids(service_entries: list[str], request_port: int):
            services = "#EXTM3U\n" + "\n".join(service_entries)
            config = _base_config(
                request_port,
                services,
                global_lines="use-relative-path-in-m3u = yes\nr2h-token = native-secret",
            )
            process = R2HProcess(r2h_binary, request_port, config_content=config)
            try:
                process.start()
                custom_host = "refplayer-loopback.invalid:%d" % request_port
                payload = _catalog(
                    request_port,
                    "/api/refplayer/v1/catalog?r2h-token=native-secret",
                    headers={"Host": custom_host},
                )
                assert payload["schema_version"] == 1
                assert isinstance(payload["helper_version"], str) and payload["helper_version"]
                assert len(payload["channels"]) == 3
                assert len({channel["id"] for channel in payload["channels"]}) == 3

                for channel in payload["channels"]:
                    assert re.fullmatch(r"[0-9a-f]{32}", channel["id"])
                    assert channel["title"] == "Example News"
                    assert channel["group_title"] == "News"
                    assert channel["live_url"].startswith("http://%s/" % custom_host)
                    assert "r2h-token=native-secret" in channel["live_url"]
                    assert channel["catchup"]["url"].startswith("http://%s/" % custom_host)
                    assert "r2h-refplayer-source=" in channel["catchup"]["url"]
                    assert channel["catchup"]["retention_seconds"] == 7 * 86400

                status, _, ready_body = http_get(
                    "127.0.0.1", request_port, "/api/refplayer/v1/ready?r2h-token=native-secret"
                )
                assert status == 200
                assert json.loads(ready_body) == {
                    "schema_version": 1,
                    "ready": True,
                    "catalog_channel_count": 3,
                }
                return {channel["id"] for channel in payload["channels"]}
            finally:
                process.stop()

        first_channel_ids = read_ids(entries, port)
        second_port = find_free_port()
        second_channel_ids = read_ids(list(reversed(entries)), second_port)
        assert second_channel_ids == first_channel_ids

    def test_external_playlist_readiness_and_catalog(self, r2h_binary):
        playlist = b"""#EXTM3U
#EXTINF:-1 group-title="External",External Channel
rtp://239.9.9.9:1234
"""
        upstream = MockHTTPUpstream(routes={"/channels.m3u": {"status": 200, "body": playlist}})
        upstream.start()
        port = find_free_port()
        config = _base_config(
            port,
            "",
            global_lines="external-m3u = http://127.0.0.1:%d/channels.m3u" % upstream.port,
        )
        process = R2HProcess(r2h_binary, port, config_content=config)
        try:
            process.start()
            deadline = time.monotonic() + 5
            last_status = None
            while time.monotonic() < deadline:
                last_status, _, body = http_get("127.0.0.1", port, "/api/refplayer/v1/ready")
                if last_status == 200:
                    assert json.loads(body)["ready"] is True
                    break
                assert last_status == 503
                time.sleep(0.05)
            assert last_status == 200
            assert _catalog(port)["channels"][0]["title"] == "External Channel"
        finally:
            process.stop()
            upstream.stop()


class TestRefPlayerCatchup:
    def test_epoch_range_expands_path_and_query_templates(self, r2h_binary):
        routes = {
            "/path/20240101120000/20240101130000/stream.ts": {"status": 200, "body": b"path-ok"},
            "/query": {"status": 200, "body": b"query-ok"},
            "/offset": {"status": 200, "body": b"offset-ok"},
        }
        upstream = MockHTTPUpstream(routes=routes)
        upstream.start()
        port = find_free_port()
        services = f"""#EXTM3U
#EXTINF:-1 catchup="default" catchup-source="http://127.0.0.1:{upstream.port}/path/${{(b)yyyyMMddHHmmss}}/${{(e)yyyyMMddHHmmss}}/stream.ts",Path Template
rtp://239.1.1.1:1234
#EXTINF:-1 catchup="default" catchup-source="http://127.0.0.1:{upstream.port}/query?playseek=${{(b)yyyyMMddHHmmss}}-${{(e)yyyyMMddHHmmss}}",Seek Template
rtp://239.1.1.2:1234
#EXTINF:-1 catchup="default" catchup-source="http://127.0.0.1:{upstream.port}/offset?starttime=${{(b)yyyyMMdd|UTC}}T${{(b)HHmmss|UTC}}&endtime=${{(e)yyyyMMdd|UTC}}T${{(e)HHmmss|UTC}}&r2h-seek-offset=3600",Offset Template
rtp://239.1.1.3:1234
"""
        process = R2HProcess(r2h_binary, port, config_content=_base_config(port, services))
        try:
            process.start()
            channels = {channel["title"]: channel for channel in _catalog(port)["channels"]}

            for title, expected_body in [
                ("Path Template", b"path-ok"),
                ("Seek Template", b"query-ok"),
                ("Offset Template", b"offset-ok"),
            ]:
                request_url = _append_range(channels[title]["catchup"]["url"])
                status, _, body = http_get("127.0.0.1", port, _request_path(request_url))
                assert status == 200
                assert body == expected_body

            paths = [request["path"] for request in upstream.requests_log]
            assert "/path/20240101120000/20240101130000/stream.ts" in paths
            assert any(path.startswith("/query?") and "playseek=20240101120000-20240101130000" in path for path in paths)
            assert any(
                path.startswith("/offset?")
                and "starttime=20240101T130000" in path
                and "endtime=20240101T140000" in path
                for path in paths
            )
        finally:
            process.stop()
            upstream.stop()

    def test_invalid_or_stale_catchup_contract_is_rejected(self, r2h_binary):
        port = find_free_port()
        services = """#EXTM3U
#EXTINF:-1 catchup="default" catchup-source="http://127.0.0.1:9/archive/${(b)yyyyMMddHHmmss}.ts",Validation Channel
rtp://239.1.1.1:1234
"""
        process = R2HProcess(r2h_binary, port, config_content=_base_config(port, services))
        try:
            process.start()
            catchup_url = _catalog(port)["channels"][0]["catchup"]["url"]
            catchup_path = _request_path(catchup_url)

            status, _, _ = http_get(
                "127.0.0.1", port, catchup_path + "&r2h-refplayer-start=%d" % _BEGIN_EPOCH
            )
            assert status == 400

            status, _, _ = http_get(
                "127.0.0.1",
                port,
                catchup_path
                + "&r2h-refplayer-start=%d&r2h-refplayer-end=%d" % (_END_EPOCH, _BEGIN_EPOCH),
            )
            assert status == 400

            stale_path = re.sub(r"r2h-refplayer-source=[0-9a-f]{32}", "r2h-refplayer-source=" + "0" * 32, catchup_path)
            status, _, _ = http_get(
                "127.0.0.1",
                port,
                stale_path
                + "&r2h-refplayer-start=%d&r2h-refplayer-end=%d" % (_BEGIN_EPOCH, _END_EPOCH),
            )
            assert status == 404
        finally:
            process.stop()
