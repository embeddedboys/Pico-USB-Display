"""Shared test infrastructure: CLI, exit codes, oracle declaration.

See `harness.py`.  Tests import it as::

    import os, sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "common"))
    from harness import Oracle, Report, Snapshot, run_test
"""
