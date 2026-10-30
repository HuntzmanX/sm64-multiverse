# SM64 AI NPCs — conversations, voices and side panel

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

## Voices and conversation timing

The companion now reads **your typed messages as Mario**, then lets the NPC
answer. The defaults are:

| Character | Fish reference ID |
| --- | --- |
| Mario | `a564085babeb4d14802ea31b7b6c2869` |
| Toad (updated manic voice) | `e273b9cb9eaf4c5caefb83f6fefa7ac3` |
| Bob-omb Buddy (Gilbert Gottfried) | `c4adfd6bd1fe4e59ae18ce19dbce2c14` |

The default Fish model is now **`drama-3`**, using your Fish API credit. Set a
Fish key in the companion's terminal alongside your existing OpenAI key:

```powershell
$env:FISH_AUDIO_API_KEY = "your-fish-key"
$env:FISH_AUDIO_MODEL = "drama-3"
python npc_bridge/bridge.py
```

Bash equivalent: `export FISH_AUDIO_API_KEY="your-fish-key"` and
`export FISH_AUDIO_MODEL="drama-3"`. No additional Python
packages are needed. Native Windows Python uses Windows' built-in MP3 playback
through WinMM/MCI. Linux/macOS or non-native MSYS Python need `ffplay` (FFmpeg),
or you can run the companion with native Windows Python.

Each submitted turn works as follows:

1. A Mario `...` bubble appears while his voice is prepared. The NPC's OpenAI
   reply is prepared concurrently.
2. Mario's message is revealed when his audio starts. The latest NPC bubble
   shows animated dots while its reply/voice is being prepared. NPC audio is
   prefetched during Mario's playback; the two TTS downloads do not overlap.
3. Once Mario finishes and the NPC audio is ready, the NPC's message appears at
   playback start. The two voices never talk over each other.
4. After the NPC finishes, you can send the next message or close the panel to
   let an accepted action run.

The Windows audio-start callback publishes each reveal event; the game shows it
on its next frame and acknowledges it. This is frame-level synchronization,
not word highlighting or lip sync. On the ffplay fallback, the reveal occurs at
player launch, so decoder/device buffering can introduce a small difference.
If a voice is disabled or fails, its text appears without audio and the turn
continues. Stage tags stay out of the displayed dialogue.

Drama 3 supports plain-language actor directions in square brackets. The OpenAI
reply now includes a separate `spoken_reply` script, with up to four concise
cues placed before the words they affect, for example:

```text
[excited] Ready for blastoff! [whispering conspiratorially] Just don't tell the King.
```

The displayed reply contains the same words without cues. Validation requires
the spoken words and punctuation to match the displayed reply, preventing the
model from inventing a different audio response. Natural directions such as
`[nervous, trying to sound brave]`, `[angry, raising your voice]`, `[small laugh]`
and `[short pause]` are allowed. Mario still reads your typed words as written.
The older broad emotion field remains a fallback for simple delivery.

Escape, an area change, or a request timeout invalidates old audio and replies.
Escaping during a turn cancels its unfinished action. NPCs can set
`end_conversation` to say a final line and close the panel after speech finishes,
including returning Mario's goodbye. Ordinary answers keep the conversation open.
Any requested action automatically closes the conversation after speech, then
starts in the NPC's next behavior update; no Escape or confirmation is needed.
With voice disabled or playback unavailable, the last line stays readable for
1.5–8 seconds before an automatic close. Escape cancels during that interval too.
AI-reply and player-message MP3s are
temporary and removed after playback. Game audio and voices mix through the computer; positional speech
and music ducking are not implemented.

The model still makes one OpenAI request per message. Fish receives each voiced
message's text, its reference ID and any performance directions. There are
no requests while idle. Keys remain in the environment and are never saved.

| Optional setting | Default / effect |
| --- | --- |
| `FISH_AUDIO_MARIO_VOICE` | Mario ID above; set empty to disable reading player messages |
| `FISH_AUDIO_TOAD_VOICE` | New Toad ID above; empty disables Toad speech |
| `FISH_AUDIO_BOBOMB_VOICE` | Gilbert ID above; empty disables Buddy speech |
| `FISH_AUDIO_MODEL` | `drama-3`; can explicitly switch back to `s2.1-pro-free` |
| `SM64_NPC_VOICE_VOLUME` | `0.8`; range `0`–`1` |

`--no-voice` or an unset `FISH_AUDIO_API_KEY` gives the same panel and history
with text-only replies. Restart the companion after changing environment
settings. Explicit voice overrides take priority over these new defaults.

**Rebuild the game and restart the companion together after this update.** The
native-page hook and updated performance scripts require the matching game and companion.

API reference: https://docs.fish.audio/api-reference/endpoint/openapi-v1/text-to-speech

## Cached original NPC dialogue

Original US-English **Toad and pink Bob-omb Buddy dialogue pages** are now voiced
in their assigned voices. The game exports the actual page being displayed,
including expanded button names and variable numbers; the companion does not
ask OpenAI to rewrite these lines.

