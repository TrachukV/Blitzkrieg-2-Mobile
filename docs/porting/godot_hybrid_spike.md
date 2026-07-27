# Godot hybrid port spike

## Decision

Do not rewrite the game in Godot. Preserve the original C++ single-player
simulation, database, mission scripting, pathfinding, and save compatibility.
Evaluate Godot only as a presentation shell for rendering, UI, and input.

A full rewrite would duplicate the riskiest parts of this project while the
asset pipeline is still incomplete. In particular, the proprietary Granny
animation dependency is not recovered by changing engines.

## Boundary introduced by this spike

`bk2_presentation_api.h` is a C ABI with no Android, bgfx, or Godot types. The
current Android runtime publishes:

- decoded terrain vertices and triangle indices;
- the current static-world marker mesh;
- dynamic AI entity IDs, players, state flags, positions, headings, and HP;
- mission ID, world bounds, and a monotonically increasing generation.

Readers copy data from an internal mutex-protected snapshot. A reader should
read `Bk2PresentationSnapshotInfo`, allocate buffers, copy the arrays, then
read the info again. If the generation changed, it retries the snapshot.

The JSON writer is a diagnostic transport and a desktop spike input. It is not
the intended per-frame production transport.

## Go/no-go gate

Continue with a Godot Android library/GDExtension only if one real mission can
demonstrate all of the following:

1. terrain and static-world parity with the current bgfx path;
2. at least one animated unit using a legally available asset path;
3. selection and a move command round trip into the original C++ simulation;
4. stable frame pacing and memory use during a 20-minute device run;
5. no new blocker for packaging the original content.

If the spike cannot beat the bgfx path on iteration speed without failing these
checks, keep bgfx and use the same presentation API to decouple it from the
simulation.
