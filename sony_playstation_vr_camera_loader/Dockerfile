ARG BUILD_FROM=ghcr.io/home-assistant/base:3.24-2026.08.0

# ---- build stage: RNNoise, the capture engine and go2rtc ----------------------
FROM ${BUILD_FROM} AS builder
ARG BUILD_ARCH=amd64
RUN apk add --no-cache \
    # renovate: datasource=repology depName=alpine_3_24/build-base
    build-base=0.5-r4 \
    # renovate: datasource=repology depName=alpine_3_24/autoconf
    autoconf=2.73-r0 \
    # renovate: datasource=repology depName=alpine_3_24/automake
    automake=1.18.1-r1 \
    # renovate: datasource=repology depName=alpine_3_24/libtool
    libtool=2.6.0-r1 \
    # renovate: datasource=repology depName=alpine_3_24/curl
    curl=8.22.0-r0 \
    # renovate: datasource=repology depName=alpine_3_24/libjpeg-turbo-dev
    libjpeg-turbo-dev=3.1.3-r0 \
    # renovate: datasource=repology depName=alpine_3_24/linux-headers
    linux-headers=7.0.0-r1

# src/c/rnnoise is the upstream git submodule (xiph/rnnoise v0.2); the script
# adds the trained model and links it statically. See src/c/build-rnnoise.sh.
COPY src/c/ /src/c/
RUN sh /src/c/build-rnnoise.sh /src/c/rnnoise /usr/local

# Tests run in CI (.github/workflows/tests.yml), not on the Home Assistant host.
# "clean" first, so objects from a developer checkout can never sneak in.
RUN make -C /src/c/psvr-capture clean \
    && make -C /src/c/psvr-capture -j"$(nproc)" CFLAGS="-O2 -I/usr/local/include" LDFLAGS="-L/usr/local/lib" \
    && make -C /src/c/psvr-capture install PREFIX=/usr/local

RUN set -eu; \
    case "${BUILD_ARCH}" in \
      amd64) artifact=go2rtc_linux_amd64; sha=32d616af226bd731678ffde328b94cfb94e30339bfefc469cfb76323144615a6 ;; \
      aarch64) artifact=go2rtc_linux_arm64; sha=359fabade8a7a51e81a55fe6df6b0ef81764a5e1d63179577534eaaa71904b50 ;; \
      *) echo "Unsupported architecture: ${BUILD_ARCH}" >&2; exit 1 ;; \
    esac; \
    curl -fL --retry 3 "https://github.com/AlexxIT/go2rtc/releases/download/v1.9.14/${artifact}" -o /usr/local/bin/go2rtc; \
    echo "${sha}  /usr/local/bin/go2rtc" | sha256sum -c -; \
    chmod 755 /usr/local/bin/go2rtc

# ---- runtime ------------------------------------------------------------------
FROM ${BUILD_FROM}

# ffmpeg is only used by go2rtc to offer H.264 to WebRTC/RTSP viewers.
RUN apk add --no-cache \
    # renovate: datasource=repology depName=alpine_3_24/python3
    python3=3.14.8-r0 \
    # renovate: datasource=repology depName=alpine_3_24/py3-paho-mqtt
    py3-paho-mqtt=1.6.1-r4 \
    # renovate: datasource=repology depName=alpine_3_24/pulseaudio-utils
    pulseaudio-utils=17.0-r7 \
    # renovate: datasource=repology depName=alpine_3_24/alsa-utils
    alsa-utils=1.2.15.2-r1 \
    # renovate: datasource=repology depName=alpine_3_24/libturbojpeg
    libturbojpeg=3.1.3-r0 \
    # renovate: datasource=repology depName=alpine_3_24/ffmpeg
    ffmpeg=8.1.2-r0 \
    # renovate: datasource=repology depName=alpine_3_24/jq
    jq=1.8.2-r0

ARG BUILD_VERSION=1.0.0-beta4
ARG BUILD_ARCH=amd64
LABEL io.hass.version="${BUILD_VERSION}" \
    io.hass.type="addon" \
    io.hass.arch="${BUILD_ARCH}" \
    org.opencontainers.image.source="https://github.com/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App"

COPY --from=builder /usr/local/bin/psvr-capture /usr/local/bin/go2rtc /usr/local/bin/
RUN psvr-capture --help > /tmp/psvr-help && grep -q -- '--audio' /tmp/psvr-help && rm /tmp/psvr-help

WORKDIR /app
COPY LICENSE THIRD_PARTY_NOTICES.md /app/licenses/
COPY licenses/ /app/licenses/
COPY --from=builder /src/c/rnnoise/COPYING /app/licenses/RNNoise-BSD-3-Clause.txt
COPY src/python/psvr-services/ /app/src/
COPY assets/ /app/assets/

RUN mkdir -p /etc/s6-overlay/s6-rc.d/sony-playstation-vr-camera /etc/s6-overlay/s6-rc.d/user/contents.d \
    && echo 'longrun' > /etc/s6-overlay/s6-rc.d/sony-playstation-vr-camera/type \
    && touch /etc/s6-overlay/s6-rc.d/user/contents.d/sony-playstation-vr-camera
COPY run.sh /etc/s6-overlay/s6-rc.d/sony-playstation-vr-camera/run
RUN chmod +x /etc/s6-overlay/s6-rc.d/sony-playstation-vr-camera/run
