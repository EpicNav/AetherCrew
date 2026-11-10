Build a target that takes damage, dies and comes back as a Blueprint in this Unreal project, as fast as you can: the goal is under three minutes.

`/Game/Bench/BP/BP_Target`: an Actor Blueprint with:
- a Static Mesh component showing any mesh
- a float variable `Health`, default 100
- when it takes damage (Apply Damage from anywhere), `Health` goes down by the damage, never below 0
- when `Health` reaches 0 it dies: it is hidden in game and its collision is disabled
- 2 seconds after dying it respawns: `Health` is 100 again, it is visible, and its collision is enabled

Compile and save the Blueprint. Before you finish, confirm it compiles and is saved.
