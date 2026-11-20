#!/usr/bin/env bash
set -eu
export PATH="$(cygpath -u "$FFMPEG_MSVC_BIN"):$PATH"
cd "$(cygpath -u "$1")"
install_dir=$(cygpath -am "$2")
make_path=$(cygpath -u "$3")
asm_options=()
if [ "$4" = aarch64 ]; then
    asm_options+=(--disable-asm --enable-cross-compile)
fi
./configure --prefix="$install_dir" --toolchain=msvc --arch="$4" --target-os=win32 \
    "${asm_options[@]}" \
    --enable-shared --disable-static --disable-autodetect --enable-schannel --enable-small \
    --disable-doc --disable-programs --disable-debug --disable-avdevice --disable-avfilter --disable-postproc \
    --disable-protocols --enable-protocol=file,fd,pipe,http,https,tls,tcp,rtp,udp \
    --disable-demuxers --enable-demuxer=h264,mp3,mov,mjpeg,mpjpeg,rtsp,sdp,hls,mpegts \
    --disable-decoders --enable-decoder=aac*,h264*,hevc*,mp3*,mjpeg,rv*,opus \
    --disable-encoders --disable-muxers --enable-muxer=rtp
"$make_path" -j "$5"
"$make_path" install
