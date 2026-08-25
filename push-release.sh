#!/bin/sh
# Bumps version.txt to match src/version.h's NSHGEOIP_VERSION (committing it
# if it changed), then re-tags and force-pushes the vX.Y.Z release tag.
#
# version.txt is a discoverability convenience -- anyone who wants the latest
# released version without parsing version.h or hitting the GitHub API can
# just read it. It plays no part in the build itself: src/version.h stays the
# only real source of truth for what actually gets compiled into the binary.
set -eu

print_delim()
{
  echo "--------------------------------------------------------------------------------"
}

header()
{
  echo
  print_delim
  echo "$1"
  print_delim
  echo
}

VERSION=$(sed -n 's/.*NSHGEOIP_VERSION "\(.*\)".*/\1/p' src/version.h)
RELEASE="v$VERSION"

header "Pushing release $RELEASE"

echo "$VERSION" > version.txt
if ! git diff --quiet -- version.txt; then
    git add version.txt
    git commit -m "version.txt: $VERSION"
    git push origin HEAD
fi

git tag -d "$RELEASE" 2>/dev/null || true
git tag "$RELEASE"
git push --force origin "$RELEASE"
