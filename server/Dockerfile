# ABOUTME: tt7-server image: the Go server built static, run as a non-root user next to Debian trixie's Chromium.
# ABOUTME: Build context is server/ (server/compose.yaml does this); config and tokens are mounted at /config.

# Pinned by digest (Docker Hub, 2026-09-28): golang:1.26.6-trixie and debian:trixie-20260918-slim.
FROM golang:1.26.6-trixie@sha256:b75d466dd608587fd66cca705a307ba65b889827d06ad61d6a75f0482b51b7c7 AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY cmd ./cmd
COPY internal ./internal
# No cgo in the server, so a static binary with no libc dependency.
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/tt7-server ./cmd/tt7-server

FROM debian:trixie-20260918-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a
# Chromium comes unpinned on purpose: Debian security replaces old chromium versions in its
# archive, so a pinned version stops building. Each build takes trixie-security's current one;
# upgrading is a rebuild (server/README.md "Docker").
# Fonts: Noto for Latin/Greek/Cyrillic and most other scripts, Noto Color Emoji, Noto CJK for
# Chinese/Japanese/Korean, Liberation for pages that ask for Arial/Times/Courier by name.
# hadolint DL3008 (pin apt versions) is ignored for the reason above.
# hadolint ignore=DL3008
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      chromium \
      fonts-noto-core fonts-noto-color-emoji fonts-noto-cjk fonts-liberation \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# The container user. Its uid/gid must be able to read the token files (mode 600) and write
# the config directory; 1000 is the owner's user on the build host and on docker-host.
ARG TT7_UID=1000
ARG TT7_GID=1000
RUN groupadd --gid "$TT7_GID" tt7 \
 && useradd --uid "$TT7_UID" --gid tt7 --create-home --home-dir /home/tt7 --shell /usr/sbin/nologin tt7

COPY --from=build /out/tt7-server /usr/local/bin/tt7-server
USER $TT7_UID:$TT7_GID
WORKDIR /home/tt7
EXPOSE 7788
# Asks the running server's admin API (GET /api/screens with the admin token) whether it answers.
HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
  CMD ["tt7-server", "-config", "/config/screens.toml", "-healthcheck"]
ENTRYPOINT ["tt7-server"]
CMD ["-config", "/config/screens.toml"]
