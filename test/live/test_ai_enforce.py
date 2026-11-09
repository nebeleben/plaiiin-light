"""AI share keys are enforced: permission mask, only-while-on, brightness cap,
whoami (Task 3)."""
import contextlib
import time

import pytest
import requests
import websocket

ALL_PERMS = ["control", "effects", "images", "scenes", "stream", "ota", "rename"]

# Smallest valid shade() script for PUT /api/js/<name> (raw JS body).
TINY_SCRIPT = "function shade(x, y, idx, frame, base, params) {\n  emitBright(1);\n}\n"


@contextlib.contextmanager
def ai_key(lamp, perms, **extra):
    body = {"role": "ai", "label": "enforce-test", "perms": perms, **extra}
    r = lamp.post("/api/keys", json=body)
    assert r.status_code == 200, r.text
    e = r.json()
    try:
        yield e["key"], e["id"]
    finally:
        lamp.delete(f"/api/keys/{e['id']}")


def put_script(lamp, name, token):
    return requests.put(f"{lamp.base}/api/js/{name}", data=TINY_SCRIPT.encode(),
                        headers={"Authorization": f"Bearer {token}",
                                 "Content-Type": "text/plain"},
                        timeout=10)


def post_raw(lamp, path, token, data):
    return requests.post(f"{lamp.base}{path}", data=data,
                         headers={"Authorization": f"Bearer {token}",
                                  "Content-Type": "application/octet-stream"},
                         timeout=10)


def brightness(lamp):
    return lamp.get("/api/state").json()["brightness"]


@pytest.fixture(scope="module", autouse=True)
def restore_light(lamp):
    before = lamp.get("/api/state").json()
    yield
    lamp.post("/api/brightness", json={"brightness": before["brightness"]})
    lamp.post("/api/power", json={"on": True})


def test_ai_key_cannot_manage_keys(lamp):
    with ai_key(lamp, ALL_PERMS) as (tok, kid):
        r = lamp.post("/api/keys", token=tok, json={"role": "user", "label": "x"})
        assert r.status_code == 403, r.text
        r = lamp.patch(f"/api/keys/{kid}", token=tok, json={"perms": ["control"]})
        assert r.status_code == 403, r.text
        r = lamp.delete("/api/pair", token=tok)
        assert r.status_code == 403, r.text
        assert lamp.get("/api/pair").json()["mode"] == "paired"


def test_whoami_reports_ai_perms(lamp):
    with ai_key(lamp, ["control"]) as (tok, _):
        r = lamp.get("/api/whoami", token=tok)
        assert r.status_code == 200, r.text
        w = r.json()
        assert w["role"] == "ai"
        assert w["paired"] is True
        assert w["perms"] == ["control"]
        assert w["maxBrightness"] is None
        assert w["onlyWhenOn"] is False


def test_control_only_key(lamp):
    with ai_key(lamp, ["control"]) as (tok, _):
        r = lamp.post("/api/power", token=tok, json={"on": True})
        assert r.status_code == 200, r.text
        try:
            r = put_script(lamp, "t1", tok)
            assert r.status_code == 403, r.text
        finally:
            lamp.delete("/api/js/t1")
        r = lamp.post("/api/images", token=tok, json={"name": "x"})
        assert r.status_code == 403, r.text
        assert lamp.get("/api/state", token=tok).status_code == 200
        assert lamp.get("/api/keys", token=tok).status_code == 403
        r = lamp.post("/api/limits", token=tok, json={"maxBrightness": 255})
        assert r.status_code == 403, r.text


def test_effects_key_can_upload_and_delete(lamp):
    with ai_key(lamp, ["effects"]) as (tok, _):
        try:
            r = put_script(lamp, "t1", tok)
            assert r.status_code == 200, r.text
            r = lamp.delete("/api/js/t1", token=tok)
            assert r.status_code == 200, r.text
        finally:
            lamp.delete("/api/js/t1")
        r = lamp.post("/api/power", token=tok, json={"on": True})
        assert r.status_code == 403, r.text


def test_ota_needs_ota_perm(lamp):
    with ai_key(lamp, ["control"]) as (tok, _):
        r = post_raw(lamp, "/api/ota", tok, b"\x00" * 16)
        assert r.status_code == 403, r.text


def test_revoked_key_is_401_immediately(lamp):
    with ai_key(lamp, ["control"]) as (tok, kid):
        assert lamp.get("/api/state", token=tok).status_code == 200
        r = lamp.post(f"/api/keys/{kid}/revoke")
        assert r.status_code == 200, r.text
        assert lamp.get("/api/state", token=tok).status_code == 401
    with ai_key(lamp, ["control"]) as (tok, kid):
        assert lamp.get("/api/state", token=tok).status_code == 200
        r = lamp.delete(f"/api/keys/{kid}")
        assert r.status_code == 200, r.text
        assert lamp.get("/api/state", token=tok).status_code == 401


