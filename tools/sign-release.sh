#!/bin/sh
# sign-release.sh - sign a kernel image so a board will install it.
#
# The board carries only the public key, in its initramfs at
# /etc/update-key.pub, so it can check a signature and cannot make one.
# The private key stays here and must never be copied onto a board or
# into the image: anything that can sign can replace the operating
# system on every machine that trusts this key.
#
#   tools/sign-release.sh --new-key            make a signing key
#   tools/sign-release.sh <image>              sign it -> <image>.sig
#
# The keypair lives in keys/. That directory is in .gitignore, which is
# a reminder and not a protection - keep the private key somewhere you
# would keep a house key.
set -e

KEYDIR="${KEYDIR:-keys}"
PRIV="$KEYDIR/update-key.pem"
PUB="$KEYDIR/update-key.pub"          # raw 256-byte modulus, big-endian

usage() {
    echo "usage: $0 --new-key"
    echo "       $0 <image>"
    exit 2
}

# The board reads a raw modulus, not PEM: there is no ASN.1 parser in
# that userland and adding one to read our own key would be more code
# than the signature check itself.
extract_modulus() {
    openssl rsa -in "$1" -noout -modulus \
      | sed 's/Modulus=//' \
      | python3 -c 'import sys,binascii;sys.stdout.buffer.write(binascii.unhexlify(sys.stdin.read().strip()))'
}

case "$1" in
--new-key)
    if [ -f "$PRIV" ]; then
        echo "$PRIV already exists."
        echo "Making a new key would stop every board that trusts the old"
        echo "one from accepting updates. Move it aside first if you mean it."
        exit 1
    fi
    mkdir -p "$KEYDIR"
    ( umask 077; openssl genrsa -out "$PRIV" 2048 2>/dev/null )
    extract_modulus "$PRIV" > "$PUB"
    chmod 600 "$PRIV"
    echo "wrote $PRIV  (private - never put this on a board)"
    echo "wrote $PUB   ($(wc -c < "$PUB") bytes, goes into the image)"
    echo
    echo "Rebuild the image so the public key is baked into it:  make"
    ;;
"")
    usage
    ;;
*)
    IMAGE="$1"
    [ -f "$IMAGE" ] || { echo "$IMAGE is not there"; exit 1; }
    [ -f "$PRIV" ]  || { echo "no signing key - run: $0 --new-key"; exit 1; }

    openssl dgst -sha256 -sign "$PRIV" -out "$IMAGE.sig" "$IMAGE"

    # Prove it verifies before saying it worked. A signing script that
    # reports success on a signature nothing accepts is worse than one
    # that fails. (A temporary file rather than process substitution:
    # this runs under /bin/sh, which does not have <(...).)
    TMPPUB="$(mktemp)"
    trap 'rm -f "$TMPPUB"' EXIT
    openssl rsa -in "$PRIV" -pubout -out "$TMPPUB" 2>/dev/null
    openssl dgst -sha256 -verify "$TMPPUB" \
        -signature "$IMAGE.sig" "$IMAGE" >/dev/null

    echo "$IMAGE.sig  ($(wc -c < "$IMAGE.sig") bytes)"
    echo "sha256: $(sha256sum "$IMAGE" | cut -d' ' -f1)"
    echo
    echo "Put both files next to each other where the board can fetch them."
    ;;
esac
