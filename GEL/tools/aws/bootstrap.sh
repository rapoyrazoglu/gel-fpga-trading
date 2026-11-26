#!/usr/bin/env bash
# tools/aws/bootstrap.sh
#
# One-time setup for the GEL execution engine on an AWS f2.6xlarge
# instance running the FPGA Developer AMI (Ubuntu 22.04).
#
# What this does (idempotent — safe to re-run):
#   1. Update apt + install build deps that the FPGA AMI doesn't ship
#   2. Install vcpkg into /opt/vcpkg (matches GEL's CMakeLists default)
#   3. vcpkg install: boost eigen3 curl nlohmann-json fmt range-v3 pybind11
#   4. Clone the connector + GEL repos into ~/capstone/ (or update them)
#   5. Build (Release): connector + gel_integrated_demo + Python module
#   6. Install the `gel` CLI symlink to /usr/local/bin so gelctl can find it
#   7. Run smoke tests: parity_test, shm_roundtrip_test, gel_integrated_demo
#
# Usage:
#   ssh ubuntu@<ip> 'bash -s' < tools/aws/bootstrap.sh
#   # or copy + run interactively:
#   scp tools/aws/bootstrap.sh ubuntu@<ip>:~/
#   ssh ubuntu@<ip>
#   bash ~/bootstrap.sh
#
# Env vars (optional):
#   GEL_REMOTE   = git URL for GEL        (default: https://github.com/<TBD>/GEL.git)
#   CONN_REMOTE  = git URL for connector  (default: https://github.com/<TBD>/connector.git)
#   WORKSPACE    = where to clone         (default: $HOME/capstone)
#   VCPKG_ROOT   = vcpkg install dir      (default: /opt/vcpkg)
#   BUILD_TYPE   = Release | Debug        (default: Release)
#
# Exit non-zero on any failure; output streamed to stdout + ~/bootstrap.log.

set -euo pipefail

# ---- config ----
GEL_REMOTE="${GEL_REMOTE:-https://github.com/REPLACE_ME/GEL.git}"
CONN_REMOTE="${CONN_REMOTE:-https://github.com/REPLACE_ME/connector.git}"
WORKSPACE="${WORKSPACE:-$HOME/capstone}"
VCPKG_ROOT="${VCPKG_ROOT:-/opt/vcpkg}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
LOG="$HOME/bootstrap.log"

# --with-claude flag — installs Claude Code + Node 20 + tmux + aws-fpga repo
WITH_CLAUDE=0
for arg in "$@"; do
    case "$arg" in
        --with-claude) WITH_CLAUDE=1 ;;
    esac
done

# ---- helpers ----
log() { printf '\n\033[1;36m==>\033[0m %s\n' "$*" | tee -a "$LOG"; }
ok()  { printf '\033[1;32m  ok\033[0m %s\n'   "$*" | tee -a "$LOG"; }
warn(){ printf '\033[1;33m  ?\033[0m  %s\n'   "$*" | tee -a "$LOG" >&2; }
err() { printf '\033[1;31m  !\033[0m %s\n'    "$*" | tee -a "$LOG" >&2; }

trap 'err "bootstrap failed at line $LINENO"' ERR

# Sanity-check: we expect the FPGA Developer AMI on Ubuntu 22.04.
if ! [[ -f /etc/os-release ]] || ! grep -q '^ID=ubuntu$' /etc/os-release; then
    warn "this script is tuned for Ubuntu 22.04. Detected:"
    cat /etc/os-release 2>&1 || true
    warn "continuing anyway — adjust packages if anything breaks"
fi

cd "$HOME"
: > "$LOG"
log "writing log to $LOG"

# ---------------------------------------------------------------------------
# 1. apt deps
# ---------------------------------------------------------------------------
log "apt: updating + installing build deps"
sudo apt-get update -qq
sudo apt-get install -yq \
    build-essential \
    cmake \
    git \
    curl \
    pkg-config \
    zip unzip tar \
    python3 python3-pip python3-dev \
    ninja-build \
    libssl-dev \
    autoconf libtool flex bison \
    >> "$LOG"
ok "apt deps installed"

