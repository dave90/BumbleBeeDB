"""Static regressions for the non-negotiable native/Python GIL boundary."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_core_engine_contains_no_python_or_gil_api() -> None:
    forbidden = ("Python.h", "pybind11", "PyObject", "PyGILState_", "gil_scoped_")
    sources = list((REPO_ROOT / "src").rglob("*.cpp")) + list((REPO_ROOT / "src" / "include").rglob("*.h"))
    for source in sources:
        text = source.read_text(encoding="utf-8")
        for token in forbidden:
            assert token not in text, f"{token} must remain outside the core engine: {source}"


def test_binding_never_explicitly_acquires_the_gil() -> None:
    binding_sources = list((REPO_ROOT / "python" / "src").rglob("*.cpp"))
    binding_sources += list((REPO_ROOT / "python" / "src" / "include").rglob("*.h"))
    combined = "\n".join(source.read_text(encoding="utf-8") for source in binding_sources)
    assert "gil_scoped_acquire" not in combined
    assert "PyGILState_Ensure" not in combined
    assert "py::gil_scoped_release" in combined


def test_native_and_binding_ownership_is_explicit() -> None:
    sources = list((REPO_ROOT / "src").rglob("*.cpp")) + list((REPO_ROOT / "src").rglob("*.h"))
    sources += list((REPO_ROOT / "python" / "src").rglob("*.cpp"))
    sources += list((REPO_ROOT / "python" / "src").rglob("*.h"))
    for source in sources:
        text = source.read_text(encoding="utf-8")
        assert "enable_shared_from_this" not in text, f"ownership must be passed explicitly: {source}"
        assert "shared_from_this(" not in text, f"ownership must be passed explicitly: {source}"


def test_binding_uses_focused_translation_units() -> None:
    source_dir = REPO_ROOT / "python" / "src"
    expected_sources = {
        "bindings.cpp",
        "dataframe_conversion.cpp",
        "py_connection.cpp",
        "py_database.cpp",
        "py_result.cpp",
        "python_conversion.cpp",
        "python_exception.cpp",
        "python_utils.cpp",
    }
    expected_headers = {
        "dataframe_conversion.h",
        "py_connection.h",
        "py_database.h",
        "py_result.h",
        "python_conversion.h",
        "python_exception.h",
        "python_utils.h",
    }
    assert expected_sources <= {path.name for path in source_dir.glob("*.cpp")}
    assert expected_headers <= {path.name for path in (source_dir / "include").glob("*.h")}

    module_entrypoint = (source_dir / "bindings.cpp").read_text(encoding="utf-8")
    assert len(module_entrypoint.splitlines()) < 250
    assert "PYBIND11_MODULE" in module_entrypoint
    for implementation_marker in (
        "PyResult::PyResult",
        "PyConnection::~PyConnection",
        "PyDatabase::~PyDatabase",
        "auto ConvertDataFrame(",
    ):
        assert implementation_marker not in module_entrypoint
