---
name: unreal-umg
description: Build or change a HUD, menu or screen as a Widget Blueprint - the widget tree, text, fonts, colours, layout, gamepad and keyboard navigation and animations in one undoable plan with HyperAI's UI tools, then the Blueprint logic that plays it. Use for any UI work - main and pause menus, HUDs, health bars, dialogs, prompts, settings screens.
---

# A screen in one plan

HyperAI's UI tools are in toolset `HyperAIStudioUI.HyperAIStudioUIToolset`, called with `{"request": {...}}`.
`hyper_ui_apply_plan` builds or changes a whole Widget Blueprint in one call: up to 64 ops, each checked against the
Widget Blueprint as the earlier ops leave it, applied as one undo step, compiled, checked again, and saved.

## The loop

1. **Make the asset** if it does not exist: Epic's `CreateWidgetBlueprint` (toolset `UMGToolSet.UMGToolSet`) with
   `{"folderPath": "/Game/UI", "assetName": "WBP_MainMenu", "parentClass": {"refPath": "/Script/UMG.UserWidget"}}`.
   Its widget tree starts empty.
2. **Look** at an existing one: `hyper_ui_inspect` with `targetPaths: ["/Game/UI/WBP_MainMenu.WBP_MainMenu"]` lists every
   widget with its class, parent, text, colours and `navigation_*` rules, every slot's layout, and the animations.
3. **Build** with one `hyper_ui_apply_plan`: `targetPath`, `bDryRun: false`, a new `operationId`, and `operations`. The
   reply waits for the edit and ends `completed`. A wrong op is refused before anything changes, saying which op and
   why (a missing parent, a value out of range, a widget that holds no children); fix it and send again.
   `bDryRun: true` checks a plan without changing anything.
4. **Validate** with `hyper_ui_validate` (`targetPaths`): the layout, accessibility, animations and bindings. Aim for no
   errors and no warnings.
5. **Look at it** full size with `hyper_ui_render_review` (below), and fix what it finds.
6. **Wire it** in the event graph with the unreal-blueprint skill: play the intro, focus the first button, and handle
   clicks (below).

Close the Widget Blueprint's editor tab before a plan, so it changes in one place.

## Ops

Each op is `{"kind", "subjectId", "name", "stringValue", "secondaryName"}`; unused fields stay empty.

| kind | subjectId | name | stringValue | secondaryName |
|---|---|---|---|---|
| `widget.add` | the new widget's name | | its class: `CanvasPanel`, `VerticalBox`, `HorizontalBox`, `Overlay`, `SizeBox`, `ScaleBox`, `Border`, `Button`, `TextBlock`, `RichTextBlock`, `Image`, `ProgressBar`, `Slider`, `CheckBox`, `Spacer`, `ScrollBox`, `UniformGridPanel`, `GridPanel`, `WidgetSwitcher`, or a Widget Blueprint path | the parent panel; empty for the root |
| `widget.set_property` | widget | `text`, `font_size`, `justification`, `auto_wrap`, `visibility`, `is_enabled`, `accessible_text`, `tool_tip`, `hint_text`, `image`, `percent`, `value`, `min_value`, `max_value`, `checked_state`, `padding`, `width_override`, `height_override`, `render_opacity`, `render_scale`, `render_translation`, `render_angle`, `is_focusable`, `is_variable` | the value | |
| `widget.set_style` | widget | `color_and_opacity`, `brush_color`, `background_color`, `fill_color_and_opacity`, `content_color`, `shadow_color` | `#RRGGBB`, `#RRGGBBAA` or `r,g,b,a` from 0 to 1 | |
| `widget.set_slot` | widget | `padding`, `h_align`, `v_align`, `fill`; in a GridPanel `row`, `column`, `row_span`, `column_span`; in a CanvasPanel `anchors`, `offsets`, `position`, `size`, `alignment`, `auto_size`, `z_order` | the value | |
| `widget.set_navigation` | widget | `up`, `down`, `left`, `right`, `next`, `previous` or `all` | `explicit`, `wrap`, `stop` or `escape` | explicit: the widget to focus |
| `animation.create` | | the animation's name | | |
| `animation.add_float_track` | widget | a float property: `RenderOpacity`, `Percent`... | keys: `"0=0, 0.3=1"` (seconds=value) | the animation |
| `animation.add_transform_track` | widget | `translation_x`, `translation_y`, `angle`, `scale_x`, `scale_y`, `shear_x`, `shear_y` | keys | the animation |

`animation.create` takes its length in `numberValue` with `bHasNumberValue: true` (seconds). A track eases in and out
with `bHasBoolValue: true, bBoolValue: true`; without, it is linear. Keys must lie inside the animation's length.