def test_only_when_on(lamp):
    with ai_key(lamp, ["control", "scenes"], onlyWhenOn=True) as (tok, _):
        r = lamp.post("/api/power", json={"on": False})
        assert r.status_code == 200, r.text
        assert lamp.get("/api/state", token=tok).status_code == 200
        r = lamp.post("/api/color", token=tok, json={"color": [255, 0, 0]})
        assert r.status_code == 403, r.text
        assert "lamp is off" in r.text
        r = lamp.post("/api/power", token=tok, json={"on": True})
        assert r.status_code == 200, r.text
        r = lamp.post("/api/color", token=tok, json={"color": [255, 0, 0]})
        assert r.status_code == 200, r.text


def test_brightness_cap(lamp):
    with ai_key(lamp, ["control"], maxBrightness=100) as (tok, _):
        r = lamp.post("/api/brightness", token=tok, json={"brightness": 250})
        assert r.status_code == 200, r.text
        assert r.json()["brightness"] == 100
        assert brightness(lamp) == 100


def test_cap_not_sticky(lamp):
    with ai_key(lamp, ["control"], maxBrightness=100) as (tok, _):
        r = lamp.post("/api/brightness", token=tok, json={"brightness": 250})
        assert r.status_code == 200, r.text
        r = lamp.post("/api/brightness", json={"brightness": 250})
        assert r.status_code == 200, r.text
        assert brightness(lamp) == 250


def _ws_url(lamp, tok):
    return lamp.base.replace("http://", "ws://") + f"/ws?token={tok}"


def ws_usable(url):
    """True if the upgrade completes AND the socket answers a ping.

    ESP-IDF's httpd sends the 101 before the /ws handler runs, so a refused
    key shows up as a completed handshake followed by the server writing the
    403 and closing the socket — not as an HTTP 403 status."""
    try:
        ws = websocket.create_connection(url, timeout=5)
    except (websocket.WebSocketException, OSError):
        return False
    try:
        ws.ping(b"ai")
        op, _ = ws.recv_data(control_frame=True)
        return op == websocket.ABNF.OPCODE_PONG
    except (websocket.WebSocketException, OSError):
        return False
    finally:
        ws.close()


def test_ws_needs_stream_perm(lamp):
    with ai_key(lamp, ["control"]) as (tok, _):
        assert not ws_usable(_ws_url(lamp, tok))
    with ai_key(lamp, ["stream"]) as (tok, _):
        assert ws_usable(_ws_url(lamp, tok))


def test_rename_needs_rename_perm(lamp):
    with ai_key(lamp, ["control"]) as (tok, _):
        r = lamp.post("/api/name", token=tok, json={"name": "aitest-nope"})
        assert r.status_code == 403, r.text


def test_rename_validates(lamp):
    for body in ({"name": ""}, {"name": "x" * 33}, {}):
        r = lamp.post("/api/name", json=body)
        assert r.status_code == 400, (body, r.status_code, r.text)


def test_rename_rejects_quote(lamp):
    # A quote/backslash/control char would break the unescaped nodeName in GET /api.
    before = lamp.get("/api").json()["nodeName"]
    for bad in ('a"b', "a\\b", "a\nb", "a\u007fb"):
        r = lamp.post("/api/name", json={"name": bad})
        assert r.status_code == 400, (bad, r.status_code, r.text)
    time.sleep(3)  # a (wrong) accepted rename would reboot ~800 ms later
    assert lamp.get("/api").json()["nodeName"] == before


def _wait_node_name(lamp, expect, timeout=40):
    """Poll GET /api until the lamp is back and reports nodeName == expect."""
    deadline = time.time() + timeout
    last = None
    time.sleep(2)  # let the 800 ms reboot timer fire first
    while time.time() < deadline:
        try:
            last = lamp.get("/api").json().get("nodeName")
            if last == expect:
                return
        except (requests.RequestException, ValueError):
            pass
        time.sleep(1)
    raise AssertionError(f"nodeName never became {expect!r} (last {last!r})")


# Must stay last: it reboots the lamp twice.
def test_rename_with_perm(lamp):
    original = lamp.get("/api").json()["nodeName"]
    temp = "aitest-rename"
    try:
        with ai_key(lamp, ["rename"]) as (tok, _):
            r = lamp.post("/api/name", token=tok, json={"name": temp})
            assert r.status_code == 200, r.text
            assert r.json() == {"status": "ok", "rebooting": True}
        _wait_node_name(lamp, temp)
    finally:
        r = lamp.post("/api/name", json={"name": original})
        assert r.status_code == 200, r.text
        _wait_node_name(lamp, original)
