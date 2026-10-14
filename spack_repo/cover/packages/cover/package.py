import os

from spack_repo.builtin.build_systems.cmake import CMakePackage, generator

from spack.package import *


class Cover(CMakePackage):
    """CoVer: contract-based verification of MPI/OpenSHMEM programs via an LLVM plugin."""

    homepage = "https://github.com/tudasc/CoVer"
    git = "https://github.com/tudasc/CoVer.git"

    license("GPL-3.0-only")

    version("master", branch="master", submodules=True)

    variant("bundled_jsoncpp", default=True, description="Use the vendored jsoncpp submodule")

    # C++20 modules require CMake >= 3.28 + Ninja
    generator("ninja")

    depends_on("c", type="build")
    depends_on("cxx", type="build")

    depends_on("cmake@3.28:", type="build")
    depends_on("ninja", type="build")
    depends_on("pkgconfig", type="build")
    depends_on("java", type="build")  # ANTLR4 grammar generation
    depends_on("python", type="build")  # premade MPI/SHMEM contract headers
    depends_on("py-pympistandard", type="build")  # apis.json for the MPI contract headers

    depends_on("llvm@22:")
    depends_on("libffi")
    depends_on("mpi")
    depends_on("jsoncpp", when="~bundled_jsoncpp")

    # GCC trips over TU-local entities in LLVM headers inside C++20 module fragments
    conflicts("%cxx=gcc", msg="CoVer must be built with clang (C++20 modules + LLVM headers)")

    def cmake_args(self):
        args = [
            self.define_from_variant("BUNDLED_JSONCPP", "bundled_jsoncpp"),
            self.define("MPI_APIS_JSON", self.spec["py-pympistandard"].package.apis_json),
        ]
        # clang-scan-deps is invoked with Spack's compiler wrapper as argv[0], so it
        # looks for clang.cfg/clang++.cfg next to the wrapper instead of the real clang
        # and silently falls back to the system GCC headers. Pass the configs explicitly.
        for lang, cfg in (("C", "clang.cfg"), ("CXX", "clang++.cfg")):
            cfg_path = join_path(self.spec["cxx"].prefix.bin, cfg)
            if os.path.isfile(cfg_path):
                args.append(self.define(f"CMAKE_{lang}_FLAGS", f"--config={cfg_path}"))
        return args
