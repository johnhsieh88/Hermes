# MEMORY (index)

One line per durable (semantic) fact — the cheap recall surface loaded every session. The
connector reads this first, then pulls the full `semantic/<name>.md` only for facts relevant to
the current turn. Consolidation keeps this list short and current.

- [Listener profile](semantic/listener-profile.md) — who's listening: name, age band, reading level, preferences
<!-- - [Book: <title>](semantic/book-<slug>.md) — progress + what they liked -->
<!-- - [Favorite characters](semantic/favorites.md) — characters/voices they ask for -->
