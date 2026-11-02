#!/usr/bin/env bash
# Adapted from konoha-kernel-gki's Ubuntu 24.04 build workflow.
set -euo pipefail
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /opt/hostedtoolcache/CodeQL
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends bc bison build-essential curl flex \
  git libelf-dev libssl-dev lz4 lzop rsync unzip xz-utils zip zstd python3 kmod jq
mkdir -p "$HOME/.local/bin"
curl -fsSL https://storage.googleapis.com/git-repo-downloads/repo -o "$HOME/.local/bin/repo"
chmod +x "$HOME/.local/bin/repo"
echo "$HOME/.local/bin" >> "$GITHUB_PATH"
git config --global user.name 'Konoha CI'
git config --global user.email '41898282+github-actions[bot]@users.noreply.github.com'
available=$(df --output=avail -B1 "$GITHUB_WORKSPACE" | tail -1)
if (( available < 55 * 1024 * 1024 * 1024 )); then
  echo 'At least 55 GiB free disk is required after hosted-runner cleanup.' >&2
  df -h
  exit 1
fi
# Bound allocation and fail visibly; do not retry forever on a full disk.
task_swap=/mnt/konoha-build.swap
sudo fallocate -l 8G "$task_swap"
sudo chmod 600 "$task_swap"
sudo mkswap "$task_swap"
sudo swapon "$task_swap"
df -h
free -h
# Public and private standard runners have different resources. Reserve memory
# for the OS and bound Kleaf to the resources actually provided by this runner.
task_memory_mb=$(awk '/^MemTotal:/ {print int($2 / 1024) - 3000}' /proc/meminfo)
if (( task_memory_mb < 2048 )); then
  echo 'Insufficient RAM for a Kleaf build.' >&2
  exit 1
fi
task_jobs=$(nproc)
if (( task_jobs > 4 )); then task_jobs=4; fi
echo "KONOHA_BUILD_JOBS=$task_jobs" >> "$GITHUB_ENV"
echo "KONOHA_BUILD_MEMORY_MB=$task_memory_mb" >> "$GITHUB_ENV"
