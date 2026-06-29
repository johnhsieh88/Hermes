#!/usr/bin/env python3
"""Hermes memory sidecar — a thin local HTTP wrapper around mem0 (https://github.com/mem0ai/mem0).

story_agent's C++ Memory facade calls this over localhost:
    POST /add    {"user_id","text","agent_id"?}     -> mem0.add   (LLM extraction; heavy)
    POST /search {"user_id","query","top_k"?}        -> mem0.search (vector recall; cheap)
    GET  /health                                     -> {"ok":true,"mem0":bool}

Runs fully local when configured for it (Chroma vector store + HF embeddings + Ollama LLM) — see
README.md. mem0 is imported lazily so the server boots even before backends are ready; until then
/search returns no facts and /add reports the reason. Stdlib only (plus mem0ai).
"""
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_mem = None        # lazily-initialised mem0 Memory
_err = None        # last init error (surfaced via /health, /add)


def _build_config():
    """All-local-friendly config from env. Returns a dict or None (mem0 defaults → OpenAI)."""
    cfg = {
        "vector_store": {
            "provider": "chroma",
            "config": {"path": os.getenv("MEM0_DB", "/var/lib/hermes/mem0"),
                       "collection_name": "hermes"},
        },
        "embedder": {
            "provider": os.getenv("MEM0_EMBEDDER", "huggingface"),
            "config": {"model": os.getenv("MEM0_EMBED_MODEL",
                                          "sentence-transformers/all-MiniLM-L6-v2")},
        },
    }
    llm = os.getenv("MEM0_LLM", "")          # "ollama" | "openai" | "" (= mem0 default OpenAI)
    if llm == "ollama":
        cfg["llm"] = {"provider": "ollama",
                      "config": {"model": os.getenv("MEM0_LLM_MODEL", "qwen2.5:3b"),
                                 "ollama_base_url": os.getenv("OLLAMA_BASE_URL",
                                                              "http://127.0.0.1:11434")}}
    elif llm == "openai":
        cfg["llm"] = {"provider": "openai",
                      "config": {"model": os.getenv("MEM0_LLM_MODEL", "gpt-4o-mini")}}
    return cfg


def mem():
    global _mem, _err
    if _mem is None:
        try:
            from mem0 import Memory
            _mem = Memory.from_config(_build_config())
            _err = None
        except Exception as e:                # not installed / backend not up yet
            _err = f"{type(e).__name__}: {e}"
            raise
    return _mem


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def log_message(self, *a):                # quiet
        pass

    def do_GET(self):
        if self.path == "/health":
            ok = _mem is not None
            self._send(200, {"ok": True, "mem0": ok, "error": _err})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        try:
            req = self._body()
            if self.path == "/search":
                res = mem().search(req["query"], user_id=req.get("user_id", "default"),
                                   limit=int(req.get("top_k", 3)))
                items = res.get("results", res) if isinstance(res, dict) else res
                facts = [r.get("memory", "") for r in (items or [])]
                self._send(200, {"facts": facts})
            elif self.path == "/add":
                msg = req.get("text", "")
                mem().add(msg, user_id=req.get("user_id", "default"),
                          agent_id=req.get("agent_id"))
                self._send(200, {"ok": True})
            else:
                self._send(404, {"error": "not found"})
        except KeyError as e:
            self._send(400, {"error": f"missing field {e}"})
        except Exception as e:                # backend not ready → degrade, don't crash
            self._send(503, {"error": str(e), "facts": []})


def main():
    host = os.getenv("HERMES_MEM_HOST", "127.0.0.1")
    port = int(os.getenv("HERMES_MEM_PORT", "7070"))
    if os.getenv("MEM0_EAGER"):               # optional: fail fast if backends misconfigured
        mem()
    print(f"hermes memory sidecar on http://{host}:{port}  (mem0 lazy-init)")
    ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == "__main__":
    main()
