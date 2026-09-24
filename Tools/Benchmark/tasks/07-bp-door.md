Build a door that opens for the player as a Blueprint in this Unreal project, as fast as you can: the goal is under three minutes.

`/Game/Bench/BP/BP_Door`: an Actor Blueprint with:
- a Static Mesh component named `Door` showing any mesh, for example `/Engine/BasicShapes/Cube` scaled like a door
- a Box collision component, the trigger, around the door and big enough to walk into (an extent of at least 100 on each axis)
- when a Pawn enters the trigger, `Door` turns smoothly to a yaw of 90 degrees over about one second, not in one jump
- when the Pawn leaves the trigger, `Door` turns smoothly back to a yaw of 0

Compile and save the Blueprint. Before you finish, confirm it compiles and is saved.
