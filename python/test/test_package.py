"""Installed distribution metadata and typing artifact checks."""

from __future__ import annotations

from importlib import metadata, resources

import bumblebeedb as bb


def test_installed_metadata_matches_imported_version() -> None:
    distribution = metadata.metadata("bumblebeedb")
    assert distribution["Name"] == "bumblebeedb"
    assert distribution["Version"] == bb.__version__
    assert distribution["Requires-Python"] == ">=3.10"


def test_typing_marker_and_native_stub_are_installed() -> None:
    package = resources.files("bumblebeedb")
    assert package.joinpath("py.typed").is_file()
    assert package.joinpath("_native.pyi").is_file()
