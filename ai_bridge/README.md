# SM64 AI Control

SM64 AI Control turns the player into Mario's director and camera operator.
Instead of controlling Mario directly, type a natural-language instruction such
as `Run to the nearest red coin, but flee if a Goomba gets close.` The companion
asks an OpenAI model to convert that request into validated Nintendo 64
controller inputs. The game publishes state between actions so the model can
observe the result and continue when necessary.

The OpenAI API key and all network requests remain in the Python companion.
The game and companion communicate through small local files; no network code
runs inside the game process.

## Contents

- [Current features](#current-features)
- [Build and setup](#build-and-setup)
- [Starting the game and bridge](#starting-the-game-and-bridge)
- [Keyboard, mouse, and camera controls](#keyboard-mouse-and-camera-controls)
- [Natural-language commands](#natural-language-commands)
- [Available AI actions](#available-ai-actions)
- [Recognised objects](#recognised-objects)
- [Object tracking and conditional instructions](#object-tracking-and-conditional-instructions)
- [NPCs, signs, and native dialog](#npcs-signs-and-native-dialog)
- [Mario speech and personality](#mario-speech-and-personality)
- [Telemetry](#telemetry)
- [Status icons](#status-icons)
- [Companion terminal commands](#companion-terminal-commands)
- [Configuration](#configuration)
- [Local protocol reference](#local-protocol-reference)
- [Current limits and behaviour](#current-limits-and-behaviour)
- [Troubleshooting](#troubleshooting)

## Current features

- Open-ended natural-language instructions rather than a fixed phrase list.
- Validated stick, button, wait, speech, and live object-targeting steps.
- Up to eight observe-act cycles for multi-stage tasks and changing situations.
- World-locked movement, so moving the spectator camera does not redirect an
  action Mario has already started.
- Live steering toward or away from recognised moving objects.
- Stable object IDs, target sparkles, arrival checks, and stuck detection.
- Conditional behaviour based on nearby threats and refreshed observations.
- In-game command entry with safe replacement of an active instruction.
- Native-looking Mario speech, reactive personality, and post-dialog remarks.
- Automated sign and NPC dialog advancement.
- Free, Focus, Follow, and First-Person spectator camera modes.
- Custom HUD icons for Ready, Thinking, Acting, Error, and Cancelled.
- Mario's final sleep transition is disabled so an instruction is not wasted
  waking him and the idle camera does not alter its FOV.

## Build and setup

### Build the game on Windows

Use the **MSYS2 MinGW 64-bit** terminal, not the plain MSYS terminal. Place a
legally obtained US SM64 ROM named `baserom.us.z64` in the repository root,
then run:

```bash
make -j4 VERSION=us
```

The Windows executable is normally created at:

```text
build/us_pc/sm64.us.exe
```

To regenerate the extracted vanilla assets:

```bash
make distclean
make -j4 VERSION=us
```

`distclean` removes build output and ROM-extracted assets before rebuilding.
Custom source assets such as the AI status PNGs are not ROM-extracted assets.

### Install the Python companion

Use standard 64-bit Windows Python from PowerShell. MSYS2 Python may attempt to
compile `jiter` instead of installing its prebuilt Windows wheel.

From the repository root:

```powershell
py -3 -m venv .venv-win
./.venv-win/Scripts/python.exe -m pip install --upgrade pip
./.venv-win/Scripts/python.exe -m pip install -r ./ai_bridge/requirements.txt
$env:OPENAI_API_KEY = "your-key-here"
```

The API key assignment lasts for the current PowerShell session. Set it through
Windows environment-variable settings if it should persist.

## Starting the game and bridge

1. Start `build/us_pc/sm64.us.exe`.
2. Wait until Mario is in a playable area.
3. From PowerShell in the repository root, start the companion:

```powershell
./.venv-win/Scripts/python.exe ./ai_bridge/bridge.py
```

The bridge searches the repository root and `build/*` for the newest live game
state or configuration file. This supports launching from a terminal or by
double-clicking the executable. It prints the selected runtime directory when
it starts.

To test local transport without making an API request, run this while the game
is open:

```bash
python ai_bridge/send_demo.py
```

Mario should perform a deterministic run-and-jump sequence.

## Keyboard, mouse, and camera controls

### Command entry

| Input | Behaviour |
|---|---|
| `T` | Opens the command box. It does not close the box when typing T. |
| `Tab` | Opens or closes the command box. |
| `Enter` | Sends the typed instruction. |
| `Backspace` | Deletes the previous character. |
| `Escape` | Closes the command box without sending. |

The input box accepts up to 120 characters and spans the widescreen window.
Gameplay and camera shortcuts are suppressed while typing.

If another instruction is Thinking, Acting, or still inside its brief Done
window, submitting a new command opens a replacement confirmation:

| Input | Replacement behaviour |
|---|---|
| `Enter` | Cancel the previous instruction and send the new one. |
| `Escape` | Return to editing the new instruction. |
| `Tab` | Close the box and keep the old instruction. |

Late API responses belonging to a cancelled instruction are discarded, so
Mario cannot suddenly resume an obsolete plan.

### Spectator camera

| Input | Behaviour |
|---|---|
| `W` / `S` | Move along the camera's full view angle, including pitch. Looking down and pressing W moves toward the ground. |
| `A` / `D` | Strafe left/right while remaining level. |
| `Q` / `E` | Descend/ascend vertically. |
| Mouse movement | Look continuously. Horizontal mouse look is inverted. |
| Arrow keys | Keyboard camera look when normal gameplay UI is not active. |
| Physical stick | Alternative spectator movement. |
| Physical C buttons | Alternative spectator look. |
| `F` | Cycle Free -> Focus -> Follow -> First-Person. |
| `F1` | Toggle the spectator camera and return to/from vanilla camera. |

| Camera mode | Behaviour | HUD glyph |
|---|---|---|
| **Free** | Position and view are fully controlled by the player. | Camera |
| **Focus** | Holds position and automatically looks at Mario. Manual movement still repositions it. | Camera + Mario |
| **Follow** | Maintains its offset from Mario and keeps him centred. Manual movement changes the stored offset. | Camera + Lakitu |
| **First-Person** | Uses Mario's eye position. Look controls offset his facing direction; his model is hidden to prevent clipping. | Camera + C-Up |

Cutscenes and menus temporarily yield camera control to the normal game.
Leaving First-Person places the spectator camera behind Mario.

### Menus and native UI

Human A, B, and Z gameplay input is deliberately removed so only the AI can
perform Mario's actions. Keyboard and mouse remain available for UI control:

| Input | UI behaviour |
|---|---|
| `Enter` or left click | Confirm, select, or advance native dialog (A). |
| `Escape` | Back/dialog cancel (B), pause where appropriate, or emergency-cancel active AI input. |
| Arrow keys | Navigate menus with the virtual stick. |
| Physical Start | Emergency-cancel an executing AI sequence. |

## Natural-language commands

There is intentionally no finite list of phrases. The bridge sends the typed
instruction, current state, nearby recognised objects, and previous results to
the model. Different wording can express the same action.

### Basic movement

- `Run forward for two seconds.`
- `Walk backward briefly, then turn left.`
- `Run forward, jump, and keep moving through the jump.`
- `Crouch, then backflip.`
- `Punch three times.`
- `Long jump forward.`
- `Wait for a second, then jump.`

Complex moves use normal N64 inputs, so success still depends on Mario's
current action, speed, timing, and terrain.

### Object-directed movement

- `Run to the nearest red coin.`
- `Collect that blue coin.`
- `Approach the breakable box and punch it.`
- `Climb the nearest tree.`
- `Run away from the Bob-omb.`
- `Go to the cannon, but stop just outside it.`
- `Find the nearest star.`

### Conditional and multi-stage behaviour

- `Run to that red coin, but if a Goomba gets near you, run away from it.`
- `Head toward the tree. If the route is blocked, try jumping once.`
- `Approach the Bob-omb Buddy, talk to him, and react when he finishes.`
- `Get away from any threat within 700 units, otherwise continue to the coin.`

Conditions are evaluated from snapshots between control cycles, not by a
continuously running language model. Live target steering does update every
game frame.

### Speech and interaction

- `Say "Let's-a go!" then jump toward the coin.`
- `Tell me whether this looks dangerous.`
- `Complain about having to punch that box, then do it anyway.`
- `Read that sign and tell me what you think.`
- `Go and speak to the nearest Toad.`
- `Talk to the Bob-omb Buddy and respond when the conversation is over.`

The game reports whether a native dialog is open, but not its text. Closing
remarks are contextual and generic rather than true readings or quotations.

## Available AI actions

Natural language is converted into four validated step types:

| Step | Purpose | Rules |
|---|---|---|
| `input` | Apply virtual stick and/or N64 buttons for a number of frames. | Stick axes range from -80 to 80. |
| `wait` | Hold neutral controls. | No stick or buttons. |
| `say` | Show Mario speech while holding neutral controls. | At most one per plan; must be first; maximum 80 ASCII characters. |
| `target` | Steer toward or away from a recognised object by stable ID. | Speed 1-80; stop distance 0-8000; live steering. |

The game runs at 30 frames per second. A step can last 1-300 frames, a plan can
contain up to 32 steps, and one plan can request at most 900 total frames.

### Stick axes

| Value | Meaning at the plan's reference camera angle |
|---|---|
| `stick_y: 1..80` | Forward |
| `stick_y: -1..-80` | Backward |
| `stick_x: -1..-80` | Left |
| `stick_x: 1..80` | Right |
| `0, 0` | Neutral |

Ordinary movement is locked to the camera orientation recorded when the plan
was made. Moving the spectator camera afterward changes the view without
bending Mario's queued path.

### AI-accessible buttons

| Button | Typical SM64 use |
|---|---|
| `A` | Jump, swim stroke, confirm, advance native dialog |
| `B` | Punch, kick, dive, pick up/throw, initiate signs and NPC interactions |
| `Z` | Crouch, ground pound, and advanced move combinations |
| `R` | Vanilla camera modifier where applicable |
| `C_UP` | Vanilla C-Up/camera input |
| `C_DOWN` | Vanilla camera input |
| `C_LEFT` | Vanilla camera input |
| `C_RIGHT` | Vanilla camera input |

Button presses must be followed by a release step. The model can combine stick
and buttons to attempt dives, long jumps, backflips, wall kicks, and other
standard moves; these are not separate scripted commands.

## Recognised objects

Telemetry includes the nearest 20 recognised active objects within 8000 world
units, sorted by 3D distance. The exact `type` names below are what the model
receives.

### Collectibles and power-ups

| Telemetry type | Recognised object |
|---|---|
| `red_coin` | Red coin |
| `yellow_coin` | Static, moving, spawned, and temporary yellow coins |
| `blue_coin` | Moving, sliding, jumping, and Mr. I blue coins |
| `star` | Spawned collectible Power Star |
| `one_up` | Walking, fleeing, sliding, and jump-on-approach 1-Ups |
| `recovery_heart` | Recovery heart |
| `wing_cap` | Loose Wing Cap |
| `metal_cap` | Loose Metal Cap |
| `vanish_cap` | Loose Vanish Cap |
| `koopa_shell` | Rideable Koopa shell |

### Boxes and containers

| Telemetry type | Recognised object |
|---|---|
| `breakable_box` | Large wooden breakable box |
| `small_breakable_box` | Small breakable/throwable box |
| `pushable_metal_box` | Pushable metal box |
| `wing_cap_box` | Wing Cap exclamation box |
| `metal_cap_box` | Metal Cap exclamation box |
| `vanish_cap_box` | Vanish Cap exclamation box |
| `koopa_shell_box` | Koopa shell exclamation box |
| `coin_box` | Single-coin exclamation box |
| `three_coin_box` | Three-coin exclamation box |
| `ten_coin_box` | Ten-coin exclamation box |
| `one_up_box` | 1-Up exclamation box |
| `running_one_up_box` | Running 1-Up exclamation box |
| `star_box` | Star exclamation boxes, including star-number variants |
| `exclamation_box` | Unclassified exclamation-box content |

### World objects and interactables

| Telemetry type | Recognised object |
|---|---|
| `door` | Normal door, warp door, and star door |
| `tree` | Climbable tree |
| `cannon` | Open cannon |
| `closed_cannon` | Closed cannon |
| `sign` | Message panel and wall sign |
| `pole` | Normal climbable pole and giant pole |
| `warp_pipe` | Warp pipe |
| `wing_cap_switch` | Red Wing Cap switch |
| `metal_cap_switch` | Green Metal Cap switch |
| `vanish_cap_switch` | Blue Vanish Cap switch |
| `cap_switch` | Unclassified cap-switch parameter |
| `blue_coin_switch` | Blue coin switch |
| `purple_switch` | Object, grill, and hidden-object purple floor switch variants |

### NPCs

| Telemetry type | Recognised NPC |
|---|---|
| `bobomb_buddy` | Bob-omb Buddy, including cannon-opening behaviour |
| `toad` | Castle Toad message NPC |
| `racing_penguin` | Racing penguin |

### Enemies and threats

Every type below is emitted with `threat: true`.

| Telemetry type | Enemy |
|---|---|
| `goomba` | Goomba |
| `bobomb` | Black Bob-omb |
| `koopa` | Koopa Troopa |
| `chain_chomp` | Chain Chomp |
| `piranha_plant` | Piranha Plant |
| `boo` | Boo |
| `fly_guy` | Fly Guy |
| `chuckya` | Chuckya |
| `spindrift` | Spindrift |
| `scuttlebug` | Scuttlebug |
| `snufit` | Snufit |
| `skeeter` | Skeeter |

Other SM64 objects and enemies still exist normally, but are omitted from
`nearby_objects` and cannot yet be selected by a `target` step.

## Object tracking and conditional instructions

Each recognised object receives a stable ID for its active lifetime. A target
step uses the ID rather than committing to one stick direction:

- **Toward** steers every frame and ends at or below `stop_distance`.
- **Away** steers in the opposite direction and ends at or above
  `stop_distance`.
- The selected object emits a small sparkle about every ten frames.
- Moving less than 20 world units over about one second returns `stuck`.
- A disappearing or out-of-room object returns `target_lost`, unless Mario was
  already close enough to count as arrived.
- Reaching the frame budget returns `timeout`.

| `target_result` | Meaning |
|---|---|
| `none` | No target result for the current plan. |
| `tracking` | A target step is active. |
| `arrived` | Mario reached the requested approach distance. |
| `clear` | Mario reached the requested safe distance while fleeing. |
| `target_lost` | The target became invalid or disappeared. |
| `stuck` | Mario did not make enough progress. |
| `timeout` | The target step used all requested frames. |

Typical planning guidance is 40-80 units for collectibles, 100-160 for boxes,
trees, and signs, 180-300 for enemies, and 700-1500 when fleeing. These are not
hard-coded restrictions.

After a plan, the bridge can report the measured result to the model and ask
for another plan. This enables conditions and corrections, but observation
happens between bursts rather than continuously.

## NPCs, signs, and native dialog

The normal planned interaction is:

1. Track toward the recognised sign or NPC.
2. Briefly press and release B to begin interaction.
3. Observe `dialog.active`.
4. While dialog is active, remain still and briefly press/release A once per
   observation cycle.
5. When telemetry reports that dialog closed, optionally display one final
   Mario remark.

The bridge can run up to eight observe-act cycles for the whole instruction.
Very long conversations can therefore hit the safety cap. Telemetry contains
dialog state and numeric ID, but not the displayed message text.

## Mario speech and personality

A `say` step displays `MARIO: ...` using SM64's dialog glyphs and a translucent
panel. It holds neutral controls and must be first in a plan. Speech is limited
to printable ASCII and 80 characters, normally 2-12 words and 45-75 frames.

Mario is prompted to be upbeat, brave, friendly, eager to impress, and lightly
theatrical, with restrained Mario-style phrasing. Explicit wording requested by
the player takes priority.

Spontaneous reactions are limited to the first observe-act cycle so retries do
not repeat an introduction. One contextual closing remark is allowed after an
NPC or sign dialog closes.

While speaking, Mario turns only his head toward the spectator camera. His body
facing and next movement direction remain unchanged. He acknowledges the camera
only inside the 160-degree field of view in front of him (80 degrees each side),
and the head turn is disabled in First-Person mode.

## Telemetry

The game refreshes `.sm64-ai-state.json` about twice per second.

| Field | Meaning |
|---|---|
| `level`, `area` | Numeric level and area IDs. |
| `position` | Mario's world X/Y/Z position. |
| `velocity` | Mario's X/Y/Z velocity. |
| `forward_speed` | Current forward velocity. |
| `facing_yaw` | Signed 16-bit facing angle. |
| `camera_yaw` | Angle used to interpret camera-relative controls. |
| `action` | Raw Mario action value. |
| `grounded` | Outside air/swimming/metal-water actions. |
| `swimming` | In swimming or metal-water movement. |
| `cutscene` | In the cutscene action group. |
| `dialog.active`, `dialog.id` | Native dialog visibility and ID. |
| `health` | Raw SM64 health value. |
| `environment.floor_height` | Floor height beneath Mario. |
| `environment.height_above_floor` | Mario Y minus floor height. |
| `environment.floor_type` | Raw floor surface type. |
| `environment.floor_normal_y` | Floor normal's vertical component; lower means steeper. |
| `environment.touching_wall` | Whether Mario has a wall collision. |
| `environment.water_level` | Current water surface height. |
| `target_result` | Latest target outcome. |
| `nearby_objects` | Up to 20 recognised objects within 8000 units. |

Each nearby object contains:

| Field | Meaning |
|---|---|
| `id` | Stable target ID, currently 1-240. |
| `type` | Exact recognised type from the tables above. |
| `distance` | 3D distance from Mario. |
| `horizontal_distance` | X/Z distance from Mario. |
| `height_delta` | Object Y minus Mario Y. |
| `threat` | Whether it is classified as an enemy threat. |
| `position` | Object world X/Y/Z position. |
| `stick_toward` | Legacy camera-relative hint; live plans use `target`. |

Result summaries include horizontal distance moved, height change, health
change, target outcome, and native-dialog transitions when available.

## Status icons

The top-left icon indicates bridge state. Mario's life counter is shifted right
to make room.

| State | Asset | Meaning |
|---|---|---|
| Ready | `assets/ai-ready.png` | Waiting for an instruction. |
| Thinking | `assets/ai-thinking.png` | Waiting for a model plan. |
| Acting | `assets/ai_acting.png` | Executing queued steps. |
| Complete | Ready icon | Finished; returns to Ready after about two seconds. |
| Error | `assets/ai-error.png` | API failure or rejected packet. |
| Cancelled | `assets/ai-cancelled.png` | Instruction cancelled or replaced. |

## Companion terminal commands

| Command | Behaviour |
|---|---|
| Any other text | Send a natural-language instruction. |
| `/status` | Show bridge status, latest position, and up to eight nearby objects. |
| `/help` | Print the terminal command list. |
| `/quit` or `/exit` | Stop the Python companion. |

In-game and terminal instructions use the same planner.

## Configuration

### Environment variables

| Variable | Used by | Purpose |
|---|---|---|
| `OPENAI_API_KEY` | Python | Required OpenAI API key. |
| `OPENAI_MODEL` | Python | Model name; defaults to `gpt-5.6-luna`. |
| `SM64_AI_RUNTIME_DIR` | Python | Override automatic runtime-directory detection. |
| `SM64_AI_COMMAND_FILE` | Game | Override command packet path. |
| `SM64_AI_STATE_FILE` | Game | Override telemetry path. |
| `SM64_AI_STATUS_FILE` | Game | Override status path. |
| `SM64_AI_REQUEST_FILE` | Game | Override in-game request path. |

The Python command-line options are:

```text
--model MODEL
--command-file PATH
--state-file PATH
--status-file PATH
--request-file PATH
```

View generated help with:

```powershell
./.venv-win/Scripts/python.exe ./ai_bridge/bridge.py --help
```

## Local protocol reference

| Runtime file | Direction | Purpose |
|---|---|---|
| `.sm64-ai-request` | Game -> Python | Natural-language instruction entered in-game. |
| `.sm64-ai-state.json` | Game -> Python | Current telemetry snapshot. |
| `.sm64-ai-status` | Both | Ready/Thinking/Executing/Complete/Error/Cancelled. |
| `.sm64-ai-command` | Python -> Game | Validated plan or status packet. |

These runtime files must not be committed. Writes use a temporary file followed
by a rename so readers do not see partial data.

### Packet grammar

```text
SM64AI1
REFERENCE_YAW <signed-angle>
SAY <frames> <uppercase-hex-encoded-ASCII>
INPUT <stick-x> <stick-y> <button-mask> <frames>
WAIT <frames>
TARGET <object-id> <direction> <speed> <button-mask> <frames> <stop-distance>
END
```

- `REFERENCE_YAW` is optional and appears before steps.
- `SAY` is optional, unique, and must be the first step.
- Target direction is `1` for toward or `-1` for away.
- Button masks use the N64 bits exposed by `ai_bridge/protocol.py`.
- A status packet contains `STATUS READY`, `STATUS THINKING`, or `STATUS ERROR`
  instead of control steps.

The protocol is internal transport. `protocol.py` and the game both validate
its limits, so malformed packets become Error rather than partially executing.

## Current limits and behaviour

- Full AI, keyboard, and free-camera functionality targets the PC port. N64
  builds receive harmless stubs.
- At most 20 recognised objects within 8000 units reach the model.
- One instruction receives at most eight observe-act cycles.
- A plan has at most 32 steps and 900 frames; a step has at most 300 frames.
- The model sees structured state, not rendered pixels, collision maps, native
  dialog text, or names for objects outside the recognised list.
- Target steering takes a direct horizontal route. It does not pathfind around
  walls, pits, slopes, or platforms.
- Stick-only movement can end early after Mario remains stationary. Waits,
  releases, and button-bearing inputs keep their requested timing.
- Losing a target or becoming stuck aborts remaining queued steps so the next
  observation can reconsider the plan.
- AI results are probabilistic. Complex moves can require retries, clearer
  timing, or future telemetry and planning improvements.

## Troubleshooting

### `OPENAI_API_KEY is not set`

Set it in the PowerShell window used to launch the bridge:

```powershell
$env:OPENAI_API_KEY = "your-key-here"
```

### `openai` is missing, or installation fails while building `jiter`

Use standard 64-bit Windows Python rather than MSYS2 Python:

```powershell
py -3 -m venv .venv-win
./.venv-win/Scripts/python.exe -m pip install --upgrade pip
./.venv-win/Scripts/python.exe -m pip install -r ./ai_bridge/requirements.txt
```

### A command says sent, but nothing happens

- Start the game before the bridge so runtime discovery finds the live build.
- Check the `Game runtime:` line printed by `bridge.py`.
- Use `/status` to confirm telemetry is available.
- If detection chose the wrong build, set `SM64_AI_RUNTIME_DIR` to the folder
  containing the executable and `.sm64-ai-state.json`.

### Mario goes in the wrong direction

Manual steps are camera-relative when planned, then remain world-locked.
Instructions naming a recognised nearby object should use live targeting. If an
object is missing from `nearby_objects`, it is outside range, excluded by the
20-object limit, in another room, inactive, or not yet recognised.

### Mario repeatedly runs into geometry

Targeting is direct steering, not navigation-mesh pathfinding. It should report
`stuck` after about one second without progress. Try an intermediate target, a
jump-assisted approach, or reposition Mario before retrying.

### Mouse look moves unexpectedly

Mouse look is continuous while the game window is active, spectator camera is
enabled, no native UI is active, and the command box is closed. Press `F1` to
return temporarily to the vanilla camera.
