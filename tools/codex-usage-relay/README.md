# Codex usage relay

This Python user service polls the Codex usage endpoint and serves Clawdmeter `pv:"codex"` JSON at `/usage` on port 8765. It reads `~/.codex/auth.json` on each poll. It does not write or refresh tokens, because Codex CLI does not provide a shared file-lock contract for this service.

Copy this directory to `~/codex-usage-relay` on a Linux VM. Replace `100.0.0.0` in the service file with that VM's Tailscale IPv4 from `tailscale ip -4`, then install it as a systemd user service:

```sh
mkdir -p ~/.config/systemd/user
cp ~/codex-usage-relay/codex-usage-relay.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now codex-usage-relay
```

The relay has no application password, so bind it only to an interface reachable by trusted tailnet peers. The service returns 503 until a successful usage poll, and again after ten minutes without one.
