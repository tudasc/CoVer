FROM ubuntu:latest

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get -y -qq --no-install-recommends install \
        cmake \
        make \
        zstd \
        git \
        python3 \
        autoconf \
        automake \
        autotools-dev \
        libtool \
        flex \
        python3-venv \
        unzip \
        nano \
        libzstd-dev \
        wget \
        lsb-release \
        pkg-config \
        libffi-dev \
        software-properties-common \
        libdw-dev \
        openjdk-25-jre openjdk-25-jdk

# Add LLVM Repo
RUN wget https://apt.llvm.org/llvm.sh
RUN chmod +x llvm.sh
RUN ./llvm.sh 22 all
RUN apt install -y -qq flang-22
RUN ln -s /usr/lib/llvm-22/build/utils/lit/lit.py /usr/bin/lit
ENV PATH="/usr/lib/llvm-22/bin:$PATH"

ENV CC=clang
ENV CXX=clang++
ENV OMPI_CC=$CC
ENV OMPI_CXX=$CXX

# Compile OpenMPI (Cant use prebuilt as its module files are gfortran specific)
RUN git clone --recursive https://github.com/open-mpi/ompi/
RUN cd ompi && ./autogen.pl && ./configure CC=clang CXX=clang++ FC=flang --prefix=/usr --with-pmix=internal
RUN cd ompi && make -j8 install

ENV OMPI_ALLOW_RUN_AS_ROOT=1
ENV OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
ENV PMIX_MCA_gds="hash"
ENV OMPI_MCA_memory="^patcher"

# Install Python dependencies and ensure to activate virtualenv (by setting PATH variable)
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv $VIRTUAL_ENV
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# Compile CoVer
COPY ./ /tmp/cover_src
WORKDIR /tmp/cover_src
RUN rm -rf build/ install/
RUN mkdir build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/opt/cover -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON && cmake --build . --target install -j

ENV PATH="/opt/cover/bin:$PATH"

WORKDIR /root
