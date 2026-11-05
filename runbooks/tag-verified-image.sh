#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
# =============================================================================
# Tag verified Docker image — run once when a phase baseline is confirmed
# Usage: ./tag-verified-image.sh phase0
# =============================================================================

PHASE="${1:-phase0}"
DATE=$(date +%Y%m%d)
SOURCE_IMAGE="automotive-dash-platform:latest"
TARGET_TAG="automotive-dash-platform:${PHASE}-verified-${DATE}"

echo "Tagging $SOURCE_IMAGE → $TARGET_TAG"
docker tag "$SOURCE_IMAGE" "$TARGET_TAG"
echo "Done. Update runbook DOCKER_IMAGE variable to: $TARGET_TAG"
docker image ls | grep automotive-dash-platform
