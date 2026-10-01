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

- [x] **(asked) Rigid bodies with real gravity.** *Done in `vpyphys` (2026-10-01), with
  rotation: boxes tip and tumble, balls roll. Inertia is a scalar.* Position, velocity, mass,
  restitution, friction, in Q16, stepped at a fixed rate (semi-implicit Euler).
  Resting contact and "sleep" so a pile of boxes stops costing CPU. 2D first,
  then 3D on vpy3d transforms. Costs no strokes; CPU only.
- [x] **(asked) Collision detection.** *Spheres, turning boxes (all fifteen axes, edge against
  edge included) and checked convex hulls in `vpyphys` (2026-10-01).* Sphere and AABB first; convex-vs-convex
  (SAT) next, reusing the convex hulls the occluder already builds. A uniform grid
  as the broad phase. Contact points and normals out, so the physics and the
  effects below can use them.
- [x] **(idea) Ray casts.** *`vpyp_raycast` for bodies (hulls too); `vpy3d_ray_mesh` for any mesh,
  dented or morphed as it is now (2026-10-01).* Ray vs sphere, box and mesh, returning the hit point,
  normal and face. Shots, line of sight, the lightgun (`examples/lightgun_test`).
- [x] **(idea) Constraints: ropes, chains, hinges.** *Ropes and chains in `vpyrope`; ball
  joints and hinges between physics bodies in `vpyphys` (2026-10-01). No angle limits or motors.* Verlet points joined by
  distance constraints. A rope is a polyline, which is the cheapest thing a vector
  display can draw: a 12-link chain is 12 chained strokes.

## Impact and destruction

- [x] **(asked) Deformation on impact.** *`vpy3d_mesh_copy` + `vpy3d_mesh_dent` (2026-10-01);
  in `physics_demo`, hard contacts dent crates.* A hit pushes a mesh's vertices in along
  the impact direction with a falloff from the contact point, and the dent stays.
  Per-instance vertex offsets over a shared mesh, so a hundred identical crates do
  not need a hundred meshes. Costs no strokes: the same edges, displaced. Needs the
  face normals rebuilt after a dent so hidden-line removal stays right.