# ---------------------------------------------------------------------------
# 2. vcpkg
# ---------------------------------------------------------------------------
if [[ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    ok "vcpkg already at $VCPKG_ROOT"
else
    log "installing vcpkg into $VCPKG_ROOT"
    sudo mkdir -p "$(dirname "$VCPKG_ROOT")"
    sudo chown -R "$USER" "$(dirname "$VCPKG_ROOT")"
    git clone --depth=1 https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT" >> "$LOG" 2>&1
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics >> "$LOG" 2>&1
    ok "vcpkg bootstrapped"
fi
export VCPKG_ROOT
echo "export VCPKG_ROOT=$VCPKG_ROOT" | sudo tee /etc/profile.d/vcpkg.sh > /dev/null
sudo chmod +x /etc/profile.d/vcpkg.sh

# ---------------------------------------------------------------------------
# 3. vcpkg install — the deps GEL's CMakeLists.txt declares
# ---------------------------------------------------------------------------
log "vcpkg: ensuring deps are installed (this can take 10-20 min on first run)"
"$VCPKG_ROOT/vcpkg" install \
    boost-system boost-thread \
    eigen3 \
    curl \
    nlohmann-json \
    fmt \
    range-v3 \
    pybind11 \
    >> "$LOG" 2>&1
ok "vcpkg deps ready"

# ---------------------------------------------------------------------------
# 4. clone repos (connector first — GEL depends on it as a sibling)
# ---------------------------------------------------------------------------
mkdir -p "$WORKSPACE"
cd "$WORKSPACE"

clone_or_update () {
    local url="$1" dir="$2"
    if [[ "$url" == *REPLACE_ME* ]]; then
        warn "$dir: \$$(echo "${dir^^}_REMOTE") not set — assuming repo is already at $WORKSPACE/$dir"
        if [[ ! -d "$dir/.git" ]]; then
            err "no git URL provided AND no checkout at $WORKSPACE/$dir"
            err "set ${dir^^}_REMOTE or scp the repo into $WORKSPACE/$dir before running"
            return 1
        fi
        return 0
    fi
    if [[ -d "$dir/.git" ]]; then
        log "updating existing $dir"
        ( cd "$dir" && git fetch --quiet && git pull --ff-only --quiet ) || warn "$dir pull skipped"
    else
        log "cloning $dir"
        git clone --depth=20 "$url" "$dir" >> "$LOG" 2>&1
    fi
}
clone_or_update "$CONN_REMOTE" connector
clone_or_update "$GEL_REMOTE"  GEL
ok "repos in place at $WORKSPACE/{connector,GEL}"

# ---------------------------------------------------------------------------
# 5. build
# ---------------------------------------------------------------------------
log "building GEL (Release): connector + gel_integrated_demo + Python module"
cd "$WORKSPACE/GEL"
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    >> "$LOG" 2>&1
cmake --build build -j --target \
    gel_integrated_demo \
    connector_parity_test \
    connector_shm_roundtrip_test \
    connector_gel_daemon \
    _connector_native \
    >> "$LOG" 2>&1
ok "build complete"

# ---------------------------------------------------------------------------
# 6. install gel + gelctl-friendly entrypoint
# ---------------------------------------------------------------------------
log "installing gel CLI to /usr/local/bin"
sudo ln -sf "$WORKSPACE/GEL/tools/cli/gel" /usr/local/bin/gel
ok "gel symlink: /usr/local/bin/gel -> $WORKSPACE/GEL/tools/cli/gel"

# ---------------------------------------------------------------------------
# 7. smoke tests
# ---------------------------------------------------------------------------
log "smoke test: connector_parity_test"
./build/connector_build/connector_parity_test 2>&1 | tail -5 | tee -a "$LOG"

log "smoke test: connector_shm_roundtrip_test"
./build/connector_build/connector_shm_roundtrip_test 2>&1 | tail -3 | tee -a "$LOG"

log "smoke test: gel_integrated_demo"
./build/gel_integrated_demo 2>&1 | tail -10 | tee -a "$LOG"

log "smoke test: gel CLI doctor"
gel doctor 2>&1 | tee -a "$LOG" || true

# ---------------------------------------------------------------------------
# 8. (optional) Claude Code — for FPGA work directly on the EC2 instance
# ---------------------------------------------------------------------------
if [[ "$WITH_CLAUDE" == "1" ]]; then
    log "installing Claude Code (--with-claude was set)"
    if [[ -f "$WORKSPACE/GEL/tools/aws/install-claude.sh" ]]; then
        bash "$WORKSPACE/GEL/tools/aws/install-claude.sh" 2>&1 | tee -a "$LOG"
    else
        warn "install-claude.sh not found at $WORKSPACE/GEL/tools/aws/ — skipped"
    fi
fi

# ---------------------------------------------------------------------------
# Done.
# ---------------------------------------------------------------------------
ok "bootstrap finished. log: $LOG"
cat <<MSG | tee -a "$LOG"

next steps from your laptop:

    gelctl --check
    gelctl status
    gelctl connect

if you ever need to bump the IP:

    gelctl ipupdate <new-ip>

if you used --with-claude, log in once:

    ssh gel-prod 'claude login'
    gelctl claude                # opens claude code in a tmux session on the host

MSG
