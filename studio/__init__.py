"""Hermes Book-to-Voice Studio — the offline Book → Data half of the pipeline.

Produces the content bundle (docs/end_to_end_pipeline.md §3) that the Hermes device ingests.
Rules tiers run with zero dependencies; spaCy / LLM tiers are opt-in. This package is Python-only
and is NOT part of the aarch64 device build (CMake never sees it).
"""
__version__ = "0.1.0"
