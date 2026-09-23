Create a styled signal-relay UI panel in this Unreal project.

- Create the Widget Blueprint `/Game/Bench/UI/WBP_SignalRelay`. Its root panel is a dark charcoal bordered panel named `Panel`, containing:
  - a title text `SIGNAL RELAY` in a cyan accent colour
  - a status text reading `LINK STABLE`
  - a progress bar showing 75%, with a cyan fill
- Add a widget animation named `Pulse`, one second long, that animates `Panel`'s render opacity from 0.3 at the start to 1.0 at the end.
- Compile and save.

Before you finish, confirm the Widget Blueprint compiles, confirm the `Pulse` animation animates the opacity, and check the visual result.
