#!/usr/bin/env bash
# Prepares a fresh (Linux) development container for Code:Icarus:
#   - Godot 4.5.1 editor binary at /opt/godot, symlinked as `godot`
#   - SCons (GDExtension build), Pillow (image tooling)
#   - Mesa lavapipe (software Vulkan) + Xvfb for rendered screenshots
#   - godot-cpp submodule
set -euo pipefail
GODOT_VERSION="4.5.1-stable"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v godot >/dev/null 2>&1; then
    mkdir -p /opt/godot
    curl -sSL -o /opt/godot/godot.zip \
        "https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/Godot_v${GODOT_VERSION}_linux.x86_64.zip"
    (cd /opt/godot && unzip -o -q godot.zip && rm godot.zip)
    ln -sf "/opt/godot/Godot_v${GODOT_VERSION}_linux.x86_64" /usr/local/bin/godot
fi
python3 -m pip install -q scons pillow 2>/dev/null || true
if [ ! -f /usr/share/vulkan/icd.d/lvp_icd.json ] && command -v apt-get >/dev/null 2>&1; then
    (apt-get update -qq && apt-get install -y -qq mesa-vulkan-drivers xvfb) >/dev/null 2>&1 || true
fi
cd "$ROOT"
git submodule update --init --recursive
echo "setup done: $(godot --version)"
