# Build environment for the RawTherapee AppImage.
#
# Mirrors .github/workflows/appimage.yml: Ubuntu 22.04 (glibc 2.35, so the
# resulting AppImage runs on any distro from 2022 on), Exiv2 0.28 and libjxl
# built from source because the distro versions are too old, linuxdeploy +
# its GTK plugin for the packaging step.
#
# Everything that needs the network happens here, at image build time; the
# per-build script (build-inside.sh) runs offline.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG EXIV2_VERSION=v0.28.8
ARG LIBJXL_VERSION=v0.11.2
ARG LINUXDEPLOY_TAG=1-alpha-20240109-1
ARG FMT_COMMIT=e424e3f2e607da02742f73db84873b8084fc714c

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates curl \
        autoconf automake libtool gettext file rsync zstd xz-utils bzip2 \
        uuid-runtime python3 patchelf desktop-file-utils \
        libgtk-3-dev libgtkmm-3.0-dev librsvg2-dev librsvg2-common \
        liblensfun-dev liblensfun-bin liblensfun-data-v1 \
        liblcms2-dev libfftw3-dev libiptcdata0-dev libtiff5-dev libjpeg-dev libpng-dev \
        libcanberra-gtk3-dev libexpat1-dev libbrotli-dev zlib1g-dev libinih-dev \
        libgif-dev libwebp-dev libwebpdemux2 libgflags-dev \
        adwaita-icon-theme-full hicolor-icon-theme libglib2.0-bin gtk-update-icon-cache \
        libarchive-tools xvfb \
    && rm -rf /var/lib/apt/lists/*

# Exiv2 0.28 (Ubuntu 22.04 ships 0.27, RawTherapee wants the 0.28 API).
RUN git clone --depth 1 --branch "$EXIV2_VERSION" https://github.com/Exiv2/exiv2.git /tmp/exiv2 \
    && cmake -S /tmp/exiv2 -B /tmp/exiv2/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/usr/local -DEXIV2_BUILD_SAMPLES=OFF -DEXIV2_BUILD_UNIT_TESTS=OFF \
         -DEXIV2_ENABLE_INIH=ON -DEXIV2_ENABLE_BROTLI=ON \
    && cmake --build /tmp/exiv2/build && cmake --install /tmp/exiv2/build \
    && rm -rf /tmp/exiv2 && ldconfig

# libjxl (JPEG XL support), same recipe as tools/makedeb/PKGBUILD.libjxl.
RUN git clone --depth 1 --branch "$LIBJXL_VERSION" https://github.com/libjxl/libjxl.git /tmp/libjxl \
    && cd /tmp/libjxl && git submodule update --init --recursive --depth 1 --recommend-shallow \
    && cmake -S /tmp/libjxl -B /tmp/libjxl/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=/usr/local \
         -DJPEGXL_FORCE_SYSTEM_BROTLI=ON -DBUILD_TESTING=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF \
         -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_TOOLS=OFF \
         -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -Wno-dev \
    && cmake --build /tmp/libjxl/build && cmake --install /tmp/libjxl/build \
    && rm -rf /tmp/libjxl && ldconfig

# fmt is normally pulled by FetchContent at configure time; pre-clone the exact
# commit cmake/Dependencies.cmake asks for so the build itself is offline.
RUN git clone https://github.com/fmtlib/fmt /opt/src/fmt \
    && git -C /opt/src/fmt checkout --quiet "$FMT_COMMIT"

# linuxdeploy and its GTK plugin (the plugin's forced GTK_THEME export is
# removed, exactly as upstream CI does).
RUN mkdir -p /opt/linuxdeploy && cd /opt/linuxdeploy \
    && curl -fsSL -o linuxdeploy-x86_64.AppImage \
         "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_TAG}/linuxdeploy-x86_64.AppImage" \
    && curl -fsSL "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" \
         | sed 's/^\(export GTK_THEME\)/#\1/' > linuxdeploy-plugin-gtk.sh \
    && chmod +x linuxdeploy-*

# Lensfun camera/lens database: try to fetch the latest one, fall back to the
# copy shipped by Ubuntu (liblensfun-data-v1) at build time if this fails.
RUN sed -i 's/HTTPError\(, ValueError\)/URLError\1/' "$(which lensfun-update-data)" \
    && (lensfun-update-data && ls /var/lib/lensfun-updates/version_1 | head -3) \
       || echo "lensfun-update-data failed, the distro database will be used"

ENV APPIMAGE_EXTRACT_AND_RUN=1
WORKDIR /work
