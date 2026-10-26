# SM64 AI bridge

This companion converts natural-language instructions into validated SM64
controller steps. The OpenAI API key and network requests remain outside the
game process.

## Setup

First rebuild and start the game from the repository root. In a second MSYS2
window, test the local transport without making an API call:

```bash
python ai_bridge/send_demo.py
```

Mario should perform the same deterministic run-and-jump sequence. Bridge
status is published in `.sm64-ai-status`; in-game status text is temporarily
disabled because the original colorful HUD font is unstable in the PC renderer.

The companion automatically looks for the newest live state file in the
repository root or a `build/*` directory. This supports launching the game from
MSYS2 or by double-clicking its executable. The selected directory is printed
when the bridge starts. To override detection, set `SM64_AI_RUNTIME_DIR`.

To enable natural-language commands on Windows, use standard 64-bit Windows
Python from PowerShell. MSYS2's Python may try to compile the `jiter` dependency
from source instead of using its prebuilt Windows wheel:

```powershell
py -3 -m venv .venv-win
.\.venv-win\Scripts\python.exe -m pip install --upgrade pip
.\.venv-win\Scripts\python.exe -m pip install -r .\ai_bridge\requirements.txt
$env:OPENAI_API_KEY = "your-key-here"
```

Optionally select another compatible model:

```powershell
$env:OPENAI_MODEL = "gpt-6-astra"
```

With the built game still running from the repository root, run:

```powershell
.\.venv-win\Scripts\python.exe .\ai_bridge\bridge.py
```

## AI-control keyboard and camera

The player is now Mario's camera operator while queued AI inputs control Mario:

- **W/S** flies in the direction the camera is looking, including up or down;
  **A/D** strafes horizontally.
- **Q** descends and **E** ascends.
- Hold **left click** and move the **mouse** to rotate and tilt the camera; the
  **arrow keys** provide the same control without a mouse.
- **F1** toggles the spectator camera so vanilla camera testing is still
  available.
- **F** toggles focus mode while freecam is active. The camera position stays
  fixed, but its view continuously rotates to keep Mario centred. Camera
  movement remains available while manual look controls are temporarily ignored.
- **Tab** toggles the in-game command box, while **T** opens it without acting
  as a close shortcut once you are typing. Type an instruction and press
  **Enter** to send it to the running bridge. **Escape** or **Tab** closes the
  box without sending. The panel and text wrapping adapt to widescreen windows.
- While the command box is open, gameplay shortcuts are suppressed. Outside
  it, **Escape** also acts as Start/pause.

AI stick sequences retain the world-space direction they had when movement
began. Rotating the spectator camera while Mario is running therefore changes
the view without steering him off course. Object-directed plans also carry the
camera yaw from their telemetry snapshot, so moving the camera while the model
is thinking cannot corrupt their direction. Start remains the emergency cancel
for an executing sequence.

Try instructions such as:

- `Run forward for two seconds.`
- `Run forward, jump, and keep moving for another second.`
- `Turn left, pause, then jump.`
- `Say "Let's-a go!", then run forward and jump.`

The bridge prints the model's interpretation and each intended step, waits for
the game to finish the sequence, then reports measured movement, height, and
health changes. Plain stick-movement steps can finish early after Mario remains
stationary for several frames, so chained commands do not wait out a stale
frame budget. Explicit waits and button inputs always retain their requested
duration. A `say` step is displayed in-game with SM64's native dialog
font and a translucent text panel. Speech must be the first step of a plan and
temporarily gives Mario neutral input while it is displayed.

A compact HUD-font label reports `AI READY`, `AI THINKING`, `AI ACTING`,
`AI DONE`, `AI ERROR`, or `AI CANCELLED`. It uses fixed uppercase strings to
keep the PC texture path as simple as the original working ready indicator.
Mario's normal standing idle animations remain enabled, but the final transition
into sleep is suppressed so a delayed command is not spent waking him and the
camera FOV does not zoom while the model is thinking.

Telemetry includes up to 20 nearby recognised objects within 8000 world units,
sorted nearest first. Coins, stars, doors, Bob-omb Buddies, common enemies,
breakable and cap boxes, trees, cannons, signs, poles, switches, caps, shells,
1-Ups, recovery hearts, and warp pipes are identified. Each object includes a
stable ID, 3D and horizontal distance, height difference, threat flag, world
position, and camera-relative `stick_toward` hint. Mario's floor height, height
above ground, floor type/slope, wall contact, and water level are also reported.

For goals that depend on changing object positions, the companion can run up to
six short observe-act cycles. It refreshes telemetry between cycles and lets the
model decide whether the instruction is complete or needs another movement
burst. The fixed cycle cap prevents an unclear command from running forever.

Object-directed movement uses native live tracking rather than fixed stick
bursts. Each recognised object receives a stable ID for its lifetime. A target
step recalculates steering every frame, follows moving targets, ends at a chosen
distance, and reports `arrived`, `clear`, `target_lost`, `stuck`, or `timeout`.
The selected target emits periodic sparkles in-game. If Mario barely moves for
one second, the step stops instead of spending all six cycles running into the
same wall.

Examples after entering a course with recognised objects nearby:

- `Run toward the nearest red coin.`
- `Approach the red coin, but if a Goomba gets within 700 units, run away from it.`
- `Say "That Goomba looks suspicious!", then back away from the nearest Goomba.`

Interactive bridge commands:

- `/status` shows the current bridge status and Mario's latest position.
- `/help` lists companion commands.
- `/quit` exits the companion.

The files `.sm64-ai-command`, `.sm64-ai-state.json`, `.sm64-ai-status`, and
`.sm64-ai-request` are runtime-only local communication files and must not be
committed.

Press **Start** in the game to cancel an executing sequence and immediately
return to physical control.
