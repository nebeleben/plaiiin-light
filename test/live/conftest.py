"""Live tests against a real lamp.

Requirements: pytest, requests, websocket-client.

Set LAMP_HOST to an UNPAIRED test lamp (e.g. LAMP_HOST=192.168.178.195).
The session fixture pairs it, hands the tests an admin client, and always
unpairs again on teardown (which also drops every share key), so the lamp is
left unpaired even when a test fails.
"""
import os

import pytest
import requests

TIMEOUT = 10


class LampClient:
    def __init__(self, host, admin_token):
        self.base = f"http://{host}"
        self.admin_token = admin_token

    def request(self, method, path, token=None, json=None):
        tok = self.admin_token if token is None else token
        headers = {"Authorization": f"Bearer {tok}"} if tok else {}
        return requests.request(method, self.base + path, headers=headers,
                                json=json, timeout=TIMEOUT)

    def get(self, path, token=None):
        return self.request("GET", path, token=token)

    def post(self, path, token=None, json=None):
        return self.request("POST", path, token=token, json=json)

    def put(self, path, token=None, json=None):
        return self.request("PUT", path, token=token, json=json)

    def patch(self, path, token=None, json=None):
        return self.request("PATCH", path, token=token, json=json)

    def delete(self, path, token=None):
        return self.request("DELETE", path, token=token)


@pytest.fixture(scope="session")
def lamp():
    host = os.environ.get("LAMP_HOST")
    if not host:
        pytest.skip("LAMP_HOST not set")
    base = f"http://{host}"
    state = requests.get(base + "/api/pair", timeout=TIMEOUT).json()
    assert state.get("mode") == "unpaired", f"test lamp must be unpaired, got {state}"
    r = requests.post(base + "/api/pair", timeout=TIMEOUT)
    r.raise_for_status()
    token = r.json()["token"]
    try:
        yield LampClient(host, token)
    finally:
        r = requests.delete(base + "/api/pair",
                            headers={"Authorization": f"Bearer {token}"},
                            timeout=TIMEOUT)
        assert r.ok, f"unpair failed: {r.status_code} {r.text}"
