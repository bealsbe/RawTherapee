# Cross-compilation environment for the portable Windows (x86_64) build.
#
# Fedora ships a complete MinGW-w64 GTK stack as mingw64-* packages (gtkmm 3,
# exiv2 0.28, librsvg, fftw, lcms2, libtiff, libjpeg-turbo, libpng, expat,
# zlib, libgomp, Adwaita icons...). The three RawTherapee dependencies Fedora
# does not package for MinGW -- lensfun, libiptcdata and libjxl -- are built
# from source below and installed into the MinGW sysroot, as is the SVG
# gdk-pixbuf loader (Fedora's loaders.cache references it but the mingw
# librsvg2 package does not ship it; GTK needs it for the Adwaita symbolic
# icons).
#
# The vendored LibRaw (external/libraw) is NOT built here: it is part of the
# source tree and is cross-compiled by build-inside.sh at build time so that
# changes to it are picked up.
#
# Wine + Xvfb are installed only to smoke-test the produced executables.
FROM fedora:44

ARG WITH_JXL=1
ARG LENSFUN_VERSION=0.3.4
ARG LIBIPTCDATA_VERSION=1.0.5
ARG LIBIPTCDATA_TAG=release_1_0_5
ARG LIBJXL_VERSION=v0.11.2
ARG LIBRSVG_LOADER_REF=2.56.4
ARG FMT_COMMIT=e424e3f2e607da02742f73db84873b8084fc714c

ENV MINGW_PREFIX=/usr/x86_64-w64-mingw32/sys-root/mingw \
    MINGW_TRIPLET=x86_64-w64-mingw32

# Host tools + MinGW toolchain + MinGW libraries.
RUN dnf -y install --setopt=install_weak_deps=False \
        cmake ninja-build make autoconf automake libtool pkgconf-pkg-config \
        gcc gcc-c++ git rsync which util-linux file zip unzip xz bzip2 tar \
        python3 python3-setuptools glib2 glib2-devel binutils patch \
        mingw64-filesystem mingw64-gcc mingw64-gcc-c++ mingw64-binutils \
        mingw64-headers mingw64-crt mingw64-winpthreads mingw64-libgomp \
        mingw64-gtkmm30 mingw64-gtk3 mingw64-glib2 mingw64-gdk-pixbuf mingw64-cairo \
        mingw64-cairomm mingw64-glibmm24 mingw64-pangomm mingw64-atkmm mingw64-libsigc++20 \
        mingw64-exiv2 mingw64-librsvg2 mingw64-fftw mingw64-lcms2 \
        mingw64-libtiff mingw64-libjpeg-turbo mingw64-libpng mingw64-libwebp \
        mingw64-expat mingw64-zlib mingw64-brotli mingw64-inih \
        mingw64-adwaita-icon-theme mingw64-hicolor-icon-theme mingw64-fontconfig \
        wine-core.x86_64 xorg-x11-server-Xvfb \
    && dnf clean all

# lensfun (lens correction library + database)
RUN curl -fsSL -o /tmp/lensfun.tar.gz "https://github.com/lensfun/lensfun/archive/refs/tags/v${LENSFUN_VERSION}.tar.gz" \
    && mkdir -p /tmp/lensfun && tar -xzf /tmp/lensfun.tar.gz -C /tmp/lensfun --strip-components=1 \
    && cd /tmp/lensfun \
    && mingw64-cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
         -DBUILD_STATIC=OFF -DBUILD_TESTS=OFF -DBUILD_LENSTOOL=OFF -DBUILD_DOC=OFF \
         -DINSTALL_HELPER_SCRIPTS=OFF -DINSTALL_PYTHON_MODULE=OFF -DBUILD_FOR_SSE=OFF -DBUILD_FOR_SSE2=OFF \
    && cmake --build build && cmake --install build \
    && rm -rf /tmp/lensfun /tmp/lensfun.tar.gz

