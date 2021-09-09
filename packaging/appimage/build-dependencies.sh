#!/bin/bash

# Halt on errors
set -e

# Be verbose
set -x

#apt-get update && apt-get install -y apt-transport-https ca-certificates gnupg software-properties-common wget
#wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | apt-key add -
#add-apt-repository -y ppa:openjdk-r/ppa && apt-add-repository 'deb https://apt.kitware.com/ubuntu/ xenial main'

## Update the system and bring in our core operating requirements
#apt-get update && apt-get upgrade -y && apt-get install -y openssh-server openjdk-8-jre-headless

## Some software demands a newer GCC because they're using C++14 stuff, which is just insane
## We do this after the general system update to ensure it doesn't bring in any unnecessary updates
#add-apt-repository -y ppa:ubuntu-toolchain-r/test && apt-get update

## Now install the general dependencies we need for builds
#apt-get install -y build-essential cmake git-core locales automake gcc-6 g++-6 libxml-parser-perl libpq-dev libaio-dev bison gettext gperf libasound2-dev libatkmm-1.6-dev libbz2-dev libcairo-perl libcap-dev libcups2-dev libdbus-1-dev libdrm-dev libegl1-mesa-dev libfontconfig1-dev libfreetype6-dev libgcrypt11-dev libgl1-mesa-dev libglib-perl libgsl0-dev libgsl0-dev gstreamer1.0-alsa libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgtk2-perl libjpeg-dev libnss3-dev libpci-dev libpng12-dev libpulse-dev libssl-dev libgstreamer-plugins-good1.0-dev libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly libtiff5-dev libudev-dev libwebp-dev flex libmysqlclient-dev libx11-dev libxkbcommon-x11-dev libxcb-glx0-dev libxcb-keysyms1-dev libxcb-util0-dev libxcb-res0-dev libxcb1-dev libxcomposite-dev libxcursor-dev libxdamage-dev libxext-dev libxfixes-dev libxi-dev libxrandr-dev libxrender-dev libxss-dev libxtst-dev mesa-common-dev liblist-moreutils-perl libtool libpixman-1-dev subversion

#apt-get -y install libpixman-1-dev docbook-xml docbook-xsl libattr1-dev

## Required for vaapi gpu encoding
#apt-get -y install libva-dev

# Read in our parameters
export BUILD_PREFIX=$1
export KDENLIVE_SOURCES=$2

# qjsonparser, used to add metadata to the plugins needs to work in a en_US.UTF-8 environment.
# That's not always the case, so make sure it is
export LC_ALL=en_US.UTF-8
export LANG=en_us.UTF-8

# We want to use $prefix/deps/usr/ for all our dependencies
export DEPS_INSTALL_PREFIX=$BUILD_PREFIX/deps/usr
export DOWNLOADS_DIR=$BUILD_PREFIX/downloads

# Setup variables needed to help everything find what we build
export LD_LIBRARY_PATH=$DEPS_INSTALL_PREFIX/lib:$DEPS_INSTALL_PREFIX/openssl/lib:$LD_LIBRARY_PATH
export PATH=$DEPS_INSTALL_PREFIX/bin:$DEPS_INSTALL_PREFIX/openssl/bin:$PATH
export PKG_CONFIG_PATH=$DEPS_INSTALL_PREFIX/share/pkgconfig:$DEPS_INSTALL_PREFIX/lib/pkgconfig:$DEPS_INSTALL_PREFIX/openssl/lib/pkgconfig:/usr/lib/pkgconfig:$PKG_CONFIG_PATH

# A kdenlive build layout looks like this:
# kdenlive/ -- the source directory
# downloads/ -- downloads of the dependencies from files.kde.org
# deps-build/ -- build directory for the dependencies
# deps/ -- the location for the built dependencies
# build/ -- build directory for kdenlive itself
# kdenlive.appdir/ -- install directory for kdenlive and the dependencies

# Make sure our downloads directory exists
if [ ! -d $DOWNLOADS_DIR ] ; then
    mkdir -p $DOWNLOADS_DIR
fi

# Make sure our build directory exists
if [ ! -d $BUILD_PREFIX/deps-build/ ] ; then
    mkdir -p $BUILD_PREFIX/deps-build/
fi

# The 3rdparty dependency handling in Kdenlive also requires the install directory to be pre-created
if [ ! -d $DEPS_INSTALL_PREFIX ] ; then
    mkdir -p $DEPS_INSTALL_PREFIX
fi

# Switch to our build directory as we're basically ready to start building...
cd $BUILD_PREFIX/deps-build/

# Configure the dependencies for building
cmake $KDENLIVE_SOURCES/packaging/appimage/3rdparty -DCMAKE_INSTALL_PREFIX=$DEPS_INSTALL_PREFIX -DEXT_INSTALL_DIR=$DEPS_INSTALL_PREFIX -DEXT_DOWNLOAD_DIR=$DOWNLOADS_DIR -DEXT_BUILD_DIR=$BUILD_PREFIX

CPU_CORES=$(grep -c ^processor /proc/cpuinfo 2>/dev/null || sysctl -n hw.ncpu)

if [[ $CPU_CORES -gt 1 ]]; then
    CPU_CORES=$((CPU_CORES-1))
