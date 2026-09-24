Build a score pickup as Blueprints in this Unreal project, as fast as you can: the goal is under three minutes.

- `/Game/Bench/BP/BP_BenchGameMode`: a Game Mode Base Blueprint with an integer variable `Score`, default 0.
- `/Game/Bench/BP/BP_Pickup`: an Actor Blueprint with:
  - a Static Mesh component showing any mesh, for example `/Engine/BasicShapes/Cylinder`
  - a Sphere collision component with a radius of about 100
  - a Rotating Movement component that spins it around its vertical axis (yaw)
  - an integer variable `Points`, default 10
  - when a Pawn starts to overlap it, it adds its `Points` to `Score` on the current Game Mode (a `BP_BenchGameMode`), then destroys itself
- Compile and save both Blueprints.

Before you finish, confirm both Blueprints compile and are saved.
