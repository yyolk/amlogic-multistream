# Sourced by the CoreELEC device scripts. Not executable on its own.
#
# One entry per CoreELEC line the player is packaged for. Each entry pins
# the official CoreELEC update archive the sysroot is extracted from, the
# FFmpeg release whose headers match that image's shared libraries, and
# the cross toolchain that targets its userspace.
#
#   coreelec21-ng-armv7hf  CoreELEC 21 (Omega) Amlogic-ng, 32-bit ARM
#                          userspace. GS-King X / S922X on CE21.
#   coreelec22-no-aarch64  CoreELEC 22 (Piers) Amlogic-no, 64-bit ARM
#                          userspace. GS-King X / S922X on CE22.
#
# Bumping a CoreELEC release: update CE_RELEASE, CE_TAR_SHA256 (the GitHub
# release asset digest), and CE_FFMPEG_VERSION / CE_FFMPEG_SHA256 if the
# image's libavcodec soname changed. fetch-coreelec-sysroot.sh refuses a
# sysroot whose libav* sonames do not match CE_FFMPEG_SONAMES.

COREELEC_TARGETS="coreelec21-ng-armv7hf coreelec22-no-aarch64"

# Pinned for every target. Built on the x86_64 build host.
CE_MOSQUITTO_VERSION=2.0.22
CE_MOSQUITTO_SHA256=2f752589ef7db40260b633fbdb536e9a04b446a315138d64a7ff3c14e2de6b68
CE_LIBDRM_VERSION=2.4.124
CE_LIBDRM_SHA256=ac36293f61ca4aafaf4b16a2a7afff312aa4f5c37c9fbd797de9e3c0863ca379
CE_APPIMAGETOOL_VERSION=1.9.1
CE_APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
CE_RUNTIME_VERSION=20251108

coreelec_target() {
  CE_TARGET=$1
  case "$CE_TARGET" in
    coreelec21-ng-armv7hf)
      CE_LINE="CoreELEC 21 Amlogic-ng (32-bit ARM userspace)"
      CE_RELEASE=21.3-Omega
      CE_DISTRO_ARCH=Amlogic-ng.arm
      CE_TAR_SHA256=a962fc30016ef0b6878580cccc32b0003816481c7c55679232c065a31f0d4908
      CE_TRIPLE=arm-linux-gnueabihf
      CE_CMAKE_PROCESSOR=arm
      CE_FFMPEG_ARCH=arm
      CE_QEMU=qemu-arm-static
      CE_INTERP=/lib/ld-linux-armhf.so.3
      CE_RUNTIME_ARCH=armhf
      CE_RUNTIME_SHA256=e9060d37577b8a29914ec12d8740add24e19ff29012fb1fa0f60daf62db0688d
      CE_FILE_PATTERN='ELF 32-bit LSB .*ARM, EABI5'
      CE_FFMPEG_VERSION=6.0.1
      CE_FFMPEG_SHA256=9b16b8731d78e596b4be0d720428ca42df642bb2d78342881ff7f5bc29fc9623
      CE_FFMPEG_SONAMES="libavcodec.so.60 libavformat.so.60 libavutil.so.58 libswscale.so.7"
      ;;
    coreelec22-no-aarch64)
      CE_LINE="CoreELEC 22 Amlogic-no (64-bit ARM userspace)"
      CE_RELEASE=22.0-Piers_rc1
      CE_DISTRO_ARCH=Amlogic-no.aarch64
      CE_TAR_SHA256=d9df062a7d744f8db026bac29d8e081ccc51e0eb43d6ea85ef81a5ee1a3a6365
      CE_TRIPLE=aarch64-linux-gnu
      CE_CMAKE_PROCESSOR=aarch64
      CE_FFMPEG_ARCH=aarch64
      CE_QEMU=qemu-aarch64-static
      CE_INTERP=/lib/ld-linux-aarch64.so.1
      CE_RUNTIME_ARCH=aarch64
      CE_RUNTIME_SHA256=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
      CE_FILE_PATTERN='ELF 64-bit LSB .*ARM aarch64'
      CE_FFMPEG_VERSION=9.0.2
      CE_FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
      CE_FFMPEG_SONAMES="libavcodec.so.63 libavformat.so.63 libavutil.so.61 libswscale.so.10"
      ;;
    *)
      echo "error: unknown CoreELEC target '$CE_TARGET' (expected one of: $COREELEC_TARGETS)" >&2
      return 1
      ;;
  esac
  CE_TAR_NAME="CoreELEC-${CE_DISTRO_ARCH}-${CE_RELEASE}.tar"
  CE_TAR_URL="https://github.com/CoreELEC/CoreELEC/releases/download/${CE_RELEASE}/${CE_TAR_NAME}"
  CE_FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${CE_FFMPEG_VERSION}.tar.xz"
  CE_RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/${CE_RUNTIME_VERSION}/runtime-${CE_RUNTIME_ARCH}"
}

CE_MOSQUITTO_URL="https://mosquitto.org/files/source/mosquitto-${CE_MOSQUITTO_VERSION}.tar.gz"
CE_LIBDRM_URL="https://dri.freedesktop.org/libdrm/libdrm-${CE_LIBDRM_VERSION}.tar.xz"
CE_APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/${CE_APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"

# download URL DEST SHA256. Keeps DEST if it already matches.
coreelec_download() {
  local url=$1 dest=$2 sha=$3
  if [[ -f "$dest" ]] && echo "$sha  $dest" | sha256sum -c --status; then
    echo "Using cached $dest"
    return 0
  fi
  echo "Downloading $url"
  mkdir -p "$(dirname "$dest")"
  curl -fL --retry 5 --retry-delay 3 --retry-all-errors -sS -o "$dest.partial" "$url"
  if ! echo "$sha  $dest.partial" | sha256sum -c --status; then
    echo "error: sha256 mismatch for $url" >&2
    echo "  expected $sha" >&2
    echo "  got      $(sha256sum "$dest.partial" | cut -d' ' -f1)" >&2
    rm -f "$dest.partial"
    return 1
  fi
  mv "$dest.partial" "$dest"
}
