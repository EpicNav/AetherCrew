Create an animated pulse-grid surface in this Unreal project.

- Create the material `/Game/Bench/Material/M_PulseGrid`:
  - a charcoal base colour
  - a cyan grid of thin lines over it
  - an orange pulse that sweeps across the surface over time, so the material visibly animates without any Blueprint or game code
- Create the level `/Game/Bench/Material/L_Bench_Material`:
  - a flat plane static mesh actor labelled `SM_Plane` at the world origin, using `M_PulseGrid`
  - a camera actor labelled `CAM_Material` looking at the plane
- Compile and save everything.

Before you finish, confirm the material compiles, confirm it is assigned to `SM_Plane`, and prove from `CAM_Material` that the surface animates.
