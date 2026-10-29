# SM64 AI NPCs — first playable prototype

Branch: `feature/ai-npcs`, based on vanilla `master`. You control Mario with the
normal game controls and camera. A separate Python companion gives Toads and
pink Bob-omb Buddies their own OpenAI conversations and supported actions.

## Build and launch (Windows / existing MSYS2 setup)

Use the same extracted assets, legally supplied base ROM, and build dependencies
as your working PC port. This branch does not include game assets or your key.
Do not copy AI-control source files into this branch.

In your existing checkout:

```bash
git fetch origin
git switch --track origin/feature/ai-npcs
make -j4
```

If you've already checked out the branch, use `git switch feature/ai-npcs` and
`git pull --ff-only`. If switching from another mod leaves stale build output,
run `make clean` followed by your usual build command. Keep any build flags you
normally use (e.g. `VERSION=us`).

Launch the built PC game normally. In a second terminal, at the repository root,
install the companion dependencies with the Python that already runs your
AI-control bridge:

```powershell
python -m pip install -r npc_bridge/requirements.txt
$env:OPENAI_API_KEY = "your-key"
python npc_bridge/bridge.py
```

Or from MSYS2 / Bash:

```bash
python -m pip install -r npc_bridge/requirements.txt
export OPENAI_API_KEY="your-key"
python npc_bridge/bridge.py
```

Keep the companion running. It automatically finds the newest NPC request in
the repository root or `build/*/` after your first message. If you launch the
game elsewhere, pass its actual **working directory**, for example:

```powershell
python npc_bridge/bridge.py --runtime-dir "C:\path\to\sm64-multiverse\build\us_pc"
```

`SM64_NPC_RUNTIME_DIR` is another option: set it to an existing directory before
launching both the game and companion. Both processes must use the same folder.
Run only one NPC companion for a game instance. No AI-control bridge is needed.

The model defaults to `gpt-5.6-luna`, matching the existing AI-control bridge.
Use `--model YOUR_MODEL` or `OPENAI_MODEL` if your account uses a different
model supporting Responses structured output. Each submitted message makes an
API request using your own account; ordinary gameplay makes no requests.

## Playing

- Speak to Toad or a pink Bob-omb Buddy normally. Finish their original dialogue
  and any cannon/star sequence; a reply box then opens automatically.
- **T** opens a chat with the closest supported NPC within 650 game units.
  This also lets you talk to someone who is following or chasing you.
- Type your message, up to 120 characters; **Enter** sends it.
- **Escape** closes the conversation. If a response is still loading, it cancels
  the turn. If an action is queued, closing the box lets the NPC perform it.
- Ordinary conversation can continue in the same box. When an action is queued,
  close the box, let them act, then press T to speak again.
- Mario's movement/buttons are suppressed while typing, including controller
  input. The world continues running; this prototype is not a pause menu.
- The keyboard editor uses the same US scancode approach as the AI-control mod.
  Letters, digits, Shift, punctuation and Backspace work; paste, Unicode, cursor
  editing and voice input are not implemented.

Try the first Bob-omb Buddy in Bob-omb Battlefield:

- "What's your job here?"
- "Come with me, I could use some backup."
- "Can you spare two coins?"
- "You're a rubbish bomb. I bet you couldn't even catch me."
- "Okay, sorry! Let's be friends again."

Toad is a worried but helpful castle attendant. Pink Bob-omb Buddies are proud,
playful cannon enthusiasts who may take offence. Neither is required to obey.
Their original US dialogue, course, area, act, star count and current AI movement
mode provide context for their replies.

## Actions in this version

| Action | Toad | Bob-omb Buddy | Effect |
| --- | --- | --- | --- |
| Speak | Yes | Yes | An in-character response, at most 200 characters |
| Follow / flee | Yes | Yes | Local steering for up to 30 seconds |
| Approach | Yes | Yes | Walk toward Mario and stop nearby |
| Stop / friendly | Yes | Yes | End AI movement or hostility |
| Give coins | Yes | Yes | Spawn 1–3 collectible yellow coins |
| Hostile | No | Yes | Chase and shove Mario for one wedge on contact, with invincibility cooldown |
| Open cannon | No | Cannon operators only | Unlock the current course's cannon |

AI movement uses floor/wall collision checks and refuses obvious ledges, lava
and death planes. It is not navigation/pathfinding: NPCs cannot route through
complex terrain, doors or arbitrary named destinations. Animation polish and
more elaborate combat are future work. Following does not carry across areas.
The normal game's star rewards, cannon cutscenes and progression remain intact.

## Memory, rewards and failures

Each NPC is identified by save slot, level, area, character kind, original spawn
coordinates and behaviour parameters. Conversation histories are separate, with
the latest 16 acknowledged turns retained per NPC. The companion persists those
histories and coin totals in `.sm64-npc-memory.json` at the repository root.
The cap is **10 gifted coins per NPC per save slot**, across area visits and
companion restarts. Native coins and native star rewards are unaffected.
Deleting the companion memory deliberately resets its histories and budgets;
starting a new game in the same save slot does not automatically erase them.

Actions use a fixed validated vocabulary, never executable model code. Both the
companion and game validate capabilities and coin amounts. Every request has a
unique ID. The game ignores late replies after cancellation or an area change,
and executes each accepted action once. Its receipt records what actually
happened; the companion only then adds the turn and actual coin amount to memory.
Movement receipts mean "started", not "reached destination". No autonomous
follow-up model calls happen while an NPC moves.

The model receives the message you submit, that NPC's recent conversation, and
the supplied game context. API keys stay in the companion's environment.
Runtime files, keys in `.env`, and conversation memory are ignored by git.

If the companion is stopped, Escape always exits the box. Requests time out
after about 60 seconds of active gameplay. API/schema errors print details in
the companion terminal and produce a short retry response without an action.

## Validation

```bash
python -m pip install -r npc_bridge/requirements.txt
bash tests/check_ai_npc.sh
```

The checks syntax-compile all touched C translation units, verify the new N64
no-op implementation, run the actual C conversation/action state machine with
stub engine functions, and run Python protocol/memory/model-boundary tests with
a mocked SDK client. No API calls or ROM are required for those checks.

A full linked build and visual/play test require your local extracted game
assets. In particular, verify dialogue timing, readability, Toad movement,
Buddy collision/knockback and controller input on the Windows build.
The N64 target retains vanilla behaviour; the companion feature targets the
native PC port. Browser/console text entry is not supported.
