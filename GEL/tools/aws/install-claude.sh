#!/usr/bin/env bash
# tools/aws/install-claude.sh
#
# Install Claude Code on an Ubuntu EC2 instance (the FPGA Developer AMI
# comes with Node.js too old for it). After this runs, the user logs in
# once with `claude login` and Claude Code is ready to drive the FPGA
# workflow directly on the host that has Vivado + aws-fpga.
#
# Idempotent: re-run safely. Won't reinstall Node if 20+ is already
# present.
#
# Usage (run on the EC2 instance, or via bootstrap.sh --with-claude):
#   bash ~/install-claude.sh
#   claude login                    # one-time, opens a browser-paste flow

set -euo pipefail

NODE_TARGET_MAJOR=20

ok()   { printf '\033[1;32m  ok\033[0m %s\n'  "$*"; }
log()  { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m  ?\033[0m  %s\n' "$*" >&2; }
err()  { printf '\033[1;31m  !\033[0m %s\n'  "$*" >&2; }

# --- 1. Node.js 20+ ---------------------------------------------------------
need_node=0
if command -v node >/dev/null 2>&1; then
    cur_major="$(node -v | sed 's/^v\([0-9]\+\).*/\1/')"
    if [[ "$cur_major" -ge "$NODE_TARGET_MAJOR" ]]; then
        ok "Node.js v$(node -v) already installed"
    else
        warn "Node.js v$(node -v) too old; need ${NODE_TARGET_MAJOR}+"
        need_node=1
    fi
else
    need_node=1
fi

if [[ "$need_node" == "1" ]]; then
    log "installing Node.js ${NODE_TARGET_MAJOR}.x via NodeSource"
    curl -fsSL "https://deb.nodesource.com/setup_${NODE_TARGET_MAJOR}.x" | sudo -E bash -
    sudo apt-get install -yq nodejs
    ok "Node.js $(node -v) installed"
fi

# --- 2. tmux (for long-running Claude Code sessions) ------------------------
if ! command -v tmux >/dev/null 2>&1; then
    log "installing tmux (so synthesis can run while you detach)"
    sudo apt-get install -yq tmux
fi
ok "tmux $(tmux -V | awk '{print $2}') ready"

# --- 3. Claude Code ---------------------------------------------------------
if command -v claude >/dev/null 2>&1; then
    ok "claude already on PATH ($(claude --version 2>/dev/null || echo '?'))"
else
    log "installing @anthropic-ai/claude-code globally via npm"
    sudo npm install -g @anthropic-ai/claude-code
    ok "claude installed: $(command -v claude)"
fi

# --- 4. AWS FPGA repo (needed for the HDK/SDK setup scripts) ----------------
if [[ ! -d "$HOME/aws-fpga" ]]; then
    log "cloning aws-fpga (HDK + SDK + examples)"
    git clone --depth=20 https://github.com/aws/aws-fpga.git "$HOME/aws-fpga"
fi
ok "aws-fpga at $HOME/aws-fpga"

# Add HDK/SDK source helpers to a single 'aws-fpga-env' alias for convenience.
cat > "$HOME/.aws-fpga-env" <<'ENV'
# Source this before doing FPGA work:
#     source ~/.aws-fpga-env
source ~/aws-fpga/hdk_setup.sh    >/dev/null 2>&1 || true
source ~/aws-fpga/sdk_setup.sh    >/dev/null 2>&1 || true
export AWS_FPGA_REPO_DIR="$HOME/aws-fpga"
ENV
ok "aws-fpga env helper at $HOME/.aws-fpga-env"

# --- 5. Friendly hints ------------------------------------------------------
cat <<MSG

next steps (run on this EC2 instance):

    claude login                  # one-time: paste the auth code
    source ~/.aws-fpga-env        # before any FPGA build
    tmux new -s fpga              # start a session that survives disconnect
    claude                        # talk to Claude Code

  inside tmux, when synthesis is running long, detach with:
    ctrl-b d                      # detach, leave job running
    tmux attach -t fpga           # later: come back

from your laptop you can shortcut all of this:

    gelctl claude                 # ssh -t + tmux attach-or-new + claude

MSG
