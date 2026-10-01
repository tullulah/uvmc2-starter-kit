# TODO — capabilities worth adding to the SDK

Ideas for what games could do next, not a commitment. Each entry says why it suits
a vector display, what it costs, and what it builds on.

The constraints every entry is written against:

* **Strokes are the budget.** A full command list is worth ~940 strokes at 50 Hz
  (`vpy.c`, `VPY_SB_MAX`). Anything that multiplies geometry has to say how many
  strokes it adds, and degrade by priority (`vpy_set_priority`) rather than drop.
* **Integer only.** libvpy and vpy3d are fixed point (Q14 trig, Q16 parameters).
  The images build with `-mfloat-abi=soft`; new maths stays fixed point.
* **No depth buffer.** Anything that hides anything goes through the occluder
  (`vpy3d_occl_*`), near to far.
* **Deterministic.** Replays and the host harness depend on the same inputs giving
  the same frame. No wall-clock time in simulation; one seeded PRNG.

Legend: **[ ]** not started · **[x]** done · **[~]** partly done · **(asked)** requested · **(idea)** proposed.

---

## Physics

- [~] **(asked) Rigid bodies with real gravity.** *Done in `vpyphys` (2026-10-01) except
  rotation: boxes stay axis-aligned.* Position, velocity, mass,
  restitution, friction, in Q16, stepped at a fixed rate (semi-implicit Euler).
  Resting contact and "sleep" so a pile of boxes stops costing CPU. 2D first,
  then 3D on vpy3d transforms. Costs no strokes; CPU only.
- [~] **(asked) Collision detection.** *Sphere and AABB done in `vpyphys`; convex-vs-convex
  (SAT) still to do.* Sphere and AABB first; convex-vs-convex
  (SAT) next, reusing the convex hulls the occluder already builds. A uniform grid
  as the broad phase. Contact points and normals out, so the physics and the
  effects below can use them.
- [x] **(idea) Ray casts.** *`vpyp_raycast`; vs mesh still to do.* Ray vs sphere, box and mesh, returning the hit point,
  normal and face. Shots, line of sight, the lightgun (`examples/lightgun_test`).
- [ ] **(idea) Constraints: ropes, chains, hinges.** Verlet points joined by
  distance constraints. A rope is a polyline, which is the cheapest thing a vector
  display can draw: a 12-link chain is 12 chained strokes.

## Impact and destruction

- [ ] **(asked) Deformation on impact.** A hit pushes a mesh's vertices in along
  the impact direction with a falloff from the contact point, and the dent stays.
  Per-instance vertex offsets over a shared mesh, so a hundred identical crates do
  not need a hundred meshes. Costs no strokes: the same edges, displaced. Needs the
  face normals rebuilt after a dent so hidden-line removal stays right.
