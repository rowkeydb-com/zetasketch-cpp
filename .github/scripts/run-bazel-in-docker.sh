#!/bin/bash
# Run a Bazel command inside the Limen CI Docker container.
#
# Usage: run-bazel-in-docker.sh <config> <command>
#   config:  release | asan-ubsan | tsan | msan | clang-tidy | clang-tidy-analyzer | coverage | fuzzer
#   command: build | test | coverage
#
# The CI workflows build the Docker image (tag: zetasketch-cpp-ci) before
# invoking this script. Local developers can either rely on the
# pre-built image or let this script build the image on first run.
#
# Adapted from rowkeydb/.github/scripts/run-bazel-in-docker.sh and
# simplified for the GitHub-hosted-runner case: no host-cache
# mounting, no low-priority knob, no concurrency lock.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
readonly SCRIPT_DIR
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
readonly WORKSPACE_DIR
readonly DOCKER_IMAGE="zetasketch-cpp-ci"
CONTAINER_NAME="zetasketch-cpp-ci-$$"
readonly CONTAINER_NAME

# Always remove the container on exit, including on Ctrl-C / SIGTERM.
# shellcheck disable=SC2317  # invoked indirectly via the EXIT trap below
cleanup() {
    if docker inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
        docker stop -t 5 "$CONTAINER_NAME" >/dev/null 2>&1 || true
        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if [ $# -lt 2 ]; then
    echo "Usage: $0 <config> <command> [bazel_args...]" >&2
    echo "  config:  release | asan-ubsan | tsan | msan | clang-tidy | clang-tidy-analyzer | coverage | fuzzer" >&2
    echo "  command: build | test | coverage" >&2
    exit 1
fi

CONFIG="$1"
COMMAND="$2"
shift 2

case "$CONFIG" in
    release|asan-ubsan|tsan|msan-amd64|msan-arm64|clang-tidy|clang-tidy-analyzer|coverage|fuzzer) ;;
    *)
        echo "Error: invalid config '$CONFIG'" >&2
        exit 1
        ;;
esac

case "$COMMAND" in
    build|test|coverage) ;;
    *)
        echo "Error: invalid command '$COMMAND'" >&2
        exit 1
        ;;
esac

# Build the image locally if it isn't present. In CI the
# docker/build-push-action step has already loaded it.
if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
    echo "==> Building $DOCKER_IMAGE image..."
    docker build -t "$DOCKER_IMAGE" \
        -f "$WORKSPACE_DIR/.github/Dockerfile.ci" "$WORKSPACE_DIR"
fi

DOCKER_OPTS=(--name "$CONTAINER_NAME")

# TSan and MSan need the personality() syscall, which seccomp blocks by default.
if [ "$CONFIG" = "tsan" ] || [ "$CONFIG" = "msan-amd64" ] || [ "$CONFIG" = "msan-arm64" ]; then
    DOCKER_OPTS+=(--security-opt seccomp=unconfined)
fi

# Map the host user so output files are owned by the caller, not root.
DOCKER_OPTS+=(--user "$(id -u):$(id -g)")
DOCKER_OPTS+=(-e HOME=/tmp)
DOCKER_OPTS+=(-e USER=zetasketch-cpp)
DOCKER_OPTS+=(-v "$WORKSPACE_DIR:/workspace")
if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
    mkdir -p "$WORKSPACE_DIR/.bazel-cache"
    DOCKER_OPTS+=(-v "$WORKSPACE_DIR/.bazel-cache:/tmp/bazel-cache")
else
    mkdir -p "$HOME/.cache/bazel"
    DOCKER_OPTS+=(-v "$HOME/.cache/bazel:/tmp/bazel-cache")
    if [ -f "$WORKSPACE_DIR/.local_docker_opts.sh" ]; then
        # shellcheck disable=SC1091
        source "$WORKSPACE_DIR/.local_docker_opts.sh"
    fi
fi
DOCKER_OPTS+=(-w /workspace)

BAZEL_OPTS=()
BAZEL_OPTS+=(--disk_cache=/tmp/bazel-cache --repository_cache=/tmp/bazel-cache/repos)

