FROM --platform=linux/amd64 debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251

ENV DEBIAN_FRONTEND=noninteractive \
    DISPLAY=:99 \
    VMPX_HOME=/opt/vmpx \
    VMPX_PORT=11451

RUN dpkg --add-architecture i386 \
    && rm -f /etc/apt/sources.list /etc/apt/sources.list.d/* \
    && printf '%s\n' \
        'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260919T000000Z/ bookworm main' \
        'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260919T000000Z/ bookworm-updates main' \
        'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/20260919T000000Z/ bookworm-security main' \
        > /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        novnc \
        passwd \
        tini \
        websockify \
        wine \
        wine64 \
        wine32 \
        x11vnc \
        xvfb \
    && rm -rf /var/lib/apt/lists/*

COPY docker/vmpx-wine/entrypoint.sh /usr/local/bin/vmpx-wine-entrypoint

RUN groupadd --system --gid 10001 vmpx \
    && useradd --system --uid 10001 --gid vmpx --home-dir /opt/vmpx --no-create-home vmpx \
    && mkdir -p /opt/vmpx/data \
    && chown -R vmpx:vmpx /opt/vmpx \
    && chmod 0755 /usr/local/bin/vmpx-wine-entrypoint

WORKDIR /opt/vmpx
EXPOSE 6080 11451
ENTRYPOINT ["/usr/bin/tini", "-g", "--", "/usr/local/bin/vmpx-wine-entrypoint"]
USER 10001:10001