# libiptcdata (IPTC metadata)
RUN curl -fsSL -o /tmp/libiptcdata.tar.gz "https://github.com/ianw/libiptcdata/releases/download/${LIBIPTCDATA_TAG}/libiptcdata-${LIBIPTCDATA_VERSION}.tar.gz" \
    && mkdir -p /tmp/libiptcdata && tar -xzf /tmp/libiptcdata.tar.gz -C /tmp/libiptcdata --strip-components=1 \
    && cd /tmp/libiptcdata \
    && mingw64-configure --disable-nls --disable-static --enable-shared --disable-python \
    && make -C libiptcdata -j"$(nproc)" LDFLAGS="-no-undefined" && make -C libiptcdata install \
    && ${MINGW_TRIPLET}-pkg-config --modversion libiptcdata \
    && rm -rf /tmp/libiptcdata /tmp/libiptcdata.tar.gz

# libjxl (JPEG XL). Highway and skcms come vendored, brotli and lcms2 from the sysroot.
RUN if [ "$WITH_JXL" = "1" ]; then \
      git clone --depth 1 --branch "$LIBJXL_VERSION" https://github.com/libjxl/libjxl.git /tmp/libjxl \
      && cd /tmp/libjxl && git submodule update --init --recursive --depth 1 --recommend-shallow \
      && mingw64-cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
           -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON \
           -DJPEGXL_FORCE_SYSTEM_BROTLI=ON -DJPEGXL_FORCE_SYSTEM_LCMS2=ON -DJPEGXL_FORCE_SYSTEM_HWY=OFF \
           -DBUILD_TESTING=OFF -DJPEGXL_ENABLE_FUZZERS=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF \
           -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_TOOLS=OFF \
           -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF \
           -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_VIEWERS=OFF \
           -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_BUNDLE_SKCMS=ON \
           -DJPEGXL_ENABLE_TCMALLOC=OFF -Wno-dev \
      && cmake --build build && cmake --install build \
      && rm -rf /tmp/libjxl; \
    else echo "libjxl disabled"; fi

# SVG loader for gdk-pixbuf, built from librsvg's (public-API-only) C loader
# against the sysroot librsvg. Fedora's loaders.cache already lists it.
RUN mkdir -p /tmp/svgloader && cd /tmp/svgloader \
    && curl -fsSL -o io-svg.c "https://gitlab.gnome.org/GNOME/librsvg/-/raw/${LIBRSVG_LOADER_REF}/gdk-pixbuf-loader/io-svg.c" \
    && : > config.h \
    && ${MINGW_TRIPLET}-gcc -O2 -shared -o libpixbufloader-svg.dll io-svg.c -I. \
         -DGDK_PIXBUF_ENABLE_BACKEND \
         $(${MINGW_TRIPLET}-pkg-config --cflags --libs librsvg-2.0 gdk-pixbuf-2.0) \
    && install -m 755 libpixbufloader-svg.dll "${MINGW_PREFIX}/lib/gdk-pixbuf-2.0/2.10.0/loaders/" \
    && grep -q 'libpixbufloader-svg.dll' "${MINGW_PREFIX}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" \
    && rm -rf /tmp/svgloader

# fmt is normally pulled by FetchContent at configure time; pre-clone the exact
# commit cmake/Dependencies.cmake asks for so the build itself is offline.
RUN git clone https://github.com/fmtlib/fmt /opt/src/fmt \
    && git -C /opt/src/fmt checkout --quiet "$FMT_COMMIT"

# Compiled GSettings schemas for GTK (the sysroot ships only the XML sources).
RUN glib-compile-schemas "${MINGW_PREFIX}/share/glib-2.0/schemas" \
    && test -f "${MINGW_PREFIX}/share/glib-2.0/schemas/gschemas.compiled"

# Newer lensfun database than the one in the lensfun tarball, best effort.
# lensfun-update-data (the host python script shipped by lensfun) is not
# packaged on Fedora minimal images, so fetch the db tarball directly.
RUN mkdir -p /opt/lensfun-db && cd /opt/lensfun-db \
    && (curl -fsSL -o version_1.tar.bz2 "https://lensfun.github.io/db/version_1.tar.bz2" \
        && mkdir version_1 && tar -xjf version_1.tar.bz2 -C version_1 && rm version_1.tar.bz2 \
        && ls version_1 | head -3) \
       || { echo "lensfun db download failed, using the tarball database"; rm -rf /opt/lensfun-db/version_1; }

ENV WINEDEBUG=-all
WORKDIR /work
