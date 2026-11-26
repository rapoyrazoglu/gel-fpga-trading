# AWS deployment

GEL runs on an **AWS f2.6xlarge** instance (FPGA Developer AMI, Ubuntu 22.04).
The CLI on your laptop talks to it over plain SSH — no custom RPC, no
network ports beyond 22, no embedded credentials. Standard `~/.ssh/config`
+ `.pem` does the auth.

```
  laptop:  gelctl --host gel-prod  status
                       │
                       ssh (port 22, .pem auth)
                       │
                       ▼
  EC2 host:  gel status        ← runs on Ubuntu (CPU side)
                 │
                 PCIe / DMA
                 │
                 ▼
                 FPGA (bitstream — order_router, MM pricing, parser)
```

## One-time setup (after you receive the .pem and the static IP)

### Option A — one-command setup (recommended)

```sh
cd path/to/GEL
tools/aws/setup.sh \
    --ip 18.207.123.45 \
    --pem ~/.aws/gel-keypair.pem \
    --host gel-prod \
    --gel-remote  https://github.com/youruser/GEL.git \
    --conn-remote https://github.com/youruser/connector.git
```

This script does five things:

1. Sets `.pem` perms to 600 if needed.
2. Adds a `Host gel-prod` block to `~/.ssh/config` (via `gelctl install`).
3. Tests SSH reachability.
4. Uploads `bootstrap.sh` to the instance and runs it (installs vcpkg deps,
   builds connector + GEL, installs the `gel` CLI to `/usr/local/bin`,
   runs smoke tests).
5. Runs `gelctl --check` to verify the round-trip works.

Total time on a fresh f2.6xlarge: **~15-25 minutes** (most of it spent
in vcpkg compiling Boost/Eigen on the first run).

### Option B — step-by-step (if you want to see what's happening)

```sh
# 2.1  ssh config
gelctl install --ip 18.207.123.45 \
               --pem ~/.aws/gel-keypair.pem \
               --host gel-prod \
               --user ubuntu

# 2.2  test ssh
ssh gel-prod 'uname -a'

# 2.3  push bootstrap, run it
ssh gel-prod 'bash -s' < tools/aws/bootstrap.sh

# 2.4  verify
gelctl --check
gelctl status
```

## Daily usage

```sh
gelctl status                        # what's running on the server
gelctl accounts list                 # config on the server (separate from laptop)
gelctl daemon start                  # spawn the engine daemon on the server
gelctl ipc send-order --strategy 1 --symbol 1 --side BUY --qty 100
gelctl connect                       # interactive REPL over SSH
```

Anything `gel <cmd>` does locally, `gelctl <cmd>` does on the server.

## When the IP changes

EC2 Elastic IPs are static unless you release them, but a stop+start
without an Elastic IP will hand you a new public IP. Bump it with:

```sh
gelctl ipupdate 18.222.45.67               # default host (gel-prod)
gelctl ipupdate 18.222.45.67 --host gel-test
```

That just rewrites the `HostName` line in your `~/.ssh/config` block; the
.pem path stays the same.

## Multiple environments

If you spin up a second instance for testing:

```sh
gelctl install --host gel-test --ip ... --pem ~/.aws/gel-test.pem
gelctl --host gel-test status
GELCTL_HOST=gel-test gelctl status        # set as default for the shell
```

Each `Host` block is delimited by sentinel comments
(`# >>> gelctl: gel-test` … `# <<< gelctl: gel-test`) so they don't
interfere with each other or with hand-edited config.

## Tearing down

```sh
gelctl --host gel-prod daemon stop       # stop the engine on the server
gelctl uninstall --host gel-prod         # remove the alias from ~/.ssh/config
                                          # (the AWS instance + .pem are untouched)
```

## Security model

| Layer | Mechanism |
|---|---|
| Authentication | SSH public-key (the `.pem`) |
| Authorization  | EC2 user has the right group memberships (set up by AMI) |
| Network exposure | only port 22, restricted via Security Group to your IPs |
| GEL daemon | binds only to `localhost` SHM rings — never network-facing |
| Credentials in repo | none |
| Credentials in CLI binary | none |

The CLI knows the **alias name** (`gel-prod`); the IP and key live in
`~/.ssh/config` (managed by `gelctl install`/`ipupdate`/`uninstall`).
Nothing sensitive is committable to git.

## Files

```
GEL/tools/
├── cli/
│   ├── gel             # local CLI
│   ├── gelctl          # remote CLI (ssh-wrapped)
│   └── README.md
└── aws/
    ├── bootstrap.sh    # runs ON the EC2 instance, first time + idempotent re-run
    ├── setup.sh        # runs ON your laptop, orchestrates everything
    └── README.md       # this file
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `ssh: connect to host …: Operation timed out` | Security Group blocks your IP | Add your home IP CIDR to port 22 in the SG |
| `Permissions 0644 for '…/key.pem' are too open` | wrong perms | `chmod 600 ~/.aws/key.pem` |
| `gelctl --check` fails with `gel: command not found` | bootstrap didn't run / symlink missing | `ssh gel-prod 'bash -s' < tools/aws/bootstrap.sh` |
| `gelctl status` hangs | DNS / instance stopped | `aws ec2 describe-instances`; if running, `gelctl ipupdate <new-ip>` |
| vcpkg build fails on Boost | low memory mid-compile | f2.6xlarge has 256GB so this shouldn't happen; check `~/bootstrap.log` |
| `_connector_native.so` missing | pybind11 not picked up | re-run bootstrap; vcpkg should install pybind11 unconditionally |