# Bazel runs inside the container, where none of the runner's
# environment is visible, so BuildBuddy has nothing to detect and
# records every invocation with no repository, branch, commit or user.
# The values are read here on the host, where the runner's environment
# is present, and passed in as flags.
#
# Only what GitHub already publishes about a public repository is sent:
# the repository URL, the branch, the commit, the account that
# triggered the run, and the runner's platform. The API key travels in
# a request header rather than in metadata, and the environment is left
# redacted, so nothing here widens what BuildBuddy can see.
if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
    workflow="${GITHUB_WORKFLOW:-unknown}"
    workflow="${workflow// /-}"
    server="${GITHUB_SERVER_URL:-https://github.com}"
    repository="${GITHUB_REPOSITORY:-}"

    # The command is worth a tag of its own only when it differs from
    # the configuration; for coverage the two coincide.
    tags="${workflow},${CONFIG}"
    if [ "$COMMAND" != "$CONFIG" ]; then
        tags="${tags},${COMMAND}"
    fi

    BAZEL_OPTS+=(
        --build_metadata=ROLE=CI
        --build_metadata=USER="${GITHUB_ACTOR:-ci}"
        --build_metadata=HOST="${RUNNER_OS:-unknown}-${RUNNER_ARCH:-unknown}"
        --build_metadata=COMMIT_SHA="${GITHUB_SHA:-}"
        --build_metadata=BRANCH_NAME="${GITHUB_HEAD_REF:-${GITHUB_REF_NAME:-}}"
        --build_metadata=TAGS="${tags}"
        --build_metadata=COMMIT_STATUS_LABEL="${workflow}-${CONFIG}"
    )

    if [ -n "$repository" ]; then
        BAZEL_OPTS+=(--build_metadata=REPO_URL="${server}/${repository}")
        BAZEL_OPTS+=(
            --build_metadata=BUILDBUDDY_LINKS="[Workflow-run](${server}/${repository}/actions/runs/${GITHUB_RUN_ID:-0})"
        )
    fi
fi
if [ -n "${BAZEL_REMOTE_CACHE:-}" ]; then
    BAZEL_OPTS+=(--remote_cache="$BAZEL_REMOTE_CACHE" --remote_upload_local_results=true)
    DOCKER_OPTS+=(--add-host=host.docker.internal:host-gateway)
fi


set +e
if [ "$COMMAND" = "coverage" ]; then
    # The `coverage` lines in .bazelrc already apply to this
    # subcommand, including `--config=clang` for thread-safety and
    # `--combined_report=lcov`. Don't pass `--config=$CONFIG` here:
    # CONFIG is the script's own "coverage" sentinel, not a Bazel
    # build config, and Bazel would reject it as undefined.
    # Run the build and copy the report file out to the workspace
    # root in a single container invocation, so paths inside the
    # container (where Bazel's symlinks resolve) are valid.
    # The arguments are passed positionally rather than interpolated
    # into the command string, so that a metadata value holding a
    # bracket or a parenthesis reaches Bazel intact instead of being
    # read as shell syntax.
    docker run "${DOCKER_OPTS[@]}" "$DOCKER_IMAGE" bash -c '
        bazel coverage "$@" //...
        rc=$?
        if [ "$rc" -eq 0 ]; then
            cp "$(bazel info output_path)/_coverage/_coverage_report.dat" \
               /workspace/coverage.lcov
        elif [ "$rc" -eq 4 ]; then
            : > /workspace/coverage.lcov
            echo "No test targets in the repository yet; emitting empty coverage.lcov."
        fi
        exit $rc
    ' coverage-in-container "${BAZEL_OPTS[@]}" "$@"
else
    docker run "${DOCKER_OPTS[@]}" "$DOCKER_IMAGE" \
        bazel "$COMMAND" "${BAZEL_OPTS[@]}" --config="$CONFIG" "$@" //...
fi
rc=$?
set -e

# Bazel exits with code 4 when `bazel test` (or `bazel coverage`) is asked
# to run tests but none exist. Commit 1 ships no test targets; later
# commits populate them. Treat exit code 4 as success for the test and
# coverage subcommands. Once test targets exist, this branch becomes
# inert.
if [ "$rc" -eq 4 ] && { [ "$COMMAND" = "test" ] || [ "$COMMAND" = "coverage" ]; }; then
    echo "No test targets in the repository yet; treating as success."
    exit 0
fi
exit "$rc"
