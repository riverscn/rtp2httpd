"""Native RefPlayer catalog and epoch-based catchup API coverage."""

from __future__ import annotations

import json
import os
import re
import time
from urllib.parse import urlsplit

import pytest

from helpers import MockHTTPUpstream, MockRTSPServer, R2HProcess, find_free_port, http_get, http_request, stream_get
from helpers.rtp import TS_NULL_PACKET


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
                assert payload["schema_version"] == 2
                assert isinstance(payload["helper_version"], str) and payload["helper_version"]
                assert len(payload["channels"]) == 3
                assert len({channel["id"] for channel in payload["channels"]}) == 3

                for channel in payload["channels"]:
                    assert re.fullmatch(r"[0-9a-f]{32}", channel["id"])
                    assert channel["client_source_id"] is None
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
                    "schema_version": 2,
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
#EXTINF:-1 group-title="External" refplayer-source-id="external-must-not-be-trusted",External Channel
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
            channel = _catalog(port)["channels"][0]
            assert channel["title"] == "External Channel"
            assert channel["client_source_id"] is None
        finally:
            process.stop()
            upstream.stop()


class TestRefPlayerRTSPTimeshift:
    def test_open_clock_source_publishes_bounded_coordinate_capability(self, r2h_binary):
        """clock=0- proves seek coordinates, not server retention."""
        target = int(time.time()) - 3600
        timezone_offset = 8 * 3600
        wire_clock = time.strftime(
            "%Y%m%dT%H%M%SZ",
            time.gmtime(target + timezone_offset),
        )
        acknowledged_clock = wire_clock[:-1] + ".32Z"
        sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=live\r\nt=0 0\r\na=range:clock=0-\r\nm=video 0 MP2T/AVP 33\r\n"
        rtsp = MockRTSPServer(
            num_packets=500,
            custom_sdp=sdp,
            server_header="HMS_V1R2",
            timeshift_status=1,
            play_response_headers_sequence=[
                [],
                [("Range", f"clock={acknowledged_clock}-")],
            ],
        )
        rtsp.start()
        port = find_free_port()
        token = "10101010101010101010101010101010"
        services = f"""#EXTM3U
#EXTINF:-1,Open Clock
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        process = R2HProcess(
            r2h_binary,
            port,
            config_content=_base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}"),
            env=env,
        )
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            status, _, body = stream_get(
                "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
            )
            assert status == 200 and body
            status, _, body = http_get(
                "127.0.0.1",
                port,
                f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}",
            )
            assert status == 200
            capability = json.loads(body)["capability"]
            assert capability["range"] == {
                "kind": "clock",
                "open_ended": True,
                "maximum_lookback_seconds": 7 * 86_400,
            }
            assert abs(capability["observed_at_epoch"] - int(time.time())) <= 5

            resolve_path = (
                f"/api/refplayer/v1/rtsp-timeshift/resolve?r2h-token={token}"
                f"&source_id={channel['id']}&observation_id={capability['observation_id']}"
                f"&kind=clock&target_epoch={target}&timezone_offset_seconds={timezone_offset}"
            )
            status, _, body = http_get("127.0.0.1", port, resolve_path)
            assert status == 200
            media_url = json.loads(body)["media_url"]
            assert f"r2h-refplayer-rtsp-timezone-offset={timezone_offset}" in media_url
            status, _, body = stream_get("127.0.0.1", port, _request_path(media_url), read_bytes=4096, timeout=15)
            assert status == 200 and body
            play_requests = [request for request in rtsp.requests_detailed if request["method"] == "PLAY"]
            assert play_requests[-1]["headers"]["Range"] == f"clock={wire_clock}-"
        finally:
            process.stop()
            rtsp.stop()

    def test_open_clock_archive_learns_a_safe_forward_clamp(self, r2h_binary):
        now = int(time.time())
        requested_actual = now - 6 * 3600
        clamped_actual = now - 4 * 3600
        timezone_offset = 8 * 3600
        clamped_wire = time.strftime(
            "%Y%m%dT%H%M%SZ",
            time.gmtime(clamped_actual + timezone_offset),
        )
        sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=live\r\nt=0 0\r\na=range:clock=0-\r\nm=video 0 MP2T/AVP 33\r\n"
        rtsp = MockRTSPServer(
            num_packets=500,
            custom_sdp=sdp,
            play_response_headers_sequence=[
                [],
                [("Range", f"clock={clamped_wire}-")],
            ],
        )
        rtsp.start()
        port = find_free_port()
        token = "12121212121212121212121212121212"
        services = f"""#EXTM3U
