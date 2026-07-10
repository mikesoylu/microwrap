#!/bin/sh
set -eu

repo=${MICROWRAP_REPO:-mikesoylu/microwrap}
prefix=${PREFIX:-/usr/local}
release=${MICROWRAP_RELEASE:-latest}

die()
{
    echo "microwrap setup: $*" >&2
    exit 1
}

usage()
{
    cat <<'EOF'
Usage: setup.sh [--prefix DIR] [--release TAG]

Options:
  --prefix DIR   Install into DIR/bin. Defaults to /usr/local.
  --release TAG  Install a specific SHA release. Defaults to latest.
  --help         Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --prefix)
        [ "$#" -ge 2 ] || die "--prefix requires a directory"
        prefix=$2
        shift 2
        ;;
    --release)
        [ "$#" -ge 2 ] || die "--release requires a tag"
        release=$2
        shift 2
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        die "unknown option: $1"
        ;;
    esac
done

[ "$(uname -s)" = Linux ] || die "only Linux is supported"
case "$(uname -m)" in
x86_64|amd64) arch=amd64 ;;
aarch64|arm64) arch=arm64 ;;
*) die "unsupported architecture: $(uname -m)" ;;
esac

case "$prefix" in
/*) ;;
*) die "--prefix must be an absolute path" ;;
esac
case "$release" in
*[!A-Za-z0-9._-]*) die "invalid release tag: $release" ;;
esac
[ -n "$release" ] || die "release tag must not be empty"

if command -v curl >/dev/null 2>&1; then
    download()
    {
        curl -fsSL --retry 3 --output "$2" "$1"
    }
elif command -v wget >/dev/null 2>&1; then
    download()
    {
        wget -q -O "$2" "$1"
    }
else
    die "curl or wget is required"
fi

if [ -n "${MICROWRAP_RELEASE_BASE_URL:-}" ]; then
    base_url=${MICROWRAP_RELEASE_BASE_URL%/}
else
    case "$release" in
    latest) base_url="https://github.com/${repo}/releases/latest/download" ;;
    *) base_url="https://github.com/${repo}/releases/download/${release}" ;;
    esac
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/microwrap-setup.XXXXXX") ||
    die "could not create a temporary directory"
install_tmp=
cleanup()
{
    [ -z "$install_tmp" ] || rm -f "$install_tmp"
    rm -rf "$tmp"
}
trap cleanup 0
trap 'exit 1' 1 2 15

checksums="$tmp/SHA256SUMS"
download "$base_url/SHA256SUMS" "$checksums" ||
    die "could not download release checksums"

asset=$(awk -v suffix="-linux-${arch}" '
    {
        name = $2
        sub(/^\.\//, "", name)
        if (length(name) >= length(suffix) &&
            substr(name, length(name) - length(suffix) + 1) == suffix) {
            print name
            exit
        }
    }
' "$checksums")
packed=false
if [ -z "$asset" ]; then
    asset=$(awk -v suffix="-linux-${arch}.tar.gz" '
        {
            name = $2
            sub(/^\.\//, "", name)
            if (length(name) >= length(suffix) &&
                substr(name, length(name) - length(suffix) + 1) == suffix) {
                print name
                exit
            }
        }
    ' "$checksums")
    packed=true
fi
[ -n "$asset" ] || die "release does not contain a linux-${arch} binary"

expected=$(awk -v wanted="$asset" '
    {
        name = $2
        sub(/^\.\//, "", name)
        if (name == wanted) {
            print tolower($1)
            exit
        }
    }
' "$checksums")
[ -n "$expected" ] || die "release checksum is missing for $asset"

download "$base_url/$asset" "$tmp/$asset" ||
    die "could not download $asset"

if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp/$asset")
    actual=${actual%% *}
elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$tmp/$asset")
    actual=${actual%% *}
elif command -v openssl >/dev/null 2>&1; then
    actual=$(openssl dgst -sha256 "$tmp/$asset")
    actual=${actual##* }
else
    die "sha256sum, shasum, or openssl is required to verify the download"
fi
actual=$(printf '%s' "$actual" | tr 'A-F' 'a-f')
[ "$actual" = "$expected" ] || die "checksum verification failed for $asset"

if [ "$packed" = true ]; then
    command -v tar >/dev/null 2>&1 || die "tar is required for this older release"
    package=${asset%.tar.gz}
    tar -xzf "$tmp/$asset" -C "$tmp" || die "could not extract $asset"
    binary="$tmp/$package/microwrap"
    [ -f "$binary" ] || die "archive does not contain microwrap"
else
    binary="$tmp/$asset"
fi

case "$prefix" in
/) bin_dir=/bin ;;
*) bin_dir=${prefix%/}/bin ;;
esac
mkdir -p "$bin_dir" || die "could not create $bin_dir"
install_tmp="$bin_dir/.microwrap.$$"
cp "$binary" "$install_tmp" || die "could not write to $bin_dir"
chmod 0755 "$install_tmp" || die "could not make microwrap executable"
mv -f "$install_tmp" "$bin_dir/microwrap" || die "could not install microwrap"
install_tmp=

echo "Installed microwrap to $bin_dir/microwrap"
