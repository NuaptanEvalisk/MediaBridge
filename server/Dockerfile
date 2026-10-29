# SPDX-License-Identifier: GPL-3.0-only
FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY server/ ./server/
RUN CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' -o /out/mediabridge-server ./server

# Includes the tailscale CLI used by the management WebUI. tailscaled itself
# lives in a separate sidecar with a shared Unix socket.
FROM tailscale/tailscale:v1.104.2
COPY --from=build /out/mediabridge-server /usr/local/bin/mediabridge-server
USER 0:568
ENTRYPOINT ["/usr/local/bin/mediabridge-server"]