#EXTINF:-1,Clamped Clock
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        process = R2HProcess(
            r2h_binary,
            port,
            config_content=_base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}"),
            env=env,
        )
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            status, _, body = stream_get(
                "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
            )
            assert status == 200 and body
            discover_path = f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}"
            status, _, body = http_get("127.0.0.1", port, discover_path)
            capability = json.loads(body)["capability"]
            assert status == 200 and capability["range"]["open_ended"] is True
            resolve_path = (
                f"/api/refplayer/v1/rtsp-timeshift/resolve?r2h-token={token}"
                f"&source_id={channel['id']}&observation_id={capability['observation_id']}"
                f"&kind=clock&target_epoch={requested_actual}&timezone_offset_seconds={timezone_offset}"
            )
            status, _, body = http_get("127.0.0.1", port, resolve_path)
            assert status == 200
            status, _, archive_body = stream_get(
                "127.0.0.1",
                port,
                _request_path(json.loads(body)["media_url"]),
                read_bytes=4096,
                timeout=15,
            )
            assert status == 200 and archive_body

            deadline = time.monotonic() + 2
            learned = None
            while time.monotonic() < deadline:
                status, _, body = http_get("127.0.0.1", port, discover_path)
                learned = json.loads(body)["capability"]
                if learned and learned["range"].get("open_ended") is not True:
                    break
                time.sleep(0.02)
            assert status == 200
            assert learned["range"] == {
                "kind": "clock",
                "start_epoch": clamped_actual,
                "end_epoch": capability["observed_at_epoch"],
            }
        finally:
            process.stop()
            rtsp.stop()

    @pytest.mark.parametrize("ack_delta", [-(3 * 3600), 3600])
    def test_open_clock_archive_rejects_unsafe_clamps(self, r2h_binary, ack_delta):
        now = int(time.time())
        requested_actual = now - 2 * 3600
        timezone_offset = 8 * 3600
        acknowledged_wire = time.strftime(
            "%Y%m%dT%H%M%SZ",
            time.gmtime(now + ack_delta + timezone_offset),
        )
        sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=live\r\nt=0 0\r\na=range:clock=0-\r\nm=video 0 MP2T/AVP 33\r\n"
        rtsp = MockRTSPServer(
            num_packets=500,
            custom_sdp=sdp,
            play_response_headers_sequence=[
                [],
                [("Range", f"clock={acknowledged_wire}-")],
            ],
        )
        rtsp.start()
        port = find_free_port()
        token = f"{ack_delta & ((1 << 128) - 1):032x}"
        services = f"""#EXTM3U
#EXTINF:-1,Unsafe Clock Clamp
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        process = R2HProcess(
            r2h_binary,
            port,
            config_content=_base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}"),
            env=env,
        )
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            status, _, body = stream_get(
                "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
            )
            assert status == 200 and body
            discover_path = f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}"
            status, _, body = http_get("127.0.0.1", port, discover_path)
            capability = json.loads(body)["capability"]
            resolve_path = (
                f"/api/refplayer/v1/rtsp-timeshift/resolve?r2h-token={token}"
                f"&source_id={channel['id']}&observation_id={capability['observation_id']}"
                f"&kind=clock&target_epoch={requested_actual}&timezone_offset_seconds={timezone_offset}"
            )
            status, _, body = http_get("127.0.0.1", port, resolve_path)
            assert status == 200
            status, _, _ = stream_get(
                "127.0.0.1",
                port,
                _request_path(json.loads(body)["media_url"]),
                read_bytes=4096,
                timeout=15,
            )
            assert status != 200
        finally:
            process.stop()
            rtsp.stop()

    def test_header_queue_failure_never_commits_or_publishes_capability(self, r2h_binary):
        rtsp = MockRTSPServer(
            num_packets=8,
            media_payload=TS_NULL_PACKET * 7,
            play_response_headers=[("Range", "npt=0-600")],
        )
        rtsp.start()
        port = find_free_port()
        token = "feedfacefeedfacefeedfacefeedface"
        services = f"""#EXTM3U
