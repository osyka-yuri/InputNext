#!/usr/bin/env bash
set -euo pipefail
[[ "$EVENT" == push && "$REF_TYPE" == tag ]]
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]]
[[ "$SOURCE_TAG" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]
[[ "$SOURCE_REF" == "refs/tags/$SOURCE_TAG" ]]
[[ "$WORKFLOW_REPOSITORY" == "$SOURCE_REPOSITORY" ]]
[[ "$WORKFLOW_SHA" == "$SOURCE_SHA" && "$CALLER_SHA" == "$SOURCE_SHA" ]]
[[ "$(git rev-parse HEAD)" == "$SOURCE_SHA" ]]
[[ "$(git rev-parse --verify --end-of-options "$SOURCE_REF^{commit}")" == "$SOURCE_SHA" ]]
git merge-base --is-ancestor "$SOURCE_SHA" refs/remotes/origin/main
