FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Berlin

RUN apt-get update \
    && apt-get -y -qq --no-install-recommends install \
    cmake \
    curl \
    binutils-dev \
    make \
    automake \
    autotools-dev \
    nano \
    autoconf \
    libtool \
    zlib1g \
    zlib1g-dev \
    zstd \
    libzstd-dev \
    bzip2 \
    libucx-dev \
    libucx0 \
    libatomic1 \
    libfabric-dev \
    libxml2-dev \
    python3 \
    python3-pip \
    python3-venv \
    gfortran \
    gcc \
    g++ \
    git \
    antlr4 \
    graphviz \
    libgtest-dev \
    ninja-build \
    vim \
    openssh-client \
    openssh-server \
    gdb \
    wget \
    googletest \
    clang-19 \
    libomp-19-dev \
    clang-format-19 \
    llvm-19 \
    llvm-19-tools \
    clang-tools-19 \
    llvm-19-dev

# Install OpenMPI
RUN wget https://download.open-mpi.org/release/open-mpi/v5.0/openmpi-5.0.7.tar.bz2
RUN tar xf openmpi-5.0.7.tar.bz2 && \
    cd openmpi-5.0.7 && \
    ./configure && \
    make -j$(nproc) install
RUN ldconfig

# ensure that LLVM 19 toolset is used
RUN ln -s /usr/bin/FileCheck-19 /usr/bin/FileCheck
RUN ln -s /usr/bin/clang-19 /usr/bin/clang
RUN ln -s /usr/bin/clang++-19 /usr/bin/clang++
RUN ln -s /usr/bin/clang-format-19 /usr/bin/clang-format
RUN ln -s $(which llvm-link-19) /usr/bin/llvm-link
RUN ln -s $(which opt-19) /usr/bin/opt
RUN ln -s $(which llc-19) /usr/bin/llc

# Install Python dependencies and ensure to activate virtualenv (by setting PATH variable)
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv $VIRTUAL_ENV
ENV PATH="$VIRTUAL_ENV/bin:$PATH"
RUN pip3 install http://apps.fz-juelich.de/jsc/jube/download.php?version=2.7.1
RUN pip3 install pandas

WORKDIR /externals

# Get newer ANTLR4 + Runtime
RUN wget https://github.com/antlr/website-antlr4/raw/refs/heads/gh-pages/download/antlr-4.10.1-complete.jar
COPY docker_files/libantlr4-runtime-dev_4.10+dfsg-1_amd64.deb antlr4-runtime.deb
COPY docker_files/libantlr4-runtime4.10_4.10+dfsg-1_amd64.deb antlr4-runtime1.deb
RUN apt-get -y -qq --no-install-recommends install ./antlr4-runtime1.deb ./antlr4-runtime.deb

# Install CoVer
COPY ./ ./contrplugin
# Remove local dev files if they exist
RUN rm -rf ./contrplugin/build ./contrplugin/install
# Compile
RUN cd ./contrplugin && \
    mkdir -p build && \
    cd build && \
    CC=clang CXX=clang++ OMPI_CC=clang OMPI_CXX=clang++ MPICH_CC=clang MPICH_CXX=clang++ cmake -DANTLR4_JAR_LOCATION="../../antlr-4.10.1-complete.jar" -DCMAKE_PREFIX_PATH='/usr/lib/llvm-19/;/usr/lib/;/usr/lib64/' -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/contrplugin .. && \
    make -j$(nproc) install

# Clean up externals
RUN rm -rf /externals

WORKDIR /root/

ENV OMPI_CC=clang
ENV OMPI_CXX=clang++
ENV MPICH_CC=clang
ENV MPICH_CXX=clang++
ENV PATH="/opt/contrplugin/bin:/opt/parcoach/bin:$PATH"
ENV LD_LIBRARY_PATH="/usr/lib64:$LD_LIBRARY_PATH"

RUN echo "root:root" | chpasswd
RUN sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config
RUN mkdir /var/run/sshd
CMD ["/usr/sbin/sshd", "-D"]
EXPOSE 22

# Always use bash (for time builtin)
RUN ln -sf /bin/bash /bin/sh

# Make sure things like venv are working in SSH
RUN echo "export PATH=${PATH}:\$PATH" >> /root/.bashrc
