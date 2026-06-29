# Memory sidecar (mem0)

A small local HTTP service that fronts [mem0](https://github.com/mem0ai/mem0) so the C++
`story_agent` can use long-term memory without embedding Python. `story_agent`'s `Memory` facade
calls it on `127.0.0.1:7070`.

```
story_agent (C++) ──HTTP──► services/memory/server.py ──► mem0 (Chroma + embeddings + LLM)
   recall()  → POST /search   (vector search — cheap, runs on the turn)
   remember()→ POST /add      (LLM extraction — heavy; fire-and-forget / batch at idle)
```

## Run (fully local, no cloud)
```bash
pip install -r requirements.txt
pip install chromadb sentence-transformers ollama
ollama serve & ollama pull qwen2.5:3b            # the extraction LLM (reuse your main one)

MEM0_LLM=ollama MEM0_DB=./mem0_db python3 server.py
# → http://127.0.0.1:7070
```

## Or with OpenAI (dev)
```bash
MEM0_LLM=openai OPENAI_API_KEY=sk-... python3 server.py
```

## Config (env)
| Var | Default | Meaning |
|-----|---------|---------|
| `HERMES_MEM_HOST/PORT` | `127.0.0.1` / `7070` | bind address (must match `story_agent`) |
| `MEM0_DB` | `/var/lib/hermes/mem0` | Chroma store path (on-device, private) |
| `MEM0_EMBEDDER` / `MEM0_EMBED_MODEL` | `huggingface` / MiniLM | local embeddings |
| `MEM0_LLM` / `MEM0_LLM_MODEL` | *(none → OpenAI)* | `ollama` or `openai` for extraction |
| `MEM0_EAGER` | unset | init mem0 at boot (fail fast) instead of lazily |

## API
- `GET /health` → `{"ok":true,"mem0":<initialised?>,"error":...}`
- `POST /search` `{"user_id","query","top_k"?}` → `{"facts":["…","…"]}`
- `POST /add` `{"user_id","text","agent_id"?}` → `{"ok":true}`

## On-device (RK3588) notes
- **Recall** is cheap (embeddings + vector search) — fine on the turn.
- **`/add`** runs an LLM extraction (~7K tokens; seconds on-device). Call it **fire-and-forget**
  or batch it during the idle "sleep" consolidation — never on the response critical path.
- Reuse your **one** local LLM (point `MEM0_LLM=ollama` at the same model the connector uses) so
  you don't load a second model. Privacy: keep `MEM0_DB` on-device, parent-erasable.
