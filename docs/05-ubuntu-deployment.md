# Local and Ubuntu deployment plan

Status: design only. **Do not start any service or change networking in the current planning task.**

## Authorized destination and safeguards

The user permits this project's service on **port 8765** on the existing Ubuntu SSH host. Main development and testing happen on the Windows PC; final Ubuntu deployment comes after local release readiness. Earlier Ubuntu use is allowed only for temporary tests needed to answer a specific question that local testing cannot reasonably cover. Use the saved SSH connection and its existing key/host verification. Do not copy private keys or secrets into the repository or Markdown files.

The previously inspected host is an Oracle VM.Standard.A1.Flex instance with two ARM cores, 12 GB configured RAM, and 2 Gbps configured network bandwidth. These are historical observations, not a current load or throughput measurement. Minecraft and other existing workloads must remain running.

Use a dedicated project directory such as `/home/ubuntu/goodsharing`, project-owned process/service names, and explicit owned-process records for temporary tests. Prefer a native ARM64 SFU binary and a small control-service binary over extra infrastructure. A single-node first release does not require a distributed deployment.

## Local development first

Keep the application, capture/audio fixtures, integration, UI, load/soak, and packaged-build tests on the Windows PC. Use loopback-only bindings for temporary local signaling/control endpoints, preferably port 8765 if it is free. Run an official SFU through a supported local runtime where available, such as existing WSL/Docker. Verify the pinned release supports the chosen runtime; do not assume a native Windows server build exists.

If a necessary local fixture is unavailable, assess supported local alternatives first. An earlier temporary Ubuntu fixture may answer a concrete integration or networking question, but must have a stated purpose, bounded duration, owned-process cleanup, and an explicit result. Return to PC development afterward; do not make the VPS the routine development environment or leave a premature deployment running.

Use development credentials only for isolated local fixtures. Keep server signing credentials outside source and outside the desktop app. Do not expose a development-mode server to the Internet.

## Public port-8765 design

LiveKit signaling and its media transport are separate. A successful HTTP/WebSocket request to port 8765 does not prove video/audio connectivity.

Proposed minimal public layout:

| Listener | Proposed use | Exposure |
| --- | --- | --- |
| TCP 8765 | HTTPS/WSS reverse proxy: app health/join-token paths and LiveKit signaling/API routing | Public only when remote execution/deployment begins and TLS is ready. |
| UDP 8765 | LiveKit single-port ICE/UDP mux via `rtc.udp_port` | Public media path to validate with Windows clients. TCP and UDP may share the same numeric port. |
| Internal token service port | Private-room token issuance | Loopback only; choose a free internal port at implementation time. |
| Internal LiveKit signaling port | Reverse-proxy upstream, commonly 7880 | Private access only; never expose administrative/signing APIs unnecessarily. |
| Diagnostics/metrics | Project health and measurements | Loopback/private only. |

The reverse proxy is not a WebRTC media relay; UDP 8765 must reach the SFU directly. Configure no port range when using the single-port UDP mux. Initially disable the extra ICE/TCP listener instead of silently opening default TCP 7881.

Verify actual socket bindings for the selected LiveKit version. In particular, a global loopback bind may also bind the UDP mux to loopback, while a global public bind may expose internal HTTP/metrics listeners. Use supported per-listener bindings and explicit firewall isolation as needed; validate from both the host and an external client. Do not assume one global bind flag meets both goals.

Port 8765 is the user's requested public port. Planning assumes both TCP and UDP on that number for the first remote test, to be stated clearly when execution starts. Inspect both Ubuntu firewall and Oracle network rules; preserve existing rules and change only this project's exact listeners when deployment execution is authorized. No other public ports are part of the current plan.

UDP-blocked networks will not necessarily work with that minimal layout. If tests show a need for ICE/TCP or TURN/TLS, explain the concrete additional listener/proxy requirement and obtain the missing port/network preference before extending public exposure. Do not claim WebRTC media can use the HTTPS reverse proxy automatically.

## TLS and room access

A public endpoint should use a chosen hostname and trusted TLS certificate, yielding a client address such as `wss://<chosen-host>:8765`. The actual hostname and certificate provisioning remain unresolved. Do not bypass certificate validation or ship a disabled-validation option as the production solution.

Certificate issuance/renewal must fit the approved network layout. If the chosen method requires other public ports, select an appropriate alternative such as DNS-based validation or resolve the additional requirement with the user. Domain/DNS credentials are not present in this plan.

Use a small private-group access mechanism, such as per-user join credentials exchanged for short-lived room-scoped tokens. Store credentials only through appropriate local/server secret storage. The app receives participant tokens, not the SFU signing secret. Enforce allowed rooms and publication/subscription grants in the service; a hidden room name alone is not access control.

## Future deployment sequence

Start final deployment only after the packaged Windows client has passed the local readiness gate in [roadmap milestone 5](03-implementation-roadmap.md#5-windows-packaging-and-local-release-readiness). Temporary earlier diagnostics are the limited exception described above, not a reason to skip that gate.

1. Record current listeners, service state, free space, resource baseline, and relevant firewall rules using read-only inspection. A port collision is a reported conflict; do not terminate its owner.
2. Build and validate the control service and client locally; obtain a verified pinned ARM64 SFU release.
3. Prepare the full configuration, executable paths, environment/secret locations, listener table, health checks, and rollback steps before changing the running host.
4. Stage project files in the dedicated directory and validate configuration without replacing unrelated files.
5. Start project components with private bindings first and run authentication/signaling smoke tests.
6. Configure the approved TCP/UDP 8765 path and TLS, confirm advertised public ICE addresses, and test from Windows over the Internet.
7. Run bounded simultaneous-sharing tests while monitoring the SFU and existing workload separately.
8. Only after those checks, enable project-specific persistent services if the execution scope includes deployment. Keep operational logs and secrets private.

Existing user authorization for routine project hosting should be used when execution resumes; do not repeatedly ask to use the same host/port. Ask only when a genuinely missing input, unapproved exposure, disruptive action, or cost-changing action is required.

## Rollback

Stop only this project's processes/units, restore its previous configuration/version if one exists, and remove only network rules added for this project. Keep SSH, Minecraft, Mumble, remote desktop, and other services running. Never resize or reboot the VM as a performance workaround without a separate user decision.

Verify cleanup by process identity and resolved project paths. Preserve reports and useful diagnostics; do not recursively delete an unchecked computed path. No server rollback action is needed in the current planning turn because no service is being deployed.

## Sources

- [LiveKit ports, ICE/UDP mux, and firewall requirements](https://docs.livekit.io/transport/self-hosting/ports-firewall/)
- [LiveKit deployment, TLS, and public-IP configuration](https://docs.livekit.io/transport/self-hosting/deployment/)
- [Single-node LiveKit architecture](https://docs.livekit.io/reference/internals/livekit-sfu/)