- [ ] **(asked) Shots that mark what they hit.** Ray cast + deformation: a dent, a
  scorch ring drawn on the face, or a hole (the face's outline split around it).
- [ ] **(idea) Shatter.** At a strong enough impact the mesh breaks into pieces:
  each piece a small convex mesh, or simply each EDGE becomes a spinning stick with
  its own velocity — the classic vector explosion, and it costs exactly the edges
  the object already had, falling to zero as pieces fade.
- [ ] **(idea) Soft bodies.** Mass-spring meshes (jelly, flags, cloth, a wobbling
  blob). Same Verlet core as the ropes; the strokes are the springs.
- [ ] **(idea) Shockwaves.** An expanding ring that fades with radius and pushes
  bodies it passes. Cost: one ring of N strokes, N fixed.

## Particles and effects

- [ ] **(idea) A particle system with a stroke budget.** Sparks, debris, exhaust as
  short strokes along their velocity (motion blur for free). A hard cap per frame
  and LOW priority, so particles are the first thing shed, never the scenery.
- [ ] **(idea) Camera shake and hit-stop.** Screen-space offset on impact, decaying;
  a few frames' freeze on a big hit. Both nearly free.
- [ ] **(idea) Trails.** Keep the last N positions of a fast object and draw them
  dimmer and dimmer. A vector display's natural afterimage.

## Animation

- [ ] **(idea) Skeletal animation.** Bones with rigid parts first (each limb a
  mesh on a joint — what kuroishi's figure does by hand), keyframes, blending
  between clips.
- [ ] **(idea) Inverse kinematics.** Two-bone IK for feet on uneven ground and arms
  reaching a target.
- [ ] **(idea) Morphing.** Blend between two shapes with the same vertex count —
  a vector logo turning into a ship. Cost: the larger shape's edges.
- [ ] **(idea) Tweening and easing.** A small library of fixed-point curves
  (ease in/out, overshoot, bounce) for UI, cameras and animation.

## 3D rendering

- [ ] **(idea) Projected shadows.** Project a solid's convex hull onto the floor
  plane from a light direction and draw it as a dim outline. The hull code exists
  (the occluder); a shadow is one more polygon per object.
- [ ] **(idea) Level of detail.** Two or three versions of a mesh, picked by
  on-screen size, so distant objects cost fewer strokes.
- [ ] **(idea) Depth cueing in the SDK.** Brightness falling with distance, one
  call, instead of every game computing its own fog (hakaba does it by hand).
- [ ] **(idea) Hidden-line terrain.** Heightmap landscapes with the floating-horizon
  algorithm: rows drawn front to back, each clipped by the highest line so far. The
  natural occluder for terrain, which convex hulls are not.
- [ ] **(idea) 3D Imager support.** The Vectrex's own stereo goggles: draw a left
  and a right view in step with the spinning wheel. Real 3D, on this console only.
- [ ] **(idea) Text on 3D planes.** The vector font transformed like any geometry:
  signs, titles flying past the camera.

## Gameplay infrastructure

- [ ] **(idea) An entity/scene layer.** Objects with transform, mesh, body and
  collider; the SDK does near-to-far ordering and occluder registration, removing
  the ordering trap from the occluder.
- [ ] **(idea) Steering and pathfinding.** Seek, flee, arrive, separation (flocks
  of vector birds); grid A* for enemies in mazes.
- [ ] **(idea) A camera system.** Follow with look-ahead, dead zones, shake,
  scripted paths — hakaba's camera, made general.
- [ ] **(idea) Deterministic replay.** Record inputs and a seed, play back
  frame-exact (kuroishi has one); the base for attract modes and ghost runs.

## Audio

- [ ] **(idea) Positional sound.** Pan on the stereo jack by screen position,
  volume by distance, a pitch shift for approaching objects.
- [ ] **(idea) Impact sounds from physics.** Contact speed drives the volume and
  choice of sample, so a falling crate sounds as hard as it lands.

## Tooling (so the above can be debugged)

- [ ] **(idea) A beam simulator.** Decode a command list cycle by cycle and render
  what is lit to SVG, flagging anomalies (ramps with the clamp on, lit with no
  ramp). Written ad hoc to find the 2026-10-01 asterisk; worth making permanent.
- [ ] **(idea) An official command-list dump over RTT**, the same on both
  cartridges, with a script that reassembles the passes for the simulator.
- [ ] **(idea) A built-in diagnostics HUD** on a button combo: dropped, strokes,
  fps, `ramps_clamped`, over any game.
- [ ] **(idea) Fail the BIOS build if it does not define the SDK's weak hooks.**
  A renamed hook silently disconnected the menu's music and SD (2026-10-01).
- [ ] **(idea) Mend the six host tools that do not link** and build them all from
  one target, so they cannot rot unnoticed again.
- [ ] **(idea) Per-console screen shape in the calibration wizard.** Measure the
  visible window and the small aspect error per console, store them in
  `uvm2.cfg`, and let vpy3d use them.

---

## A suggested order

1. ~~**Collision + rigid bodies with gravity**~~ — done (`vpyphys`); **rotation**
   is what is left of it, and boxes that tumble are what shatter will want.
2. **Particles with a stroke budget**, then **shatter** — the most visible payoff,
   and the budget rules get settled once.
3. **Deformation on impact** and **shots that mark what they hit**.
4. **The entity layer**, once there are enough systems to tie together.
