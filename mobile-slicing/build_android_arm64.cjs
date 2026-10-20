const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const root = path.resolve(__dirname, '..');
const build = path.join(__dirname, 'build-android-arm64');
const evidence = path.resolve(root, '..', '..', '..', 'artifacts', 'p1-slice-native-build-20261021', 'evidence');
fs.mkdirSync(evidence, { recursive: true });
const cmake = process.env.CMAKE || 'cmake';
const ninja = process.env.NINJA || 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe';
const ndk = process.env.ANDROID_NDK || 'C:/Users/1/AppData/Local/Android/Sdk/ndk/28.2.13676358';
const args = ['-S', path.join(root, 'mobile-slicing'), '-B', build, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM='+ninja, '-DCMAKE_TOOLCHAIN_FILE='+path.join(ndk, 'build/cmake/android.toolchain.cmake'), '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-35', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release'];
const log = path.join(evidence, 'android-arm64-build.log');
const out = fs.openSync(log, 'a');
function run(a) {
    const r = cp.spawnSync(cmake, a, { cwd: root, stdio: ['ignore', out, out], windowsHide: true });
    if (r.status !== 0) process.exit(r.status || 1);
}
run(args);
run(['--build', build, '--target', 'libslic3r_mobile', '-j2']);
