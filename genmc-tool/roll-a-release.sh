#!/bin/bash
#
# Rolls a new release to the public repository.
# It excludes internal and Gitlab-specific files such as:
#
#    - .gitlab-ci.yml
#    - gitlab-ci/
#    - scripts/check-regression.sh
#    - scripts/run-parallel.sh
#    - Dockerfile
#    - .dockerignore
#    - /doc/manual.org
#    - clean-artifacts.sh
#    - roll-a-release.sh (the script itself)
#
# This script needs to be run *before* merging dev to master,
# otherwise the commit from which the patch should be created
# needs to be passed as an argument.

VERSION=0.10.2
INTERNAL_PATH=/home/michalis/checkouts/genmc-tool
EXTERNAL_PATH=/home/michalis/checkouts/genmc-github
PATCH_PATH=/tmp/release-"${VERSION}".patch
COMMIT=master

set -e

[ $# -gt 0 ] && COMMIT=$1 && shift

# cannot run this on a dumb terminal (TERM = dumb)
if [[ $TERM == "dumb" ]]
then
    echo "Do not release from a dumb terminal. Merging might be required."
    exit 1
fi

# update local github mirror
cd "${INTERNAL_PATH}"
git diff "${COMMIT}"..dev --binary -- . ':!./.gitmodules' ':!./.gitlab-ci.yml' ':!./gitlab-ci' ':!./scripts/check-regression.sh' ':!./scripts/renew-drivers.sh' ':!./scripts/run-parallel.sh' ':!./clean-artifacts.sh' ':!./export-github-patch.sh' ':!./Dockerfile' ':!./.dockerignore' ':!./roll-a-release.sh' ':!./doc/manual.org' > "${PATCH_PATH}"
git checkout master && git merge dev && git tag "v${VERSION}"
git push origin master && git push origin "v${VERSION}"

# update github
cd "${EXTERNAL_PATH}"
git checkout master && git apply --whitespace=fix "${PATCH_PATH}"
autoreconf --install && ./configure && make -j8 && ./scripts/fast-driver.sh
git add -A && git commit -s -m "Release GenMC v${VERSION}" && git tag "v${VERSION}"
git push origin master && git push origin "v${VERSION}"
git clean -dfX

# publish dockerfile
cd "${INTERNAL_PATH}"
sudo docker build --no-cache -t genmc:"v${VERSION}" .
sudo docker image tag genmc:"v${VERSION}" genmc/genmc:"v${VERSION}"
sudo docker image tag genmc:"v${VERSION}" genmc/genmc:latest
sudo docker image push genmc/genmc:"v${VERSION}"
sudo docker image push genmc/genmc:latest

echo "A new release has been rolled."
