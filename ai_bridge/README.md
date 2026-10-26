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

Mario should perform the same deterministic run-and-jump sequence, while the
HUD changes from `AI EXECUTING` to `AI COMPLETE` and then `AI READY`.

To enable natural-language commands, set up the companion from the repository
root in MSYS2:

```bash
python -m venv .venv
source .venv/Scripts/activate
python -m pip install -r ai_bridge/requirements.txt
export OPENAI_API_KEY="your-key-here"
```

Optionally select another compatible model:

```bash
export OPENAI_MODEL="gpt-6-astra"
```

With the built game still running from the repository root, run:

```bash
source .venv/Scripts/activate
python ai_bridge/bridge.py
```

Try instructions such as:

- `Run forward for two seconds.`
- `Run forward, jump, and keep moving for another second.`
- `Turn left, pause, then jump.`

The files `.sm64-ai-command`, `.sm64-ai-state.json`, and `.sm64-ai-status` are
runtime-only local communication files and must not be committed.

Press **Start** in the game to cancel an executing sequence and immediately
return to physical control.
