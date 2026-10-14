from spack_repo.builtin.build_systems.python import PythonPackage

from spack.package import *


class PyPympistandard(PythonPackage):
    """Python API to the MPI Standard, including the machine-readable
    MPI API definitions (apis.json) published by the MPI Forum."""

    homepage = "https://github.com/mpi-forum/pympistandard"
    pypi = "pympistandard/pympistandard-0.2.0.tar.gz"

    license("MIT")

    version("0.2.0", sha256="cc7d5aa339289ff8e981dce1af69dbdadde2efb070eab66900d130ddca20fad7")

    depends_on("python@3.8:", type=("build", "run"))
    depends_on("py-pdm-backend", type="build")

    import_modules = ["pympistandard"]

    @property
    def apis_json(self):
        """Path to the bundled MPI API definitions."""
        purelib = self.spec["python"].package.purelib
        return join_path(self.prefix, purelib, "pympistandard", "data", "apis.json")
