"""Progress for the launcher (launcher/): with fci-recomp.py --events, the steps, their progress, warnings
and errors are also printed as single lines of JSON after "@fci ". Without it nothing is printed here.

  {"event": "plan", "steps": [{"key": "disc", "text": "...", "weight": 10}, ...]}   the steps to come
  {"event": "step", "key": "disc", "text": "..."}                                    one starts
  {"event": "progress", "key": "build", "percent": 40, "part": "game", "text": "..."}
  {"event": "warning" | "error", "text": "..."}
  {"event": "license", "url": "..."}    Microsoft's terms must be accepted first (exit code 3)
  {"event": "done", "minutes": 12}
"""

import json
import threading

enabled = False
PROTOCOL = 1
_lock = threading.Lock()
_shown = {}


def emit(event, **fields):
    if enabled:
        with _lock:
            print("@fci " + json.dumps({"event": event, **fields}, ensure_ascii=False), flush=True)


def progress(key, done, total, text=None, part=None):
    """a progress event when the whole percent changed (per key and part)"""
    if not enabled or not total:
        return
    percent = min(100, done * 100 // total)
    if _shown.get((key, part)) != percent:
        _shown[(key, part)] = percent
        extra = {k: v for k, v in (("part", part), ("text", text)) if v}
        emit("progress", key=key, percent=percent, **extra)
