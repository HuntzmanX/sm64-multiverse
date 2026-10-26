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

Try instructions such as:

- `Run forward for two seconds.`
- `Run forward, jump, and keep moving for another second.`
- `Turn left, pause, then jump.`
- `Say "Let's-a go!", then run forward and jump.`

The bridge prints the model's interpretation and each intended step, waits for
the game to finish the sequence, then reports measured movement, height, and
health changes. A `say` step currently appears in the companion console and
becomes a neutral pause in-game. Its structured action is ready to drive an
in-game text box or TTS in a later phase without changing the model contract.

Interactive bridge commands:

- `/status` shows the current bridge status and Mario's latest position.
- `/help` lists companion commands.
- `/quit` exits the companion.

The files `.sm64-ai-command`, `.sm64-ai-state.json`, and `.sm64-ai-status` are
runtime-only local communication files and must not be committed.

Press **Start** in the game to cancel an executing sequence and immediately
return to physical control.