- **First encounter:** generate any missing page recording and save it under
  `npc_audio_cache/` in the repository root. There may be a short generation wait.
- **Repeat encounter:** load the existing MP3 without another TTS request.
- Advancing a page or leaving the dialogue stops the old page's playback.
  Cannon preparation and "ready for blastoff" are separate pages/recordings.
- If a page's first download has already started when you skip it, the complete
  result can still be cached for next time. Queued pages that have not started
  are cancelled. Stale audio never plays over a different page or an AI reply.
- A cache key includes exact spoken text, model, reference voice, directions,
  language and format. Changing those creates a fresh recording. Old files are
  retained, so changing back can reuse them.
- Failed, incomplete or non-MP3 downloads are not cached. An invalid existing
  recording is regenerated. Concurrent requests for the same entry share one
  generation.

**Only original dialogue audio is cached.** AI-generated replies and Mario's
messages always use temporary audio. The cache is ignored by git, and the live
native-dialogue state expires if the game stops updating. Other characters and
non-US dialogue decoding are not enabled by this version.

Set `SM64_NPC_AUDIO_CACHE_DIR` to use another cache folder. Removing a cached
recording causes it to be generated again next time. `--no-voice`, an unset Fish
key, or an empty reference ID disables that voice without blocking the game.

References: [Drama 3 performance guide](https://docs.fish.audio/features/text-to-speech/drama-3)
and [Text-to-Speech API](https://docs.fish.audio/api-reference/endpoint/openapi-v1/text-to-speech).

## Panel, controls and camera

- Finish an NPC's normal dialogue and any cannon/star sequence to open the
  conversation panel, or press **T** near a supported NPC.
- The panel occupies the **right third** of the widescreen view, with separate
  Mario/NPC bubbles and half-size text. UI scaling is independent of game
  resolution; no doubling of render resolution or changes to the normal HUD
  are needed.
- The camera gently changes its aim so the two characters' midpoint sits near
  the **left third** of the screen. Its position stays under the normal camera's
  control, preserving collision placement. Framing eases back after closing.
- **Up/Down**, **Page Up/Page Down**, **Home/End**, the **mouse wheel**, or clicking
  the **on-screen arrows** scroll the transcript. A new message follows the
  newest entry automatically.
- Type up to 120 characters. **Enter** sends when the current turn is finished.
  You can draft your next message during
  playback. **Escape** closes/cancels as described above.
- History remains visible when reopening the same NPC's chat in an area.
  The latest 16 acknowledged turns are restored from persistent memory on the
  first submitted message after entering a new area or restarting the game.
- Mario's gameplay buttons are suppressed while chatting, including controller
  input. The world continues running; the panel is not a pause menu.
- The US scancode editor supports letters, digits, Shift, punctuation and
  Backspace. Paste, Unicode, cursor editing and voice input are not implemented.

Try asking Toad to whisper a secret, then typing an indignant reply as Mario.
Neither NPC is required to obey you. Their original US dialogue, course, area,
act, star count, conversation history and current AI movement mode provide context.

## Actions in this version

| Action | Toad | Bob-omb Buddy | Effect |
| --- | --- | --- | --- |
| Speak | Yes | Yes | An in-character response, at most 200 characters |
| End conversation | Yes | Yes | Say a final line, then close the panel after playback |
| Follow / flee | Yes | Yes | Local steering for up to 30 seconds |
| Approach | Yes | Yes | Walk toward Mario and stop nearby |
| Stop / friendly | Yes | Yes | End AI movement or hostility |
| Give coins | Yes | Yes | Spawn 1–3 collectible yellow coins |
| Hostile | Yes | Yes | Chase and shove Mario for one wedge on contact, with invincibility cooldown |
| Open cannon | No | Cannon operators only | Unlock the current course's cannon |

AI movement uses floor/wall collision checks and refuses obvious ledges, lava
and death planes. Its movement helper uses downward signed gravity, restoring
the NPC's original physics settings after each step. It is not navigation/pathfinding:
NPCs cannot route through complex terrain, doors or arbitrary named destinations. Animation polish and
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
after about 120 seconds of active gameplay without a new stage reveal.
API/schema errors print details in the companion terminal and produce a short retry response without an action.

## Validation

```bash
python -m pip install -r npc_bridge/requirements.txt
bash tests/check_ai_npc.sh
```

The checks syntax-compile all touched C translation units, verify the new N64
no-op implementation, run the actual C conversation/action state machine with
stub engine functions at 4:3 and 16:9, and check history wrapping, half scaling,
scroll buttons, reveal acknowledgement, and camera projection/easing. Python
tests cover protocol, memory, model boundaries, cancellation, audio start
callbacks, ordered playback, performance-script validation, native page decoding,
cache misses/hits/invalidation, failed downloads and parallel preparation using mocked services. No API calls or ROM are required for those checks.

A full linked build and visual/play test require your local extracted game
assets. In particular, verify the panel visually, camera composition in different rooms,
real audio timing, Toad movement, Buddy collision/knockback and controller input
on the Windows build.
The N64 target retains vanilla behaviour; the companion feature targets the
native PC port. Browser/console text entry is not supported.
