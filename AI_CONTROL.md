# AI control foundation

The AI controller is a small, game-thread-owned queue that feeds normal N64
controller states into player one's existing input path. It deliberately does
not alter Mario's movement, actions, physics, camera, levels, or save data.

Each queued step contains:

- signed X and Y stick positions in the native N64 range;
- a standard SM64 button bitmask;
- the number of 30 FPS game frames for which those inputs remain held.

`src/game/ai_controller.h` exposes the queue API. A future local OpenAI bridge
can translate structured model output into calls to `ai_controller_enqueue`
without coupling network or JSON code to Mario's movement implementation.

## Development test

While controlling Mario, hold **L + R** and press **D-pad Down**. Physical input
will be replaced temporarily with a canned run-and-jump sequence. Press
**Start** during playback to cancel immediately and return control to the real
controller.

The debug combination and canned sequence are temporary scaffolding. They will
be replaced by the command-entry interface once the control path is proven on a
real build.
