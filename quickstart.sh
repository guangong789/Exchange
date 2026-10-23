#!/usr/bin/env bash
set -euo pipefail

build_tests=OFF
target=exchange_runtime_demo
if (( $# > 0 )); then
    if (( $# != 1 )) || [[ $1 != test ]]; then
        printf 'Usage: %s [test]\n' "$0" >&2
        exit 2
    fi
    build_tests=ON
    target=exchange_test
fi

required_commands=(uname cmake)
if [[ $build_tests == ON ]]; then
    required_commands+=(ctest)
fi
for command_name in "${required_commands[@]}"; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'quickstart: required command not found: %s\n' "$command_name" >&2
        exit 127
    fi
done
if [[ $(uname -s) != Linux ]]; then
    printf 'quickstart: Linux or WSL2 is required.\n' >&2
    exit 1
fi

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="$repo_root/build-quickstart"

# Keep the original tool output and exit status, with the failing step named.
run_step() {
    local step=$1
    shift
    local status
    if "$@"; then
        return 0
    else
        status=$?
        printf 'quickstart: %s failed (exit %s).\n' "$step" "$status" >&2
        return "$status"
    fi
}

printf '[1/3] Configure\n'
run_step Configuration cmake -S "$repo_root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DEXCHANGE_BUILD_DEEPSEEK_PROVIDER=OFF \
    -DEXCHANGE_BUILD_BINANCE_ALPHA_FEED=OFF \
    -DEXCHANGE_BUILD_BENCHMARKS=OFF \
    -DEXCHANGE_BUILD_AGENT_LIVE_SMOKE=OFF \
    -DEXCHANGE_BUILD_TESTS="$build_tests"
if [[ $build_tests == ON ]]; then
    printf '[2/3] Build C++ tests\n'
else
    printf '[2/3] Build demo\n'
fi
run_step Build cmake --build "$build_dir" --target "$target" --parallel 4

if [[ $build_tests == ON ]]; then
    printf '\n[3/3] Run CTest\n'
    run_step Tests ctest --test-dir "$build_dir" --output-on-failure --no-tests=error
else
    demo="$build_dir/exchange_runtime_demo"
    if [[ ! -x $demo ]]; then
        printf 'quickstart: demo executable not found or not executable: %s\n' "$demo" >&2
        exit 1
    fi
    printf '\n[3/3] Run demo\n'
    run_step Demo "$demo"
fi