- [x] **(asked) Shots that mark what they hit.** *A dent and a mark (`vpy3d_marks`: a scorch
  ring or a crack, kept in the object's space) where a shot lands (2026-10-01). Holes are not
  planned: the occluder has no polygons with holes, so what is behind could not show through.* Ray cast + deformation: a dent, a
  scorch ring drawn on the face, or a hole (the face's outline split around it).
- [x] **(idea) Shatter.** *`vpyfx_shatter` (2026-10-01): every edge a spinning stick.* At a strong enough impact the mesh breaks into pieces:
  each piece a small convex mesh, or simply each EDGE becomes a spinning stick with
  its own velocity — the classic vector explosion, and it costs exactly the edges
  the object already had, falling to zero as pieces fade.
- [ ] **(idea) Soft bodies.** Mass-spring meshes (jelly, flags, cloth, a wobbling
  blob). Same Verlet core as the ropes; the strokes are the springs.
- [x] **(idea) Shockwaves.** *`vpyp_blast` (the push) and `vpyfx_ring` (the ring), 2026-10-01.* An expanding ring that fades with radius and pushes
  bodies it passes. Cost: one ring of N strokes, N fixed.

## Particles and effects

- [x] **(idea) A particle system with a stroke budget.** *`vpyfx` (2026-10-01).* Sparks, debris, exhaust as
  short strokes along their velocity (motion blur for free). A hard cap per frame
  and LOW priority, so particles are the first thing shed, never the scenery.
- [x] **(idea) Camera shake and hit-stop.** *`vpycam` (2026-10-01).* Screen-space offset on impact, decaying;
  a few frames' freeze on a big hit. Both nearly free.
- [x] **(idea) Trails.** *`vpyfx_line`: one a frame behind a moving object, each fading.* Keep the last N positions of a fast object and draw them
  dimmer and dimmer. A vector display's natural afterimage.

## Animation

- [ ] **(idea) Skeletal animation.** Bones with rigid parts first (each limb a
  mesh on a joint — what kuroishi's figure does by hand), keyframes, blending
  between clips.
- [x] **(idea) Inverse kinematics.** *`vpyik_two_bone` (2026-10-01).* Two-bone IK for feet on uneven ground and arms
  reaching a target.
- [x] **(idea) Morphing.** *`vpy3d_mesh_blend` (2026-10-01).* Blend between two shapes with the same vertex count —
  a vector logo turning into a ship. Cost: the larger shape's edges.
- [x] **(idea) Tweening and easing.** *`vpyease` (2026-10-01).* A small library of fixed-point curves
  (ease in/out, overshoot, bounce) for UI, cameras and animation.

## 3D rendering

- [x] **(idea) Projected shadows.** *`vpy3d_shadow` (2026-10-01).* Project a solid's convex hull onto the floor
  plane from a light direction and draw it as a dim outline. The hull code exists
  (the occluder); a shadow is one more polygon per object.
- [x] **(idea) Level of detail.** *`vpy3d_lod_pick` / `vpy3d_draw_lod` over the game's meshes,
  by `vpy3d_screen_size` (2026-10-01).* Two or three versions of a mesh, picked by
  on-screen size, so distant objects cost fewer strokes.
- [x] **(idea) Depth cueing in the SDK.** *`vpy3d_fog`.* Brightness falling with distance, one
  call, instead of every game computing its own fog (hakaba does it by hand).
- [x] **(idea) Hidden-line terrain.** *`vpy3d_terrain` (2026-10-01): rows, floating horizon.* Heightmap landscapes with the floating-horizon
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
- [x] **(idea) Steering and pathfinding.** *`vpyai` (2026-10-01); flocking is separate +
  seek, alignment not a call of its own.* Seek, flee, arrive, separation (flocks
  of vector birds); grid A* for enemies in mazes.
- [x] **(idea) A camera system.** *`vpycam`: follow with dead zone and lead, shake, hit-stop;
  scripted paths not done.* Follow with look-ahead, dead zones, shake,
  scripted paths — hakaba's camera, made general.
- [x] **(idea) Deterministic replay.** *`vpyreplay` (2026-10-01).* Record inputs and a seed, play back
  frame-exact (kuroishi has one); the base for attract modes and ghost runs.

## Audio

- [ ] **(idea) Positional sound.** Pan on the stereo jack by screen position,
  volume by distance, a pitch shift for approaching objects. *The pan needs the
  UVMC2's jack (`uvm2_jack.c`); the debug cartridge has none.*
- [x] **(idea) Impact sounds from physics.** *`vpyimpact` (2026-10-02): synthesised on
  the PSG, no samples; volume from the impulse, voice from the material. Heard on one
  console's speaker; the voices are starting values.* Contact speed drives the volume and
  choice of sample, so a falling crate sounds as hard as it lands.

## Tooling (so the above can be debugged)

- [x] **(idea) A beam simulator.** *`uvm2-sdk/tools/beam_sim.py` (2026-10-01).* Decode a command list cycle by cycle and render
  what is lit to SVG, flagging anomalies (ramps with the clamp on, lit with no
  ramp). Written ad hoc to find the 2026-10-01 asterisk; worth making permanent.
- [x] **(idea) An official command-list dump** *— to the SD card, no probe: `uvm2_dump_list` in
  the `.um2`, and the debug cartridge's BIOS calls it on buttons 3+4; `list_from_sd.py` reads it
  (2026-10-01). Tried on the debug cartridge 2026-10-02: written, read back, hash right,
  replayed in beam_sim. The RTT dump stays.*, the same on both
  cartridges, with a script that reassembles the passes for the simulator.
- [ ] **(idea) A built-in diagnostics HUD** on a button combo: dropped, strokes,
  fps, `ramps_clamped`, over any game.
- [x] **(idea) Fail the BIOS build if it does not define the SDK's weak hooks.** *For
  `uvm2_core1_gap`, the one a BIOS needs: no default under `UVM2_BIOS` (2026-10-01).*
  A renamed hook silently disconnected the menu's music and SD (2026-10-01).
- [x] **(idea) Mend the six host tools that do not link** *— `tools/uvm2_host_stubs.c` and
  `tools/build_host_tools.sh` (2026-10-01); all eight build.* and build them all from
  one target, so they cannot rot unnoticed again.
- [ ] **(idea) Per-console screen shape in the calibration wizard.** Measure the
  visible window and the small aspect error per console, store them in
  `uvm2.cfg`, and let vpy3d use them.

---

## A suggested order

1. ~~**Collision + rigid bodies with gravity**~~ — done (`vpyphys`), rotation included.
   Edge against edge, convex hulls and joints followed.
2. ~~**Particles with a stroke budget**, then **shatter**~~ — done (`vpyfx`).
3. ~~**Deformation on impact**~~ — done; **shots that mark** done as dents and marks.
4. **The entity layer**, once there are enough systems to tie together.
