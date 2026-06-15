# mcp-keep

A lifecycle layer for MCP. `mcp-keep` fronts one or more upstream MCP servers on a single local port and **keeps their tools surfaced to your client even while an upstream is offline** — then silently re-attaches when it comes back.

```
MCP client (Claude Code) → mcp-keep :8089 → upstream MCP server(s)
```

## Why

Other MCP hubs aggregate servers they *control* — they spawn the process and watch its files. `mcp-keep` does the opposite: it **attaches to a backend it doesn't control** (an editor, game engine, or app you launch yourself) and stays resilient to that backend's lifecycle.

The result is two things no other hub does:

- **Cache-when-down** — `mcp-keep` captures each upstream's tool list and serves it from disk. Open your client before the backend is running, or while it's restarting, and the tools are *still there*. Calls return a clear "start your server, then retry" instead of the tool vanishing.
- **Attach-not-spawn** — the backend's lifecycle is its own. `mcp-keep` connects when it can, re-attaches when the backend returns, and never tries to own or restart it.

Aggregating several MCP servers behind one port is table stakes; `mcp-keep` does that too. The point is what happens when one of them isn't there.

Bearer-token auth is supported as an optional passenger — injected per-upstream when a backend needs it — but it is not the headline. This is a resilience layer, not an auth proxy.

## Quick start

Python 3.10+, standard library only — no dependencies:

```bash
git clone https://github.com/exetorius/mcp-keep
cd mcp-keep/python
python proxy.py
```

On first run `mcp-keep` creates its home at `~/.mcp-keep/` and asks whether to start with your OS. To configure an upstream, either talk to it through your MCP client (the `keep_install_pack` tool) or edit `~/.mcp-keep/config.json` directly (see below).

Point your MCP client at `http://127.0.0.1:8089/mcp`.

## Where things live

`mcp-keep` is a single global install, outside any project. Everything lives under one home directory (override with the `MCP_KEEP_HOME` environment variable):

```
~/.mcp-keep/
  config.json                              — your upstreams + settings
  registry.json                            — optional known-packs overrides
  integrations/<name>/                     — an installed pack
  integrations/<name>/.cache/manifest.json — the captured tool list (the cache-when-down store)
```

Projects only carry a one-line pointer in their `.mcp.json` — never a copy of the relay.

## Configuration

`~/.mcp-keep/config.json` is a list of upstreams driven through one master port:

```json
{
  "listen_port": 8089,
  "max_body_bytes": 4194304,
  "allowed_origins": [],
  "capture_interval_seconds": 30,
  "upstreams": [
    {
      "name": "my-server",
      "host": "127.0.0.1",
      "port": 8088,
      "path": "/mcp",
      "bearer_token": "",
      "integration": ""
    }
  ]
}
```

| Key | Description |
|-----|-------------|
| `listen_port` | The single master port your client connects to (default 8089) |
| `max_body_bytes` | Reject request bodies larger than this with `413` (default 4 MB) |
| `allowed_origins` | Browser `Origin` values permitted to call the relay. Empty by default; each entry is an exact origin (`scheme://host:port`, no path). Adding one lets that web origin drive the relay — a deliberate security decision. |
| `capture_interval_seconds` | How often the background loop re-attaches to each upstream |
| `upstreams[].name` | *Your* local label for the upstream — cache key, routing handle, status display. Pick anything. |
| `upstreams[].host` / `.port` / `.path` | Where the upstream MCP server lives |
| `upstreams[].bearer_token` | Optional. Injected as `Authorization: Bearer <token>` on requests to *this* upstream only. |
| `upstreams[].integration` | Optional pack name in `integrations/` adding tool hints, synthetic tools, and instructions |

`mcp-keep` always binds to loopback (`127.0.0.1`). Reaching it from another machine is intentionally not a config toggle — see [issue #4](https://github.com/exetorius/mcp-keep/issues/4).

## Claude Code setup

Add to `.mcp.json` in your project root:

```json
{
  "mcpServers": {
    "mcp-keep": {
      "type": "http",
      "url": "http://127.0.0.1:8089/mcp"
    }
  }
}
```

Start a new session to pick up the tools.

## Integration packs

Packs add server-specific behaviour without touching relay code — tool hints, synthetic tools, and agent instructions for a specific upstream. They live in their own repo, [mcp-keep-integrations](https://github.com/exetorius/mcp-keep-integrations), and are fetched on demand.

Install one through your MCP client by calling the `keep_install_pack` tool (it lists available packs with no argument, installs with `name='<pack>'`), or from the terminal:

```
/keep-packs    — browse and install packs
/keep-status   — upstreams, reachability, cached tool counts
/keep-setup    — start-with-OS preference
/keep-reload   — reload config + packs without restarting
/keep-quit     — stop
```

## How it works

- `initialize` — answered by `mcp-keep` directly; never forwarded. Protocol version echoed back as sent.
- `tools/list` — aggregated from every upstream's **on-disk cache** plus pack hints/synthetic tools and `mcp-keep`'s own management tools. Works with every upstream offline.
- Capture loop — in the background, `mcp-keep` does an `initialize` + `tools/list` handshake against each upstream (probing for `401` to detect required auth), writes the result to that upstream's cache, and learns its identity from `serverInfo.name`.
- `tools/call` — routed to the owning upstream by tool name, with that upstream's bearer token injected. Clear error if the upstream is down.
- Security — always-on, zero-config: requests must carry a loopback `Host`, any browser `Origin` must be in the allowlist, and bodies over the size cap are rejected. These defend against DNS-rebinding and are not loosenable without an explicit opt-in.
- SSE heartbeat — a GET `/mcp` stream emits a comment every 15s to stop clients reconnecting in a tight loop.

## Contributing

PRs welcome — please target the `contrib` branch, not `main`. Pack contributions go to [mcp-keep-integrations](https://github.com/exetorius/mcp-keep-integrations).

## Repository layout

```
python/proxy.py       — the relay (single file, stdlib only)
config.example.json   — config template, safe to commit
CLAUDE.md             — setup-assistant guide for Claude Code
```
