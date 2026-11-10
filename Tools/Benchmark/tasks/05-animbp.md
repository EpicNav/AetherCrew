Create Manny locomotion animation assets in this Unreal project.

The Mannequin is at `/Game/Characters/Mannequins/Meshes/SK_Mannequin` (skeleton) and `/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple` (mesh). Its unarmed clips are under `/Game/Characters/Mannequins/Anims/Unarmed/`.

- Create the Blend Space `/Game/Bench/Anim/BS_Locomotion` for the `SK_Mannequin` skeleton.
  - One axis named `Speed`, from 0 to 500.
  - Samples: the idle clip at 0, a forward walk at about 200, and a forward jog at 500.
- Create the Animation Blueprint `/Game/Bench/Anim/ABP_Bench` for the same skeleton, containing:
  - a float variable `Speed`
  - a state machine with exactly two states:
    - `Idle`, which plays the idle clip
    - `Locomotion`, which plays `BS_Locomotion` driven by `Speed`
  - transitions: `Idle` to `Locomotion` when `Speed > 10`, and `Locomotion` to `Idle` when `Speed <= 10`
  - the state machine's pose connected to the output pose
- Compile and save both assets.

Before you finish, confirm both assets compile, confirm the Blend Space samples are valid, and confirm the state machine has both states and both transitions.
