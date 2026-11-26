#!/usr/bin/env bash
# tools/aws/setup.sh
#
# One-shot laptop-side setup for an EC2 f2 instance:
#   1. validate the .pem (must be chmod 600)
#   2. write/update the ~/.ssh/config block via `gelctl install`
#   3. test SSH reachability
#   4. scp tools/aws/bootstrap.sh to the instance and run it
#   5. verify with `gelctl --check`
#
# Usage:
#   tools/aws/setup.sh --ip 1.2.3.4 --pem ~/.aws/gel.pem [--host gel-prod]
#                      [--user ubuntu] [--gel-remote URL] [--conn-remote URL]
#
# Idempotent: re-running just refreshes the ssh config and re-runs bootstrap
# (which itself is idempotent — apt/vcpkg/git pull all skip when up-to-date).

set -euo pipefail

HERE="$(cd "$(dirname "$0")/../cli" && pwd)"
GELCTL="$HERE/gelctl"
BOOTSTRAP="$(cd "$(dirname "$0")" && pwd)/bootstrap.sh"

ip="" pem="" alias="gel-prod" user="ubuntu" gel_remote="" conn_remote=""
with_claude=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ip)           ip="$2"; shift 2 ;;
        --pem)          pem="$2"; shift 2 ;;
        --host)         alias="$2"; shift 2 ;;
        --user)         user="$2"; shift 2 ;;
        --gel-remote)   gel_remote="$2"; shift 2 ;;
        --conn-remote)  conn_remote="$2"; shift 2 ;;
        --with-claude)  with_claude=1; shift ;;
        --help|-h)
            grep '^# ' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) echo "unknown flag: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$ip" || -z "$pem" ]]; then
    echo "usage: $0 --ip IP --pem PATH [--host ALIAS] [--user USER]" >&2
    exit 2
fi

green() { printf '\033[1;32m%s\033[0m\n' "$*"; }
cyan()  { printf '\033[1;36m%s\033[0m\n' "$*"; }
red()   { printf '\033[1;31m%s\033[0m\n' "$*" >&2; }

# 1. pem perms
pem_path="$(realpath "$pem")"
[[ -f "$pem_path" ]] || { red "pem not found: $pem_path"; exit 1; }
mode="$(stat -f '%Lp' "$pem_path" 2>/dev/null || stat -c '%a' "$pem_path")"
if [[ "$mode" != "600" && "$mode" != "400" ]]; then
    cyan "fixing pem perms: chmod 600 $pem_path  (was $mode)"
    chmod 600 "$pem_path"
fi

# 2. ssh config block
cyan "==> writing ~/.ssh/config block"
"$GELCTL" install --ip "$ip" --pem "$pem_path" --host "$alias" --user "$user"

# 3. ssh reachability
cyan "==> testing ssh connectivity"
if ! ssh -o BatchMode=yes -o ConnectTimeout=8 "$alias" 'echo "connected: $(uname -a)"'; then
    red "ssh test failed. Check Security Group (port 22 open from your IP)?"
    exit 1
fi
green "ssh works"

# 4. push + run bootstrap
cyan "==> uploading bootstrap.sh and running it on the server"
[[ -f "$BOOTSTRAP" ]] || { red "missing $BOOTSTRAP"; exit 1; }
remote_args=()
[[ -n "$gel_remote"  ]] && remote_args+=("GEL_REMOTE=$gel_remote")
[[ -n "$conn_remote" ]] && remote_args+=("CONN_REMOTE=$conn_remote")
bootstrap_flags=()
[[ "$with_claude" == "1" ]] && bootstrap_flags+=("--with-claude")
ssh "$alias" "${remote_args[@]:+env "${remote_args[@]}"} bash -s -- ${bootstrap_flags[*]}" < "$BOOTSTRAP"

# 5. final check
cyan "==> gelctl --check"
"$GELCTL" --host "$alias" --check
green ""
green "all set. Try:"
echo "  gelctl --host $alias status"
echo "  gelctl --host $alias connect"
echo ""
echo "If the EC2 IP ever changes:"
echo "  gelctl ipupdate <new-ip> --host $alias"