#EXTINF:-1,Header Queue Failure
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        env["RTP2HTTPD_REFPLAYER_TEST_FAIL_RTSP_HEADER_QUEUE"] = "1"
        process = R2HProcess(
            r2h_binary,
            port,
            config_content=_base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}"),
            env=env,
        )
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            status, _, _ = stream_get(
                "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
            )
            assert status != 200
            status, _, body = http_get(
                "127.0.0.1",
                port,
                f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}",
            )
            assert status == 200
            assert json.loads(body)["capability"] is None
        finally:
            process.stop()
            rtsp.stop()

    @pytest.mark.parametrize("packet_count,expects_commit", [(1, False), (8, True)])
    def test_capability_requires_actual_http_media_commit(self, r2h_binary, packet_count, expects_commit):
        rtsp = MockRTSPServer(
            num_packets=packet_count,
            media_payload=TS_NULL_PACKET * 7,
            play_response_headers=[("Range", "npt=0-600")],
        )
        rtsp.start()
        port = find_free_port()
        token = f"{packet_count:032x}"
        services = f"""#EXTM3U
#EXTINF:-1,Commit Gate {packet_count}
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        process = R2HProcess(
            r2h_binary,
            port,
            config_content=_base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}"),
            env=env,
        )
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            status, _, _ = stream_get(
                "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
            )
            assert (status == 200) == expects_commit
            status, _, body = http_get(
                "127.0.0.1",
                port,
                f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}",
            )
            assert status == 200
            capability = json.loads(body)["capability"]
            assert (capability is not None) == expects_commit
        finally:
            process.stop()
            rtsp.stop()

    def test_live_discover_resolve_and_archive_play_range(self, r2h_binary):
        rtsp = MockRTSPServer(
            num_packets=500,
            play_response_headers_sequence=[
                [("Range", "npt=0.000-600.000")],
                [("Range", "npt=300.000-")],
            ],
        )
        rtsp.start()
        port = find_free_port()
        token = "0123456789abcdef0123456789abcdef"
        services = f"""#EXTM3U
