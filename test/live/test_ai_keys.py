"""AI share keys in the key store: create, list, edit (Task 2)."""

LEGACY_FIELDS = ("id", "key", "role", "label", "created", "revoked")

AI_BODY = {
    "role": "ai",
    "label": "Claude",
    "perms": ["control", "effects"],
    "maxBrightness": 180,
    "onlyWhenOn": True,
}


def _create(lamp, body):
    return lamp.post("/api/keys", json=body)


def _delete(lamp, key_id):
    lamp.delete(f"/api/keys/{key_id}")


def test_create_ai_key_renders_names(lamp):
    r = _create(lamp, AI_BODY)
    assert r.status_code == 200, r.text
    try:
        e = r.json()
        assert e["role"] == "ai"
        assert e["perms"] == ["control", "effects"]
        assert e["maxBrightness"] == 180
        assert e["onlyWhenOn"] is True
        for f in LEGACY_FIELDS:
            assert f in e, f
        assert e["label"] == "Claude"
        assert e["revoked"] is False
        for f in ("p", "mb", "ow"):
            assert f not in e
    finally:
        _delete(lamp, r.json()["id"])


def test_unknown_permission_rejected(lamp):
    r = _create(lamp, {**AI_BODY, "perms": ["control", "admin"]})
    assert r.status_code == 400
    assert "unknown permission: admin" in r.text


def test_empty_perms_rejected(lamp):
    r = _create(lamp, {**AI_BODY, "perms": []})
    assert r.status_code == 400


def test_fifth_ai_key_rejected(lamp):
    ids = []
    try:
        for i in range(4):
            r = _create(lamp, {**AI_BODY, "label": f"ai{i}"})
            assert r.status_code == 200, r.text
            ids.append(r.json()["id"])
        r = _create(lamp, {**AI_BODY, "label": "ai5"})
        assert r.status_code == 400
        assert "ai-key limit reached" in r.text
    finally:
        for i in ids:
            _delete(lamp, i)


def test_get_keys_lists_ai_entry_with_legacy_fields(lamp):
    ai = _create(lamp, AI_BODY)
    user = _create(lamp, {"role": "user", "label": "guest"})
    assert ai.status_code == 200, ai.text
    assert user.status_code == 200, user.text
    try:
        r = lamp.get("/api/keys")
        assert r.status_code == 200
        keys = r.json()["keys"]
        assert len(keys) >= 2
        for e in keys:
            for f in LEGACY_FIELDS:
                assert f in e, (f, e)
        by_id = {e["id"]: e for e in keys}
        listed = by_id[ai.json()["id"]]
        assert listed["perms"] == ["control", "effects"]
        assert listed["maxBrightness"] == 180
        assert listed["onlyWhenOn"] is True
        assert "perms" not in by_id[user.json()["id"]]
    finally:
        _delete(lamp, ai.json()["id"])
        _delete(lamp, user.json()["id"])


def test_patch_changes_perms(lamp):
    r = _create(lamp, AI_BODY)
    assert r.status_code == 200, r.text
    kid = r.json()["id"]
    try:
        p = lamp.patch(f"/api/keys/{kid}",
                       json={"perms": ["scenes"], "maxBrightness": None})
        assert p.status_code == 200, p.text
        e = p.json()
        assert e["perms"] == ["scenes"]
        assert e["maxBrightness"] is None
        assert e["onlyWhenOn"] is True
        assert e["label"] == "Claude"
        listed = {k["id"]: k for k in lamp.get("/api/keys").json()["keys"]}[kid]
        assert listed["perms"] == ["scenes"]
        assert listed["maxBrightness"] is None
    finally:
        _delete(lamp, kid)


def test_patch_rejects_creator_key(lamp):
    r = _create(lamp, {"role": "creator", "label": "friend"})
    assert r.status_code == 200, r.text
    kid = r.json()["id"]
    try:
        p = lamp.patch(f"/api/keys/{kid}", json={"perms": ["control"]})
        assert p.status_code == 400
        assert "not an ai key" in p.text
    finally:
        _delete(lamp, kid)


def test_patch_unknown_id_404(lamp):
    p = lamp.patch("/api/keys/deadbeef", json={"perms": ["control"]})
    assert p.status_code == 404
