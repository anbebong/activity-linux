#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ACTIVITY_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_TAG="${IMAGE_TAG:-activity-portable-builder:1.0.0}"
DOCKERFILE_PATH="${DOCKERFILE_PATH:-${ACTIVITY_DIR}/Dockerfile.activity-build}"
CONTAINER_NAME="activity-build-$$"
CONTAINER_SRC_DIR="/work/src"
HOST_OUT_DIR="${ACTIVITY_DIR}/out"

cleanup() {
    docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! command -v docker >/dev/null 2>&1; then
    echo "Error: docker is not installed or not in PATH." >&2
    exit 1
fi

if ! docker image inspect "${IMAGE_TAG}" >/dev/null 2>&1; then
    echo "Image ${IMAGE_TAG} not found. Building from ${DOCKERFILE_PATH}..."
    docker build \
        -f "${DOCKERFILE_PATH}" \
        -t "${IMAGE_TAG}" \
        "${ACTIVITY_DIR}"
else
    echo "Using existing image: ${IMAGE_TAG}"
fi

echo "Creating container: ${CONTAINER_NAME}"
docker create --name "${CONTAINER_NAME}" "${IMAGE_TAG}" tail -f /dev/null >/dev/null

echo "Preparing container workspace"
docker start "${CONTAINER_NAME}" >/dev/null
docker exec "${CONTAINER_NAME}" mkdir -p "${CONTAINER_SRC_DIR}"

echo "Copying source into container (no bind mount)"
docker cp "${ACTIVITY_DIR}/." "${CONTAINER_NAME}:${CONTAINER_SRC_DIR}"

echo "Building inside container"
docker exec "${CONTAINER_NAME}" bash -lc "cd ${CONTAINER_SRC_DIR} && make clean && make"

echo "Copying build output back to host"
mkdir -p "${HOST_OUT_DIR}"
docker cp "${CONTAINER_NAME}:${CONTAINER_SRC_DIR}/out/." "${HOST_OUT_DIR}/"

echo "Build completed. Artifacts are in: ${HOST_OUT_DIR}"