Values: text as written (it is saved as localizable text); enum words `visible`, `collapsed`, `hidden`,
`hit_test_invisible`; `left`, `center`, `right`, `fill` (and `top`, `bottom` for `v_align`); margins as `12`, `0,12`
(horizontal, vertical) or `left,top,right,bottom`; `fill` as `auto` or a share of the free space (`1`); `image` as a
texture or material path; vectors as `x,y`. Anchors take a preset - `top_left`, `top`, `top_right`, `left`, `center`,
`right`, `bottom_left`, `bottom`, `bottom_right`, `top_fill`, `center_fill`, `bottom_fill`, `left_fill`, `middle_fill`,
`right_fill`, `fill` - or `minx,miny,maxx,maxy`; with point anchors `offsets` is `x,y,width,height`.

Rules the plan checks:
- Names are identifiers (letters, digits, `_`), unique among the widgets and animations.
- A Button, Border or SizeBox holds one child. A Button's label is a TextBlock inside it: add one and set its `text`.
- `anchors`, `offsets`, `position`, `size`, `alignment` and `auto_size` are for widgets in a CanvasPanel; `fill` for
  those in a VerticalBox or HorizontalBox.
- Leave `expectedElementFingerprint` empty. Renaming, moving and removing widgets are Epic's `RenameWidget`,
  `MoveWidget` and `RemoveWidget`.

## A main menu

A centred column of buttons on a canvas, a gold title, gamepad navigation that wraps, and a 0.35 s fade and slide:

```json
[{"kind": "widget.add", "subjectId": "Root", "stringValue": "CanvasPanel"},
 {"kind": "widget.add", "subjectId": "Menu", "stringValue": "VerticalBox", "secondaryName": "Root"},
 {"kind": "widget.set_slot", "subjectId": "Menu", "name": "anchors", "stringValue": "center"},
 {"kind": "widget.set_slot", "subjectId": "Menu", "name": "alignment", "stringValue": "0.5,0.5"},
 {"kind": "widget.set_slot", "subjectId": "Menu", "name": "auto_size", "stringValue": "true"},
 {"kind": "widget.add", "subjectId": "Title", "stringValue": "TextBlock", "secondaryName": "Menu"},
 {"kind": "widget.set_property", "subjectId": "Title", "name": "text", "stringValue": "AETHERFALL"},
 {"kind": "widget.set_property", "subjectId": "Title", "name": "font_size", "stringValue": "48"},
 {"kind": "widget.set_property", "subjectId": "Title", "name": "justification", "stringValue": "center"},
 {"kind": "widget.set_style", "subjectId": "Title", "name": "color_and_opacity", "stringValue": "#E8C170"},
 {"kind": "widget.set_slot", "subjectId": "Title", "name": "padding", "stringValue": "0,0,0,24"},
 {"kind": "widget.add", "subjectId": "PlayButton", "stringValue": "Button", "secondaryName": "Menu"},
 {"kind": "widget.set_slot", "subjectId": "PlayButton", "name": "padding", "stringValue": "0,6"},
 {"kind": "widget.set_slot", "subjectId": "PlayButton", "name": "h_align", "stringValue": "fill"},
 {"kind": "widget.set_property", "subjectId": "PlayButton", "name": "accessible_text", "stringValue": "Start a new game"},
 {"kind": "widget.add", "subjectId": "PlayLabel", "stringValue": "TextBlock", "secondaryName": "PlayButton"},
 {"kind": "widget.set_property", "subjectId": "PlayLabel", "name": "text", "stringValue": "Play"},
 {"kind": "widget.set_property", "subjectId": "PlayLabel", "name": "font_size", "stringValue": "24"},
 "... the same five ops for QuitButton and QuitLabel ...",
 {"kind": "widget.set_navigation", "subjectId": "PlayButton", "name": "down", "stringValue": "explicit", "secondaryName": "QuitButton"},
 {"kind": "widget.set_navigation", "subjectId": "QuitButton", "name": "up", "stringValue": "explicit", "secondaryName": "PlayButton"},
 {"kind": "widget.set_navigation", "subjectId": "QuitButton", "name": "down", "stringValue": "wrap"},
 {"kind": "widget.set_navigation", "subjectId": "PlayButton", "name": "up", "stringValue": "wrap"},
 {"kind": "animation.create", "name": "Intro", "bHasNumberValue": true, "numberValue": 0.35},
 {"kind": "animation.add_float_track", "subjectId": "Menu", "name": "RenderOpacity", "stringValue": "0=0, 0.35=1", "secondaryName": "Intro"},
 {"kind": "animation.add_transform_track", "subjectId": "Menu", "name": "translation_y", "stringValue": "0=30, 0.35=0",
  "secondaryName": "Intro", "bHasBoolValue": true, "bBoolValue": true}]
```

