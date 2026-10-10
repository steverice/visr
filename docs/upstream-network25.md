# Networking 25 upstream update

Compared against OpenCommunityEdition/OpenCE main at
`b5a4870d05fe172e75e3dcd1b3b3e305e84aa214` on 2026-10-09.
VISR was already on networking 24. This update imports the networking 25
payload behavior and selected networking fixes, rather than merging the
entire upstream tree.

Imported commits:

- `6b55f5cca`: network version 25.
- `767a4edb0`: host object states carry integrated light presence and state;
  locally controlled players and vehicles retain their predicted light state.
- `1f87b4ff6`: newly created client units start with zero grenades until the
  host's inventory arrives.
- `22fe5628b`: host teleports of remote players retain the destination.
- `792711ca5`: grenade shock-wave and explosion reports share the same throw.
- `44da50e41`: ignore negative co-op activating clusters.
- `ab3c388c1`: connecting resets stale/closed connection flags and queued data.
- `2db317436`: banned and cheating machines receive the blacklisted rejection
  reason. Adapted to the existing ban paths, without the upstream vote system.

Upstream voice chat, voting, platform-specific changes, and unrelated rendering
changes are not part of this update. Version 25 peers have the same object-state
light flags; older protocol versions remain subject to the existing version gate.

Validation: full `ninja ios_guest`, `python3 tools/ios_test.py`, and the actual-C
light-state probe in `tools/test_network_light_state.py`. Device multiplayer
retesting is still required: join a version 25 host, toggle another player's
flashlight before/after joining, check local flashlight response, teleport a
remote player, and throw grenades immediately after spawning.
