<p align="center">
  <img src="assets/mediabridge-icon.png" alt="MediaBridge logo" width="156">
</p>

<h1 align="center">MediaBridge</h1>

**MediaBridge** is a self-hosted directory uploader for moving large collections
of files from a native Qt desktop application to private storage over
[Tailscale](https://tailscale.com/). It pairs a lightweight Go receiving service
with a browser-based administration panel.

**Status:** Deployed and in active use. MediaBridge supports real-world uploads
and resumable transfers, but is still under active development. It is a
self-hosted project, not a turnkey commercial file-transfer service.

## Features

- **Native Qt 6 desktop application:** select a directory, monitor progress,
  pause or resume transfers, and use the system's widget theme and palette.
- **Live statistics:** transfer speed in MiB/s, active elapsed time and
  estimated time remaining (ETA).
- **Collapsible live charts:** a five-minute throughput history graph and a
  Free Download Manager-style, color-coded chunk map showing confirmed,
  active, retrying and pending chunks. Both views are collapsed by default.
  Mouse over a chunk cell to see its index (or chunk range for large files).
- **Configurable parallelism:** 1–24 simultaneous 4 MiB chunks per file;
  manual mode defaults to four. Optional adaptive mode starts at four,
  progressively probes higher concurrency, and returns to the best observed
  setting when throughput plateaus or declines. Multiple approved desktops
  may upload independently.
- **Resumable uploads:** server-side chunk tracking survives interruptions and
  restarts. Transient network errors are retried automatically.
- **Stalled-request recovery:** chunk-body idle deadlines release abandoned
  upload slots. Before retrying a disconnected PUT, the desktop checks the
  server's read-only chunk status, avoiding duplicate 4 MiB bodies when an
  earlier request is still running. An already-confirmed chunk is never counted
  again as new transfer throughput.
- **File integrity:** SHA-256 verification before completed files appear at
  their destination.
- **No destructive synchronization:** existing destination files are skipped,
  not overwritten; clients cannot delete remote files.
- **Private data path:** uploads run through Tailscale, not through the
  administration website, Cloudflare Tunnel or SSH.
- **Simple device approval:** each desktop requests access once; an
  administrator approves or revokes it through the WebUI.

The desktop is currently built and tested on Linux with Qt 6. Other operating
systems are not an explicit project target.

## Architecture

```text
Desktop (Qt 6) -- Tailscale --> MediaBridge Receiver (Go) --> storage dataset
                                       |
                        device approval and upload state

Browser -- protected management proxy --> MediaBridge WebUI
                                       |
                         Tailscale enrollment and approvals
```

The receiver and WebUI are separate HTTP listeners. The receiver is reachable
through a private Tailscale TCP forwarding rule; its HTTP port should not be
published directly on a public or untrusted network.

The WebUI is a **management interface only**. It manages Tailscale login and
logout, reports service status, and approves or revokes desktop devices.
It does not upload, browse, download or delete files. Its visual design uses
square corners throughout (`border-radius: 0`).

## Desktop workflow

1. Install and sign in to Tailscale on the desktop.
2. Open MediaBridge Desktop and enter the receiver's Tailnet address, such as
   `http://<tailnet-ip>:8081` (or an equivalent MagicDNS name).
3. Click **Request access**.
4. Open the protected management WebUI and click **Approve** for the pending
   desktop. Each desktop is approved independently; access remains valid until
   revoked.
5. Select a folder, choose a manual parallel-chunk count or enable **Adaptive
   parallelism**, and click **Start upload**.

The selected directory name and its subdirectories are preserved at the
destination. Repeating an interrupted upload reuses matching server-side
progress. Existing completed files are skipped, **without verifying that their
contents match the local copy**.

The desktop bypasses system HTTP proxy settings for direct Tailnet requests.
Its speed display uses a rolling window of newly acknowledged payload bytes;
previously uploaded chunks do not inflate the reported rate. Elapsed time
excludes manual pauses. ETA is an estimate based on recent throughput and may
fluctuate as the network changes.

Click **Transfer speed history (last 5 min)** or **Chunk map (current file)**
to expand or collapse either live view. The speed graph samples acknowledged
upload throughput once per second; it marks pauses and file verification as
gaps, and retains five minutes of history even while collapsed. The chunk map
uses the receiver's resume bitmap plus in-flight and retry state; previously
uploaded chunks appear as complete immediately, without being transferred
again. Large files aggregate contiguous chunk ranges into display cells to
avoid excessive UI objects or per-chunk widget allocation. Rendering uses
Qt's native palette and does not affect the server or transfer protocol.

Adaptive parallelism uses short, warmed-up measurement windows of actual
acknowledged payload throughput. It begins with four concurrent chunks and
tries 6, 8, 10, 12, 16, 20, then 24 while improvements remain significant.
If another step fails to improve throughput sufficiently, the desktop settles
at the best tested concurrency instead of keeping unnecessary connections.
It also discards partial measurements across pauses and file transitions.
These are online heuristics, not a guaranteed globally optimal benchmark;
short files may finish before a meaningful comparison is possible.

The desktop **caches a completed adaptive measurement separately for each
receiver URL**, in its existing local configuration. A subsequent upload or
application launch reuses the measured concurrency without probing again.
Cached results expire after 30 days; sustained large throughput regressions
or repeated chunk failures can trigger a fresh probe. The **Re-probe** button
explicitly clears the current receiver's cache: during an upload it immediately
restarts probing from four lanes (existing in-flight requests finish normally);
when idle the next upload performs a fresh measurement. No server-side
settings, chunks, or already uploaded files are changed by this button.

## Device authorization

MediaBridge does not require a separate username or password on each desktop.

Each desktop generates a random device identifier and bearer credential on
first pairing. The credential is kept in a user-only local configuration file;
the server stores only its SHA-256 hash. A new request initially has
`pending` status and cannot upload until an administrator approves it.

The WebUI supports multiple approved devices. Revoking a device denies future
upload requests. The revoked desktop must request access again with a new
device identity.

**Important:** The management WebUI deliberately has **no built-in login
form**. Protect it upstream with an authenticated access gateway, such as
Cloudflare Access, and restrict access to its origin. The server also supports
an immediate-peer IP allowlist through `MB_ADMIN_ALLOWED_CIDRS`. Verify how
the reverse proxy and container networking affect the observed source IP.
Never expose an unauthenticated management endpoint directly to the internet.
Use Tailnet ACLs/grants to limit access to the receiver as an additional layer.

## Building from source

Requirements:

- C++20, CMake 3.20+ and Qt 6 (Core, Widgets, Network, Concurrent) for Desktop
- Go 1.26+ for Server
- Tailscale for the private transport

Build the native desktop:

```sh
cmake -S desktop -B build/desktop -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop --parallel
./build/desktop/mediabridge-desktop
```

Build and test the Go receiver:

```sh
go test ./...
mkdir -p build
go build -o build/mediabridge-server ./server
```

The desktop uses the system Qt style, fonts and palette rather than replacing
them with an application-wide stylesheet.

## Self-hosting the server

A [TrueNAS SCALE](https://www.truenas.com/truenas-scale/) Custom App Compose
example is provided in
[`deploy/truenas-compose.example.yaml`](deploy/truenas-compose.example.yaml).
The server image is built through GitHub Actions and published as
`ghcr.io/nuaptanevalisk/mediabridge-server`.

The example includes two containers:

- **Tailscale sidecar:** a persistent node that can be enrolled through the
  WebUI. Running `tailscaled` directly keeps management available before
  the initial Tailnet login.
- **MediaBridge receiver:** a Go service that maintains approval records and
  receives data in a writable mounted directory. Once Tailscale is running,
  it configures private TCP forwarding to the receiver.

To adapt the example:

1. Create persistent storage for the Tailscale node state and MediaBridge
   server state.
2. Set the host-path mount for the destination dataset, and verify file
   ownership, group membership and inherited ACLs for your media services.
   The current image and template contain deployment-specific UID/GID
   assumptions; review them before using a different dataset.
3. Configure a dedicated management origin reachable from your authenticated
   reverse proxy. Update the source-IP allowlist for that proxy and confirm
   that direct, unauthorized access to the management endpoint is denied.
4. Deploy the Custom App through TrueNAS-supported management mechanisms.
   Verify `/dev/net/tun`, Tailscale state persistence and the shared LocalAPI
   socket.
5. Enroll the Tailscale node from the WebUI. Ensure the receiver's data port
   is accessible only to the intended Tailnet clients.
6. Approve a desktop and test with a small, noncritical file. Check that the
   destination service can read it and that incomplete staging files are
   excluded from any media-library scans.

The receiver runs with a restricted capability set: `CHOWN` for media file
ownership, `FOWNER` for idempotent metadata changes on previously committed
staging files, and `DAC_OVERRIDE` to publish into directories that permit
owner writes but not group writes. These permissions apply within the
container's mounts; review the mounted paths and container security model
before deployment. MediaBridge does not recursively change dataset ACLs.

The server defaults to management port `8080` and receiver port `8081`
inside the application. Container host port mappings, IP addresses, paths and
hostnames are **deployment-specific**. The example Compose file is a template
and must be reviewed before use.

## Transfer behavior and limitations

- The current desktop processes files sequentially, but uploads chunks of an
  individual file in parallel. Different desktops can transfer concurrently.
- Manual pause stops scheduling new requests; in-flight requests may finish.
- The receiver tracks completed chunks and supports restarting transfers from
  persistent staging files, including compatible older upload sessions.
- Integrity is checked before publishing a completed file. A destination with
  the same path is never automatically replaced.
- There is no remote file browser, deletion operation, two-way sync, conflict
  resolution, per-device quota or server-side request rate limiter.
- The current server uses a fixed allowed browser Origin for management POST
  requests. A different management hostname requires updating that check in
  the server source and rebuilding the image.
- Some deployment settings are intentionally specialized for TrueNAS and may
  require adaptation for other storage systems.

## License

MediaBridge is licensed under the **GNU General Public License, version 3
only**. See [LICENSE](LICENSE).
