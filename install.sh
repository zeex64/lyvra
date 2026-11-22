#!/usr/bin/env bash
# Lyvra installer: builds Lyvra and installs it so it shows up in your app launcher.
#
#   ./install.sh                      build + install for your user (~/.local, no sudo)
#   ./install.sh --system             build + install for everyone (/usr/local, uses sudo)
#   ./install.sh --prefix DIR         install somewhere else
#   ./install.sh --fsr4-lib PATH      also install fsr4-vk's amd_fidelityfx_upscaler_vk.so (enables FSR 4)
#   ./install.sh --uninstall          remove what a previous install put in place (keeps your settings)
#
# Your settings stay in ~/.config/lyvra either way.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

PREFIX="$HOME/.local"
SUDO=""
FSR4_LIB=""
UNINSTALL=0
BUILD_DIR="build"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --system)    PREFIX="/usr/local"; SUDO="sudo" ;;
        --prefix)    [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift ;;
        --fsr4-lib)  [ $# -ge 2 ] || die "--fsr4-lib needs a path"; FSR4_LIB="$(readlink -f "$2")"; shift
                     [ -f "$FSR4_LIB" ] || die "no such file: $FSR4_LIB" ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help)   sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

MANIFEST="$PREFIX/share/lyvra/install_manifest.txt"

refresh_desktop() {
    local apps="$PREFIX/share/applications" icons="$PREFIX/share/icons/hicolor"
    command -v update-desktop-database >/dev/null && $SUDO update-desktop-database -q "$apps" 2>/dev/null || true
    command -v gtk-update-icon-cache >/dev/null && $SUDO gtk-update-icon-cache -q -f -t "$icons" 2>/dev/null || true
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
    bold "Uninstalling Lyvra from $PREFIX"
    [ -f "$MANIFEST" ] || die "no install found at $PREFIX (missing $MANIFEST)"
    while IFS= read -r f || [ -n "$f" ]; do
        [ -n "$f" ] && [ -e "$f" ] && $SUDO rm -f -- "$f" && ok "removed $f"
    done < "$MANIFEST"
    $SUDO rm -f -- "$MANIFEST"
    for d in "$PREFIX/lib/lyvra" "$PREFIX/share/lyvra"; do
        [ -d "$d" ] && $SUDO rmdir --ignore-fail-on-non-empty -- "$d" 2>/dev/null || true
    done
    refresh_desktop
    bold "Done. Your settings in ~/.config/lyvra were kept."
    exit 0
fi

# ---------------------------------------------------------------------------
bold "Checking build dependencies"
missing=()
need() { command -v "$1" >/dev/null 2>&1 && ok "$1" || { missing+=("$2"); warn "$1 not found"; }; }
need cmake cmake
need c++ "gcc (or clang)"
need pkg-config pkgconf
need glslc shaderc
need glslangValidator glslang
need git git
need python3 python
pkg-config --exists sdl3 2>/dev/null && ok "SDL3" || { missing+=("sdl3"); warn "SDL3 development files not found"; }
pkg-config --exists libturbojpeg 2>/dev/null && ok "libturbojpeg" || { missing+=("libjpeg-turbo"); warn "libturbojpeg development files not found"; }
pkg-config --exists vulkan 2>/dev/null && ok "Vulkan loader" || { missing+=("vulkan-icd-loader"); warn "Vulkan loader not found"; }
GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); ok "ninja"; else warn "ninja not found, using make (slower)"; fi
if [ ${#missing[@]} -gt 0 ]; then
    echo
    echo "Install the missing packages first. On Arch / CachyOS:"
    echo "  sudo pacman -S --needed cmake ninja pkgconf shaderc glslang git python sdl3 libjpeg-turbo vulkan-icd-loader"
    echo "On Debian / Ubuntu (24.10+):"
    echo "  sudo apt install cmake ninja-build pkg-config glslc glslang-tools git python3 libsdl3-dev libturbojpeg0-dev libvulkan-dev"
    die "missing: ${missing[*]}"
fi

# Submodules (ImGui, Vulkan headers, FSR 1). Not --recursive: FSR 1's sample submodules aren't needed.
if [ ! -f third_party/imgui/imgui.h ] || [ ! -f third_party/vulkan-headers/include/vulkan/vulkan.h ] || [ ! -f third_party/fsr1/ffx-fsr/ffx_fsr1.h ] \
   || [ ! -f third_party/cas/ffx-cas/ffx_cas.h ] || [ ! -f third_party/nis/NIS/NIS_Scaler.h ]; then
    [ -d .git ] || die "third_party submodules are missing and this isn't a git checkout; clone the repo with git"
    bold "Fetching submodules"
    git submodule update --init --depth 1
fi

# ---------------------------------------------------------------------------
bold "Building Lyvra (Release)"
cmake_args=(-S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX")
[ -n "$FSR4_LIB" ] && cmake_args+=(-DLYVRA_FSR4_VK_LIB="$FSR4_LIB")
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake "${cmake_args[@]}" >/dev/null          # keep the existing generator
else
    cmake "${GENERATOR[@]}" "${cmake_args[@]}" >/dev/null
fi
cmake --build "$BUILD_DIR" --parallel
ok "built $BUILD_DIR/lyvra"

# ---------------------------------------------------------------------------
bold "Installing to $PREFIX"
$SUDO cmake --install "$BUILD_DIR" >/dev/null
$SUDO install -d "$PREFIX/share/lyvra"
# CMake's manifest has no trailing newline; rebuild it line by line and include itself.
{ cat "$BUILD_DIR/install_manifest.txt"; echo; echo "$MANIFEST"; } | grep -v '^$' | $SUDO tee "$MANIFEST" >/dev/null
# Launchers don't always see ~/.local/bin on PATH, so point the menu entry at the binary directly.
DESKTOP="$PREFIX/share/applications/lyvra.desktop"
$SUDO sed -i "s|^Exec=lyvra|Exec=$PREFIX/bin/lyvra|" "$DESKTOP"
refresh_desktop
ok "binary      $PREFIX/bin/lyvra"
ok "launcher    $DESKTOP"
ok "icons       $PREFIX/share/icons/hicolor/*/apps/lyvra.*"
if [ -f "$PREFIX/lib/lyvra/amd_fidelityfx_upscaler_vk.so" ]; then
    ok "FSR 4       $PREFIX/lib/lyvra/amd_fidelityfx_upscaler_vk.so"
else
    warn "FSR 4 library not installed (FSR 4 will be greyed out). Re-run with --fsr4-lib /path/to/amd_fidelityfx_upscaler_vk.so"
fi

echo
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) warn "$PREFIX/bin is not on your PATH. Start Lyvra from your app launcher, or add it to PATH." ;;
esac
id -nG | grep -qw video || warn "you're not in the 'video' group; capture devices may not open (sudo usermod -aG video \$USER, then log out and in)"
bold "Done. Launch Lyvra from your app menu, or run: lyvra"
echo "To remove it later: ./install.sh --uninstall${SUDO:+ --system}"
