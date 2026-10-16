#!/usr/bin/env bash
# Runs inside the toolchain image (see scripts/build-tizen.sh).
set -euo pipefail

cmake -S . -B build/tizen -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TSL_EMSCRIPTEN_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release
cmake --build build/tizen
python3 scripts/check-tizen-build.py build/tizen
rm -rf build/tizen/stage
cmake --install build/tizen --prefix build/tizen/stage

widget=build/tizen/stage/widget
rm -f "$widget"/*.wgt
# `tizen package` asks for the author certificate password and whether to remember it.
expect <<EOF
set timeout 300
spawn tizen package -t wgt -s TSL -- $widget
expect {
    "Author password:" { send "1234\r"; exp_continue }
    "Yes: (Y), No: (N) ?" { send "N\r"; exp_continue }
    eof
}
catch wait result
exit [lindex \$result 3]
EOF

mkdir -p out
wgt=$(ls "$widget"/*.wgt | head -n 1)
mv "$wgt" out/TizenSteamLink.wgt
