"""Build Cython/C++; nunca importa o driver nem acessa instrumentos."""
import os
from pathlib import Path
from setuptools import Extension, setup
from Cython.Build import cythonize

build_root = Path(os.environ.get("DHO804_BUILD_ROOT", "build"))
sanitizer = os.environ.get("DHO804_SANITIZER", "")
if sanitizer not in {"", "address,undefined", "thread"}:
    raise ValueError("DHO804_SANITIZER invalido")
compile_flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-fno-strict-aliasing"]
link_flags = []
if sanitizer:
    extra = [f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer", "-O1", "-g"]
    compile_flags.extend(extra)
    link_flags.append(f"-fsanitize={sanitizer}")

setup(ext_modules=cythonize(
    [Extension("dho804_usb.driver", ["src/dho804_usb/driver.pyx"],
               language="c++", include_dirs=["native"], depends=["native/transport.hpp"], extra_compile_args=compile_flags,
               extra_link_args=link_flags)],
    build_dir=str(build_root / "cython"),
    annotate=True,
    compiler_directives={
        "language_level": 3,
        "annotation_typing": False,
        "binding": True,
        "embedsignature": True,
        "warn.unreachable": True,
        "warn.maybe_uninitialized": True,
        "warn.unused": True,
    },
))
