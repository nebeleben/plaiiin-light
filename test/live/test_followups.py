"""Follow-ups to the AI share key: page meta never upgrades a share key to the
admin token, the read-only `swarm` permission, and the key-list size limit."""


def _make_key(lamp, **body):
    r = lamp.post("/api/keys", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def _delete_all_keys(lamp):
    for k in lamp.get("/api/keys").json()["keys"]:
        lamp.delete(f"/api/keys/{k['id']}")


def test_share_key_page_load_gets_no_admin_token(lamp):
    k = _make_key(lamp, role="creator", label="meta-test")
    try:
        page = lamp.get("/control", token=k["key"]).text
        assert lamp.admin_token not in page
        assert '<meta name="plk-token"' not in page
        admin_page = lamp.get("/control").text
        assert lamp.admin_token in admin_page
    finally:
        lamp.delete(f"/api/keys/{k['id']}")


def test_ai_key_page_load_gets_no_admin_token(lamp):
    k = _make_key(lamp, role="ai", label="meta-ai", perms=["control"])
    try:
        assert lamp.admin_token not in lamp.get("/control", token=k["key"]).text
    finally:
        lamp.delete(f"/api/keys/{k['id']}")


def test_swarm_perm_reads_membership(lamp):
    k = _make_key(lamp, role="ai", label="swarm-read", perms=["swarm"])
    try:
        r = lamp.get("/api/swarm", token=k["key"])
        assert r.status_code == 200, r.text
        body = r.json()
        assert set(body) >= {"member", "id", "enabled", "channel"}
        assert "key" not in body
        assert lamp.get("/api/swarm/stats", token=k["key"]).status_code == 200
        # Read only: managing the swarm stays admin-only.
        assert lamp.post("/api/swarm/ping", token=k["key"]).status_code == 403
        assert lamp.post("/api/swarm/enable", token=k["key"], json={"enabled": True}).status_code == 403
        assert "swarm" in lamp.get("/api/whoami", token=k["key"]).json()["perms"]
    finally:
        lamp.delete(f"/api/keys/{k['id']}")


def test_swarm_read_needs_swarm_perm(lamp):
    k = _make_key(lamp, role="ai", label="no-swarm", perms=["control"])
    try:
        assert lamp.get("/api/swarm", token=k["key"]).status_code == 403
        assert lamp.get("/api/swarm/stats", token=k["key"]).status_code == 403
    finally:
        lamp.delete(f"/api/keys/{k['id']}")


def test_key_list_capped_at_20(lamp):
    _delete_all_keys(lamp)
    try:
        for i in range(20):
            _make_key(lamp, role="user", label=f"k{i}")
        r = lamp.post("/api/keys", json={"role": "user", "label": "k20"})
        assert r.status_code == 400
        assert "share-key limit reached" in r.text
    finally:
        _delete_all_keys(lamp)


def test_key_list_too_big_for_storage_is_400(lamp):
    # Labels of 32 quotes serialize to 64 bytes each; 20 such entries pass
    # the NVS string limit, which must be refused cleanly, not with a 500.
    _delete_all_keys(lamp)
    try:
        statuses = []
        for _ in range(20):
            r = lamp.post("/api/keys", json={"role": "creator", "label": '"' * 32})
            statuses.append(r.status_code)
            if r.status_code != 200:
                assert r.status_code == 400, r.text
                assert "share-key list full" in r.text
                break
        assert 400 in statuses
        assert isinstance(lamp.get("/api/keys").json()["keys"], list)
    finally:
        _delete_all_keys(lamp)
