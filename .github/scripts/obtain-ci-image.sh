#!/bin/bash
# Obtain the CI container image and make it available under the local
# name the build script expects.
#
# The image is published by the publish-ci-image workflow and tagged by
# a hash of the Dockerfile, so the tag this asks for exists only if it
# was built from the same Dockerfile as the checkout. When it is not
# available the image is built here instead, which is what happens on a
# pull request from a fork, whose token cannot read the registry, and
# in the window before the publishing workflow has finished with a
# newly changed Dockerfile.
#
# Required environment: IMAGE_REPOSITORY, IMAGE_VERSION, LOCAL_IMAGE.
# Optional: STAGE (default base), REGISTRY_USER and REGISTRY_TOKEN.

set -euo pipefail

STAGE="${STAGE:-base}"
DOCKERFILE=".github/Dockerfile.ci"
readonly STAGE DOCKERFILE

image="${IMAGE_REPOSITORY}:${STAGE}-${IMAGE_VERSION}"

build_locally() {
    echo "::warning::$image is unavailable; building the image from $DOCKERFILE"
    docker build --tag "$LOCAL_IMAGE" --target "$STAGE" --file "$DOCKERFILE" .
}

if [ -z "${REGISTRY_TOKEN:-}" ]; then
    echo "No registry token; building the image."
    build_locally
    exit 0
fi

registry="${IMAGE_REPOSITORY%%/*}"
if ! printf '%s' "$REGISTRY_TOKEN" \
    | docker login "$registry" --username "${REGISTRY_USER:-x}" --password-stdin; then
    echo "::warning::could not sign in to $registry"
    build_locally
    exit 0
fi

if docker pull --quiet "$image"; then
    docker tag "$image" "$LOCAL_IMAGE"
    echo "Using the published image $image"
else
    build_locally
fi