fi

echo "CPU Cores to use : $CPU_CORES"

# Now start building everything we need, in the appropriate order

cmake --build . --target ext_lzma  -j$CPU_CORES
cmake --build . --target ext_icu  -j$CPU_CORES
cmake --build . --target ext_xml  -j$CPU_CORES
cmake --build . --target ext_gettext  -j$CPU_CORES
cmake --build . --target ext_xslt  -j$CPU_CORES
cmake --build . --target ext_png  -j$CPU_CORES
cmake --build . --target ext_webp  -j$CPU_CORES


export CC=/usr/bin/gcc-6
export CXX=/usr/bin/g++-6

cmake --build . --target ext_openssl
cmake --build . --target ext_cmake
cmake --build . --target ext_qt

cmake --build . --target ext_boost  -j$CPU_CORES
cmake --build . --target ext_gpgme  -j$CPU_CORES
cmake --build . --target ext_libsndfile  -j$CPU_CORES
cmake --build . --target ext_libsamplerate  -j$CPU_CORES
cmake --build . --target ext_nasm  -j$CPU_CORES
cmake --build . --target ext_yasm  -j$CPU_CORES
cmake --build . --target ext_alsa  -j$CPU_CORES
cmake --build . --target ext_sdl2  -j$CPU_CORES

cmake --build . --target ext_fftw3  -j$CPU_CORES
cmake --build . --target ext_fftw3f  -j$CPU_CORES

# ladspa expects fft3w.pc pkgconfig files
cp $DEPS_INSTALL_PREFIX/lib/pkgconfig/fftwf.pc $DEPS_INSTALL_PREFIX/lib/pkgconfig/fftw3f.pc
cp $DEPS_INSTALL_PREFIX/lib/pkgconfig/fftw.pc $DEPS_INSTALL_PREFIX/lib/pkgconfig/fftw3.pc

cmake --build . --target ext_x264  -j$CPU_CORES
cmake --build . --target ext_x265  -j$CPU_CORES

# libvpx does not compile with this gcc6 version
export CC=/usr/bin/gcc
export CXX=/usr/bin/g++

cmake --build . --target ext_libvpx  -j$CPU_CORES

export CC=/usr/bin/gcc-6
export CXX=/usr/bin/g++-6


cmake --build . --target ext_opus  -j$CPU_CORES
cmake --build . --target ext_nv-codec-headers -j$CPU_CORES
cmake --build . --target ext_amf  -j$CPU_CORES
cmake --build . --target ext_mfx  -j$CPU_CORES
cmake --build . --target ext_cairo  -j$CPU_CORES
cmake --build . --target ext_harfbuzz  -j$CPU_CORES
cmake --build . --target ext_pango  -j$CPU_CORES
cmake --build . --target ext_gdkpixbuf  -j$CPU_CORES
cmake --build . --target ext_gtk+  -j$CPU_CORES
cmake --build . --target ext_fribidi  -j$CPU_CORES
cmake --build . --target ext_libass  -j$CPU_CORES
cmake --build . --target ext_libva  -j$CPU_CORES
cmake --build . --target ext_lame  -j$CPU_CORES
cmake --build . --target ext_ogg  -j$CPU_CORES
cmake --build . --target ext_vorbis  -j$CPU_CORES
cmake --build . --target ext_ffmpeg  -j$CPU_CORES
cmake --build . --target ext_sox  -j$CPU_CORES
cmake --build . --target ext_jack  -j$CPU_CORES
cmake --build . --target ext_ladspa  -j$CPU_CORES
cmake --build . --target ext_tap-plugins  -j$CPU_CORES
cmake --build . --target ext_gavl  -j$CPU_CORES
cmake --build . --target ext_frei0r  -j$CPU_CORES
cmake --build . --target ext_vidstab  -j$CPU_CORES
cmake --build . --target ext_opencv  -j$CPU_CORES

export CC=/usr/bin/gcc-6
export CXX=/usr/bin/g++-6


cmake --build . --target ext_ruby
cmake --build . --target ext_frameworks
cmake --build . --config RelWithDebInfo --target ext_extra-cmake-modules -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kconfig             -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_breeze-icons        -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kcoreaddons         -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kwindowsystem       -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_solid               -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_threadweaver        -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_karchive            -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kdbusaddons         -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kirigami2 -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_qqc2-desktop-style -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_ki18n               -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kcrash              -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kcodecs             -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kauth               -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kguiaddons          -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kwidgetsaddons      -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kitemviews          -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kcompletion         -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kconfigwidgets      -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kservice            -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kiconthemes         -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_attica        -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kglobalaccel        -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kxmlgui             -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kbookmarks          -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kjobwidgets                 -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_sonnet                 -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_ktextwidgets                 -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kio                 -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_knotifyconfig       -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kpackage                -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_knewstuff           -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_knotifications -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kdeclarative        -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kservice            -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kimageformats       -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_frameworkintegration -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_kactivities -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_plasma-framework -- -j$CPU_CORES
cmake --build . --config RelWithDebInfo --target ext_fcitx-qt5 -- -j$CPU_CORES

cmake --build . --target ext_breeze

cmake --build . --target ext_rubberband
cmake --build . --target ext_bigshot
