#!/usr/bin/env bash
# Build + deploy a new universal PlaiiinLightOS release.
#
#   Usage: ./scripts/release.sh [options]
#
#   Options:
#     --bump patch|minor|major     (default: patch)
#     --no-bump                    skip version bump (rebuild + upload current)
#     --server URL                 (default: $PLAIIIN_SERVER or http://localhost:8080)
#     --user USER:PASS             (default: $PLAIIIN_ADMIN or admin:admin)
#     --description TEXT           (default: "PlaiiinLightOS <version>")
#     --dry-run                    just print what would happen
#
#   Flow:
#     1. Bump firmware.version in lampos/version.properties (patch by default).
#     2. Call scripts/build.sh (universal, no profile).
#     3. POST the resulting -<version>-app.bin to /api/v1/admin/firmware.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VERSION_FILE="$PROJECT_DIR/version.properties"

BUMP="patch"
DO_BUMP=1
SERVER="${PLAIIIN_SERVER:-http://localhost:8080}"
CREDS="${PLAIIIN_ADMIN:-admin:admin}"
DESCRIPTION=""
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --bump)         BUMP="$2"; shift 2 ;;
        --no-bump)      DO_BUMP=0; shift ;;
        --server)       SERVER="$2"; shift 2 ;;
        --user)         CREDS="$2"; shift 2 ;;
        --description)  DESCRIPTION="$2"; shift 2 ;;
        --dry-run)      DRY_RUN=1; shift ;;
        -h|--help)      sed -n '2,25p' "$0"; exit 0 ;;
        -*)             echo "unknown flag: $1" >&2; exit 2 ;;
        *)              echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# --- 1. Read + bump version.properties ---------------------------------------
read_prop() {
    awk -F= -v k="$1" '$1==k{gsub(/[ \t\r]/,"",$2);print $2;exit}' "$VERSION_FILE"
}
CURRENT_VERSION="$(read_prop firmware.version)"
if [ -z "$CURRENT_VERSION" ]; then
    echo "firmware.version missing from $VERSION_FILE" >&2
    exit 1
fi

if [ "$DO_BUMP" = 1 ]; then
    NEW_VERSION="$(python3 -c "
import sys
parts=[int(x) for x in '$CURRENT_VERSION'.split('.')]
while len(parts)<3: parts.append(0)
b='$BUMP'
if b=='major': parts[0]+=1; parts[1]=0; parts[2]=0
elif b=='minor': parts[1]+=1; parts[2]=0
elif b=='patch': parts[2]+=1
else: sys.exit('bad --bump: '+b)
print('.'.join(str(x) for x in parts))
")"
    echo "bump: $CURRENT_VERSION → $NEW_VERSION ($BUMP)"
else
    NEW_VERSION="$CURRENT_VERSION"
    echo "no bump; reusing $NEW_VERSION"
fi

if [ "$DRY_RUN" = 1 ]; then
    echo "dry-run: would write firmware.version=$NEW_VERSION, build universal, upload to $SERVER"
    exit 0
fi

if [ "$DO_BUMP" = 1 ]; then
    python3 -c "
import pathlib
p=pathlib.Path('$VERSION_FILE')
text=p.read_text()
lines=[]
replaced=False
for line in text.splitlines(True):
    if line.lstrip().startswith('firmware.version='):
        lines.append('firmware.version=$NEW_VERSION\n'); replaced=True
    else:
        lines.append(line)
if not replaced:
    lines.append('firmware.version=$NEW_VERSION\n')
p.write_text(''.join(lines))
"
fi

# --- 2. Build -----------------------------------------------------------------
"$SCRIPT_DIR/build.sh"

ARTIFACT="$PROJECT_DIR/build/dist/plaiiinlight_os-${NEW_VERSION}-app.bin"
if [ ! -f "$ARTIFACT" ]; then
    echo "expected artifact missing: $ARTIFACT" >&2
    exit 1
fi

# --- 3. Login + upload --------------------------------------------------------
USER="${CREDS%%:*}"
PASS="${CREDS#*:}"
TOKEN="$(curl -sS -X POST "$SERVER/api/v1/auth/login" \
    -H "Content-Type: application/json" \
    -d "{\"username\":\"$USER\",\"password\":\"$PASS\"}" \
    | python3 -c "import sys,json;print(json.load(sys.stdin)['authResponse']['accessToken'])")"
if [ -z "$TOKEN" ]; then
    echo "login failed for $USER@$SERVER" >&2
    exit 1
fi

DESC="${DESCRIPTION:-PlaiiinLightOS $NEW_VERSION}"
echo "uploading $(basename "$ARTIFACT") → $SERVER (version=$NEW_VERSION)"
RESP="$(curl -sS -X POST "$SERVER/api/v1/admin/firmware" \
    -H "Authorization: Bearer $TOKEN" \
    -F "file=@$ARTIFACT" \
    -F "version=$NEW_VERSION" \
    -F "description=$DESC")"
echo "$RESP"

python3 -c "
import sys,json
try:
    d=json.loads('''$RESP''')
    if 'id' in d: print('firmware id:', d['id'])
except Exception: pass
" || true
