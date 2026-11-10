Build a lit, symmetric signal courtyard level in this Unreal project.

- Create the level `/Game/Bench/Level/L_Bench_Courtyard` containing:
  - an enclosed square courtyard about 3000 x 3000 units, with a floor and walls, and an opening in the middle of each wall
  - a signal pylon at the centre
  - props arranged mirror-symmetrically across the plane X = 0, meaning every prop at (x, y) has a matching prop at (-x, y)
- Lighting:
  - a directional light as the sun, a sky light, and a sky atmosphere
  - two matching point lights on the pylon, one on each side of X = 0
- Add two camera actors:
  - `CAM_Front`: an eye-level view into the courtyard through an opening
  - `CAM_Top`: a top-down view of the whole courtyard
- Save the level.

Before you finish, confirm that the layout is symmetric about X = 0, confirm the level is lit, and check both camera views.
