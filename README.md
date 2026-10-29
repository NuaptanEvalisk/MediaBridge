# MediaBridge

MediaBridge is a small, GPLv3-licensed, private directory uploader for a single
openSUSE Tumbleweed desktop and a TrueNAS SCALE Jellyfin media dataset.

**Status: development prototype; not deployed.** The local Go and Qt builds are
implemented. The TrueNAS/Tailscale application stack still requires a staging
deployment and end-to-end validation before production use.

## Intended environment

- Desktop: Chelonia, openSUSE Tumbleweed, Qt 6 Widgets, native application.
- Server: TrueNAS SCALE 25.10.4, Custom App with Tailscale and MediaBridge.
- Storage: `/mnt/tank/data/media`, mounted as `/media` in the receiver.
- Media permissions: UID 33, GID 568 (`apps`), preserving the existing POSIX
  ACL design. Existing Jellyfin and FileBrowser Apps remain unchanged.
- Management: an Access-protected hostname through the ZBook Cloudflare Tunnel,
  then the TrueNAS LAN IP on port 30079.
- Data: Chelonia's existing Tailscale connection directly to the new
  `media-ingest` Tailnet node on TCP port 8081. No Cloudflare or SSH transfer.

## Authentication

There are **no MediaBridge usernames or passwords**.

1. Desktop generates an independent cryptographically random 128-bit device
   identifier and 256-bit bearer credential, saved in its local user-only
   configuration file (`~/.config/Evalisk/MediaBridge/identity.json`, exact
   location follows Qt's `AppConfigLocation`).
2. Desktop submits a pairing request to the private receiver endpoint.
3. WebUI shows the pending device name and identifier. An administrator clicks
   **Approve**. The device can upload until revoked.
4. WebUI can **Revoke** an approved Desktop, immediately denying subsequent
   uploads. Revoked devices must generate new credentials and request approval.
5. Multiple Desktop devices can be approved independently.

The server stores **SHA-256 hashes of credentials**, never plaintext bearer
credentials. Tailscale encrypts the transport. Limit access to receiver TCP 8081
with Tailnet ACL/grants. Local credentials are not a substitute for securing the
desktop machine itself.

### WebUI trust boundary

The administrative WebUI has **no application-level login**. Cloudflare Access
must enforce authentication **before** traffic can reach the origin. The
server also checks the immediate peer IP against `MB_ADMIN_ALLOWED_CIDRS`, by
default allowing only loopback. Supply the expected ZBook proxy address in
the production Compose configuration.
Never publish port 8080 on the internet, enable Cloudflare bypass policies,
or expose it directly to untrusted LAN clients. The source-IP restriction must
be tested behind the actual Docker port publishing path; Docker NAT may affect
the source address seen by the application.

The WebUI is strictly for connecting/re-authenticating/logging out the Tailscale
node, viewing basic health, and approving/revoking Desktop devices. It cannot
browse, upload, download or delete media. **All WebUI elements use 0px corners.**

## Upload semantics

- Recursive directory transfer preserves the selected top-level folder name.
- Chunked upload (4 MiB client chunks; 8 MiB server maximum).
- Durable server-side partial files allow resume after client/server restart.
- SHA-256 verification before the file becomes visible at its final path.
- Atomic no-overwrite publication using a hard link on the same ZFS dataset.
- The hidden `.mediabridge-staging` directory contains a `.ignore` marker for
  Jellyfin; verify scanner behavior before deployment.
- No remote deletion or automatic replacement of existing media files.
- Existing targets are skipped by the Desktop with an explicit log entry; this
  **does not prove** the remote file is identical.
- `status.json` and the existing TrueNAS apps are not modified.

## Local build

Build the Go server:

```sh
go test ./...
go build -o build/mediabridge-server ./server
```

Build the Chelonia Qt desktop:

```sh
cmake -S desktop -B build/desktop -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop --parallel
./build/desktop/mediabridge-desktop
```

The server has separate management (default `:8080`) and receiver (default
`:8081`) HTTP listeners. Do not use the development defaults on an untrusted
network. The receiver is intended for traffic encrypted by Tailscale.

## TrueNAS deployment (not yet performed)

1. Prepare dedicated persistent application paths under
   `/mnt/tank/apps/mediabridge/` through TrueNAS-supported mechanisms.
2. Build and publish the server image from `server/Dockerfile`; replace the
   image placeholder in `deploy/truenas-compose.example.yaml`.
3. Install as a TrueNAS Custom App using the Compose template. Verify kernel
   TUN support, socket permission and Tailscale CLI login/logout from the WebUI.
4. Make `mbridge.evalisk.org` an Access-protected Cloudflare Tunnel hostname
   routed by ZBook to `http://192.168.88.11:30079`.
5. Verify the management-source CIDR allowlist and deny access from other LAN
   hosts; do not publish the receiver TCP 8081 on TrueNAS LAN.
6. Approve the Chelonia Desktop in the WebUI and test a noncritical small file
   before any multi-gigabyte media transfers.
7. Confirm Jellyfin can read new files and that partial files are not scanned.

The example runs MediaBridge as container UID 0 with GID 568 so that it can
operate the root-owned Tailscale LocalAPI and assign correct UID/GID to newly
created media files. Only the media and dedicated application state directories
are mounted, and the container drops all capabilities except `CHOWN`. This
trade-off should be revisited before treating the app as production-hardened.

## Current limitations

- Upload queue runs sequentially within one Desktop instance. Different
  clients/uploads may transfer concurrently.
- Pausing happens after the current chunk.
- Network interruptions are resumed by reselecting the same directory and
  starting a new queue; server identifies matching incomplete uploads.
- No server-side per-device quota or request rate limiter yet.
- No automatic conflict resolution, file deletion, or directory browsing.
- No automated server image publication, Tailnet enrollment, or Cloudflare
  provisioning. None of the infrastructure changes have been applied.

## License

GNU General Public License version 3 only. See [LICENSE](LICENSE).