Other layouts: a HUD element in a corner is anchored to it (`anchors` `top_left`, `offsets` `32,32,300,40`); a
full-screen background is `anchors` `fill` with `offsets` `0,0,0,0`; a health bar is a `ProgressBar` with `percent` and
`fill_color_and_opacity`; rows side by side are a `HorizontalBox` with `fill` `1` on the part that stretches.

## Wiring it

The Blueprint logic lives in the Widget Blueprint's event graph; write it with the unreal-blueprint skill's
`write_graph_dsl` on `{"refPath": "/Game/UI/WBP_MainMenu.WBP_MainMenu:EventGraph"}`. Widgets are variables named
`Variables|<Blueprint name>|Get<Widget>` and animations `Variables|Animations|Get<Animation>`. Bind a button's click
first with Epic's `BindToEventProperty` (`widgetBlueprint`, `eventName: "OnClicked"`, `propertyName: "QuitButton"`,
`propertyClass: {"refPath": "/Script/UMG.Button"}`); its event is then `OnClicked(QuitButton)`:

```
(event UserInterface|EventConstruct
  (UserInterface|Animation|PlayAnimation :InAnimation (Variables|Animations|GetIntro))
  (Widget|SetKeyboardFocus :self (Variables|WBP_MainMenu|GetPlayButton)))

(event OnClicked(QuitButton)
  (Game|QuitGame))
```

Focusing the first button on Construct is what makes a menu usable with a gamepad. Look up any other node with
`find_node_types` (`Game|OpenLevel(byName)`, `Widget|RemovefromParent`, `UserInterface|Animation|PlayAnimationReverse`).

## The polish loop: render, compare, fix, render again

`hyper_ui_render_review` draws the Widget Blueprint off screen at real screen sizes and reviews what it drew. It is
read-only: the PNGs go to `Saved/HyperAIStudio/UIReview`, nothing in the project changes.

```json
{"targetPath": "/Game/UI/WBP_MainMenu.WBP_MainMenu", "resolutions": ["1920x1080", "1280x720", "2560x1440"],
 "referenceImagePath": "Saved/References/MainMenu_mockup.png", "sessionId": "main-menu", "note": "first pass"}
```

- **Sizes and scale.** Each size is drawn at the project's DPI curve for it, as the game would (`dpiScale` overrides).
  1280x720 shrinks the UI the most: it is where text gets too small and layouts run out of room. `bFromPlay: true`
  draws the instance on screen in Play In Editor, with its live text and state.
- **Open the PNGs** (`renders[].imagePath`) and look: the checks catch measurable faults, not taste.
- **Issues**, errors first, each with the widget's path, the size it was seen at and its rect in pixels:
  `text_clipped` (the text wants more room than its slot gives: let the slot size to content, wrap it, or shorten it),
  `text_too_small` (glyphs under `minTextPixels`, 14 by default), `low_contrast` (text against what is drawn behind it,
  under 4.5:1, or 3:1 for large text; set `backdrop` near the game's view behind the UI), `offscreen`,
  `outside_safe_zone` (the outer 5% a TV may crop; `safeZone` 1 checks only the edge), `clipped_by_parent` (a panel
  that clips cuts it off), `overlapping_interactive` (two buttons a tap can confuse), `not_focusable`,
  `broken_navigation` (an explicit rule to a widget that is not there) and `no_initial_focus` (no Desired Focus Widget,
  so a gamepad starts nowhere).
- **Against a mock-up.** With `referenceImagePath` (an image inside the project), the first render is scored against it
  with the image-match compare (aligned): `reference.score` 0-100 against `targetScore` (80), `heatMapPath` (where they
  differ, brightest where most; open it), `sideBySidePath`, the worst `worstRegions` and `suggestions`. Keep one
  `sessionId` for the whole loop, with a `note` per pass, so the history shows which change helped.

The loop: render and review; fix the worst issue or region with one `hyper_ui_apply_plan`; render again in the same
session; stop when there are no errors, the warnings are ones you accept, and the score reaches the target or stops
rising for three passes.

## Quality

- Layout scales from 1280 x 720 to 3840 x 2160: anchor to the edge or centre an element belongs to, and keep important
  content 5% in from each edge.
- Body text 18 point or more, 14 at the least; one font family; spacing in multiples of 4.
- Every button, slider and checkbox reachable by gamepad and keyboard in layout order, with `accessible_text`.
- Screens animate in over 0.15 to 0.4 seconds.
- `hyper_ui_validate` reports no errors or warnings, and `hyper_ui_render_review` no errors at 1280x720, 1920x1080
  and 2560x1440.
