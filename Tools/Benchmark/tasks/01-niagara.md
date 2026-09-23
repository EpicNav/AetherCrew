Build a live signal beacon effect in this Unreal project.

- Create the Niagara System `/Game/Bench/Niagara/NS_SignalBeacon` with exactly two emitters:
  - `Core`: a warm, bright core (orange to yellow) at the centre.
  - `Halo`: a cool, soft halo (cyan to blue) surrounding the core.
  Both emitters must spawn particles continuously and loop forever.
- Create the level `/Game/Bench/Niagara/L_Bench_Niagara`. Place the system in it at the world origin as an actor labelled `Beacon`.
- Add a camera actor labelled `CAM_Beacon`, about 600 units from the beacon and looking at it.
- Compile and save everything.

Before you finish, confirm that both emitters are live, report their particle counts, and check the result visually from `CAM_Beacon`.
