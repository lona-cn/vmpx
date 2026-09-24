FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive \
    DISPLAY=:99 \
    VMPX_HOME=/opt/vmpx \
    VMPX_PORT=11451

RUN dpkg --add-architecture i386 \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        novnc \
        websockify \
        wine \
        wine64 \
        wine32 \
        x11vnc \
        xvfb \
    && rm -rf /var/lib/apt/lists/*

COPY docker/vmpx-wine/entrypoint.sh /usr/local/bin/vmpx-wine-entrypoint
RUN chmod 0755 /usr/local/bin/vmpx-wine-entrypoint

WORKDIR /opt/vmpx
EXPOSE 6080 11451
VOLUME ["/opt/vmpx"]
ENTRYPOINT ["/usr/local/bin/vmpx-wine-entrypoint"]