#EXTINF:-1 refplayer-source-id="11111111111111111111111111111111",Timeshift Channel
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        config = _base_config(
            port,
            services,
            global_lines=f"workers = 1\nr2h-token = {token}",
        )
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        process = R2HProcess(r2h_binary, port, config_content=config, env=env)
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            live_path = _request_path(channel["live_url"])
            status, _, body = stream_get("127.0.0.1", port, live_path, read_bytes=4096, timeout=15)
            assert status == 200
            assert body

            discover_path = f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}"
            status, headers, body = http_get("127.0.0.1", port, discover_path)
            assert status == 200
            assert headers["Cache-Control"] == "no-store"
            capability = json.loads(body)["capability"]
            assert re.fullmatch(r"[0-9a-f]{32}", capability["observation_id"])
            assert capability["range"] == {
                "kind": "npt",
                "start_seconds": 0,
                "end_seconds": 600,
            }
            # The live RTSP connection has already closed. Discovery must
            # still mint a fresh bounded grant from the capability learned by
            # that successful open so a later archive seek does not become the
            # last usable seek for this helper lease.
            status, _, refreshed_body = http_get(
                "127.0.0.1", port, discover_path
            )
            refreshed = json.loads(refreshed_body)["capability"]
            assert status == 200
            assert refreshed["observation_id"] != capability["observation_id"]
            assert refreshed["range"] == capability["range"]
            capability = refreshed

            resolve_path = (
                f"/api/refplayer/v1/rtsp-timeshift/resolve?r2h-token={token}"
                f"&source_id={channel['id']}&observation_id={capability['observation_id']}"
                "&kind=npt&target_seconds=300"
            )
            status, _, body = http_get("127.0.0.1", port, resolve_path)
            assert status == 200
            media_url = json.loads(body)["media_url"]
            assert urlsplit(media_url).path == urlsplit(channel["live_url"]).path

            status, _, _ = http_get(
                "127.0.0.1",
                port,
                resolve_path.replace("target_seconds=300", "target_seconds=601"),
            )
            assert status == 416
            status, _, _ = http_get(
                "127.0.0.1",
                port,
                discover_path + f"&source_id={channel['id']}",
            )
            assert status == 400
            status, _, _ = http_get(
                "127.0.0.1",
                port,
                discover_path.replace("source_id=", "Source_ID="),
            )
            assert status == 400

            status, _, body = stream_get("127.0.0.1", port, _request_path(media_url), read_bytes=4096, timeout=15)
            assert status == 200
            assert body
            play_requests = [request for request in rtsp.requests_detailed if request["method"] == "PLAY"]
            assert len(play_requests) == 2
            assert "Range" not in play_requests[0]["headers"]
            assert play_requests[1]["headers"]["Range"] == "npt=300-"
            assert all("r2h-refplayer" not in request["uri"].lower() for request in rtsp.requests_detailed)
            assert all(token not in request["uri"] for request in rtsp.requests_detailed)
        finally:
            process.stop()
            rtsp.stop()

    def test_clock_window_discovers_resolves_and_archives(self, r2h_binary):
        rtsp = MockRTSPServer(
            num_packets=500,
            play_response_headers=[
                ("Range", "clock=20240101T120000Z-20240101T130000Z"),
            ],
        )
        rtsp.start()
        port = find_free_port()
        token = "33333333333333333333333333333333"
        services = f"""#EXTM3U
#EXTINF:-1,Clock Window
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        process = R2HProcess(
            r2h_binary,
            port,
            config_content=_base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}"),
            env=env,
        )
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            status, _, body = stream_get(
                "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
            )
            assert status == 200 and body
            status, _, body = http_get(
                "127.0.0.1",
                port,
                f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}",
            )
            capability = json.loads(body)["capability"]
            assert status == 200
            assert capability["range"] == {
                "kind": "clock",
                "start_epoch": _BEGIN_EPOCH,
                "end_epoch": _END_EPOCH,
            }
            status, _, body = http_get(
                "127.0.0.1",
                port,
                f"/api/refplayer/v1/rtsp-timeshift/resolve?r2h-token={token}"
                f"&source_id={channel['id']}&observation_id={capability['observation_id']}"
                f"&kind=clock&target_epoch={_BEGIN_EPOCH + 1800}",
            )
            assert status == 200
            status, _, body = stream_get(
                "127.0.0.1",
                port,
                _request_path(json.loads(body)["media_url"]),
                read_bytes=4096,
                timeout=15,
            )
            assert status == 200 and body
            play_requests = [request for request in rtsp.requests_detailed if request["method"] == "PLAY"]
            assert play_requests[-1]["headers"]["Range"] == "clock=20240101T123000Z-"
        finally:
            process.stop()
            rtsp.stop()

    def test_finite_sdp_fallback_requires_successful_scale_one_play_and_media(self, r2h_binary):
        cases = [
            ([], "a=range:npt=10.000-20.000\r\n", {"kind": "npt", "start_seconds": 10, "end_seconds": 20}),
            (
                [("Range", "npt=10.000-")],
                "a=range:npt=10-20\r\n",
                {"kind": "npt", "start_seconds": 10, "end_seconds": 20},
            ),
            ([("Range", "npt=not-a-time-")], "a=range:npt=10-20\r\n", None),
            ([], "a=range:npt=10-\r\n", None),
            ([("Scale", "2")], "a=range:npt=10-20\r\n", None),
            ([("Scale", "invalid")], "a=range:npt=10-20\r\n", None),
            (
                [("Range", "npt=10-20"), ("Range", "npt=11-19")],
                "a=range:npt=10-20\r\n",
                None,
            ),
        ]
        for index, (play_headers, range_line, expected) in enumerate(cases):
            sdp = (
                "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=T\r\nt=0 0\r\n"
                "m=video 0 RTP/AVP 33\r\n"
                f"{range_line}a=control:*\r\n"
            )
            rtsp = MockRTSPServer(num_packets=500, custom_sdp=sdp, play_response_headers=play_headers)
            rtsp.start()
            port = find_free_port()
            token = f"{index + 4:032x}"
            services = f"""#EXTM3U
