#!/bin/bash
# Build the private runtime on Debian Trixie arm64.
set -euo pipefail
src="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="${1:?Usage: build-deb.sh WORK_DIRECTORY}"
[[ "$(dpkg --print-architecture)" == arm64 ]] || { echo "Run on arm64." >&2; exit 1; }
# shellcheck source=/dev/null
. /etc/os-release
[[ "${VERSION_CODENAME:-}" == trixie ]] || { echo "Use Debian Trixie." >&2; exit 1; }
mkdir -p "$work"
work="$(cd "$work" && pwd)"
if [[ -n "${SOURCE_MIRROR:-}" ]]; then
    # git fetch runs inside each checkout, so resolve a relative mirror first.
    SOURCE_MIRROR="$(cd -- "$SOURCE_MIRROR" && pwd)"
fi
prefix=/usr/share/lenovo-elden-sensors
(cd "$src/vendor" && sha256sum --check --quiet runtime.sha256 fallback-registry.sha256)
stage="$(mktemp -d "$work/stage.XXXXXX")"
while read -r name url revision; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    # A fresh checkout prevents stale patches from entering a release.
    source_dir="$stage/source/$name"
    git init -q "$source_dir"
    if [[ -n "${SOURCE_MIRROR:-}" ]]; then
        url="$SOURCE_MIRROR/$name"
    fi
    git -C "$source_dir" fetch --depth=1 "$url" "$revision"
    git -C "$source_dir" checkout -q FETCH_HEAD
    git -C "$source_dir" apply "$src/patches/$name.patch"
done < "$src/sources.lock"
for name in hexagonrpc libssc; do
    args=()
    [[ "$name" != hexagonrpc ]] || args+=(-Dhexagonrpcd_verbose=false)
    meson setup "$stage/build-$name" "$stage/source/$name" --prefix="$prefix" --libdir=lib "${args[@]}"
    meson compile -C "$stage/build-$name" -j"${JOBS:-4}"
    meson test -C "$stage/build-$name" --print-errorlogs --timeout-multiplier 5
    DESTDIR="$stage/install" meson install -C "$stage/build-$name"
done
# Meson supplies build-tree paths without rewriting installed .pc metadata.
export PKG_CONFIG_PATH="$stage/build-libssc/meson-uninstalled"
export LD_LIBRARY_PATH="$stage/install$prefix/lib"
meson setup "$stage/build-proxy" "$stage/source/iio-sensor-proxy" \
    --prefix="$prefix" --libdir=lib --libexecdir=libexec \
    -Dssc-support=enabled -Dtests=false -Dgtk-tests=false -Dgtk_doc=false \
    -Dsystemdsystemunitdir="$prefix/lib/systemd/system" -Dudevrulesdir="$prefix/lib/udev/rules.d"
meson compile -C "$stage/build-proxy" -j"${JOBS:-4}"
DESTDIR="$stage/install" meson install -C "$stage/build-proxy"
cmake -S "$src" -B "$stage/build-tools" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DINSTALL_RUNTIME_DATA=ON \
    -DWRAPPER_INSTALL_DIR=/usr/bin -DBUILD_TESTING=ON
cmake --build "$stage/build-tools" -j"${JOBS:-4}"
ctest --test-dir "$stage/build-tools" --output-on-failure
pkg="$stage/package"
DESTDIR="$pkg" cmake --install "$stage/build-tools"
for program in hexagonrpcd ssccli monitor-sensor; do
    install -Dm755 "$stage/install$prefix/bin/$program" "$pkg$prefix/bin/$program"
done
install -Dm755 "$stage/install$prefix/libexec/iio-sensor-proxy" "$pkg$prefix/libexec/iio-sensor-proxy"
mkdir -p "$pkg$prefix/lib"
cp -a "$stage/install$prefix/lib/"*.so* "$pkg$prefix/lib/"
install -Dm644 "$src/packaging/control" "$pkg/DEBIAN/control"
doc="$pkg/usr/share/doc/lenovo-elden-sensors"
install -Dm644 "$src/README.md" "$doc/README.md"
install -Dm644 "$src/RUNTIME.md" "$doc/RUNTIME.md"
install -Dm644 "$src/sources.lock" "$doc/sources.lock"
install -Dm644 "$src/vendor/runtime.sha256" "$doc/runtime.sha256"
install -Dm644 "$src/vendor/fallback-registry.sha256" "$doc/fallback-registry.sha256"
for name in hexagonrpc libssc iio-sensor-proxy; do
    for license in COPYING LICENSE; do
        if [[ -f "$stage/source/$name/$license" ]]; then
            install -Dm644 "$stage/source/$name/$license" "$doc/$name-$license"
        fi
    done
done
version="$(sed -n 's/^Version: //p' "$src/packaging/control")"
artifact="$work/lenovo-elden-sensors_${version}_arm64.deb"
dpkg-deb --root-owner-group --build "$pkg" "$artifact"
# Include exact upstream source, patches and build files with the binary release.
tar --exclude=.git -czf "$work/lenovo-elden-sensors_${version}_sources.tar.gz" \
    -C "$stage" source -C "$src" CMakeLists.txt sources.lock patches scripts src tests packaging README.md RUNTIME.md LICENSE vendor
sha256sum "$artifact" "$work/lenovo-elden-sensors_${version}_sources.tar.gz"