#EXTINF:-1,SDP Window {index}
rtsp://127.0.0.1:{rtsp.port}/stream
"""
            env = os.environ.copy()
            env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
            process = R2HProcess(
                r2h_binary,
                port,
                config_content=_base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}"),
                env=env,
            )
            try:
                process.start()
                channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
                status, _, body = stream_get(
                    "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
                )
                assert status == 200 and body
                status, _, body = http_get(
                    "127.0.0.1",
                    port,
                    f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}",
                )
                assert status == 200
                capability = json.loads(body)["capability"]
                assert (capability and capability["range"]) == expected if expected else capability is None
            finally:
                process.stop()
                rtsp.stop()

    def test_no_window_and_private_query_aliases_fail_closed_without_upstream(self, r2h_binary):
        rtsp = MockRTSPServer(num_packets=500)
        rtsp.start()
        port = find_free_port()
        token = "22222222222222222222222222222222"
        services = f"""#EXTM3U
#EXTINF:-1,No Window
rtsp://127.0.0.1:{rtsp.port}/stream
"""
        config = _base_config(port, services, global_lines=f"workers = 1\nr2h-token = {token}")
        env = os.environ.copy()
        env["RTP2HTTPD_REFPLAYER_TIMESHIFT"] = "1"
        process = R2HProcess(r2h_binary, port, config_content=config, capture_log=True, env=env)
        try:
            process.start()
            channel = _catalog(port, f"/api/refplayer/v1/catalog?r2h-token={token}")["channels"][0]
            status, _, body = stream_get(
                "127.0.0.1", port, _request_path(channel["live_url"]), read_bytes=4096, timeout=15
            )
            assert status == 200 and body
            status, _, body = http_get(
                "127.0.0.1",
                port,
                f"/api/refplayer/v1/rtsp-timeshift?r2h-token={token}&source_id={channel['id']}",
            )
            assert status == 200
            assert json.loads(body)["capability"] is None

            baseline = len(rtsp.requests_received)
            encoded_name = "%72%32%68-refplayer-rtsp-source"
            status, _, _ = http_request(
                "127.0.0.1",
                port,
                "GET",
                _request_path(channel["live_url"]) + f"&{encoded_name}={'a' * 32}",
            )
            assert status == 400
            long_name = "r2h-refplayer-rtsp-source" + ("x" * 160)
            status, _, _ = http_request(
                "127.0.0.1",
                port,
                "GET",
                _request_path(channel["live_url"]) + f"&{long_name}=value",
            )
            assert status == 400
            assert len(rtsp.requests_received) == baseline

            observation = "abcdefabcdefabcdefabcdefabcdefab"
            target = "123456789"
            http_get(
                "127.0.0.1",
                port,
                f"/api/refplayer/v1/rtsp-timeshift/resolve?r2h-token={token}"
                f"&source_id={channel['id']}&observation_id={observation}"
                f"&kind=clock&target_epoch={target}",
            )
            time.sleep(0.05)
            log = process.read_log()
            assert token not in log
            assert observation not in log
            assert target not in log
        finally:
            process.stop()
            rtsp.stop()

    def test_private_inline_snapshot_round_trips_client_source_id(self, r2h_binary):
        client_source_id = "0123456789abcdef0123456789abcdef"
        port = find_free_port()
        services = f"""#EXTM3U
#EXTINF:-1 refplayer-source-id="{client_source_id}",refplayer-rtsp
rtsp://127.0.0.1:9/live
"""
        process = R2HProcess(r2h_binary, port, config_content=_base_config(port, services))
        try:
            process.start()
            channel = _catalog(port)["channels"][0]
            assert channel["client_source_id"] == client_source_id
            assert channel["id"] != client_source_id
        finally:
            process.stop()


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
            assert any(
                path.startswith("/query?") and "playseek=20240101120000-20240101130000" in path for path in paths
            )
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

            status, _, _ = http_get("127.0.0.1", port, catchup_path + "&r2h-refplayer-start=%d" % _BEGIN_EPOCH)
            assert status == 400

            status, _, _ = http_get(
                "127.0.0.1",
                port,
                catchup_path + "&r2h-refplayer-start=%d&r2h-refplayer-end=%d" % (_END_EPOCH, _BEGIN_EPOCH),
            )
            assert status == 400

            stale_path = re.sub(r"r2h-refplayer-source=[0-9a-f]{32}", "r2h-refplayer-source=" + "0" * 32, catchup_path)
            status, _, _ = http_get(
                "127.0.0.1",
                port,
                stale_path + "&r2h-refplayer-start=%d&r2h-refplayer-end=%d" % (_BEGIN_EPOCH, _END_EPOCH),
            )
            assert status == 404
        finally:
            process.stop()
