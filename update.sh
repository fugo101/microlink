#!/usr/bin/env bash
set -e

COMPAT_BRANCH="esp-idf-6x-compat"
UPSTREAM_REMOTE="upstream"
UPSTREAM_BRANCH="main"

echo "==> Fetching upstream..."
git fetch "$UPSTREAM_REMOTE"

NEW_COMMITS=$(git log HEAD.."$UPSTREAM_REMOTE/$UPSTREAM_BRANCH" --oneline)
if [ -z "$NEW_COMMITS" ]; then
    echo "==> Already up to date. Nothing to do."
    exit 0
fi

echo ""
echo "==> New upstream commits:"
echo "$NEW_COMMITS"
echo ""

read -r -p "Rebase $COMPAT_BRANCH onto upstream/$UPSTREAM_BRANCH? [y/N] " confirm
if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
    echo "Aborted."
    exit 0
fi

CURRENT_BRANCH=$(git branch --show-current)
if [ "$CURRENT_BRANCH" != "$COMPAT_BRANCH" ]; then
    echo "==> Switching to $COMPAT_BRANCH..."
    git checkout "$COMPAT_BRANCH"
fi

echo "==> Rebasing..."
if ! git rebase "$UPSTREAM_REMOTE/$UPSTREAM_BRANCH"; then
    echo ""
    echo "ERROR: Rebase conflict. Resolve conflicts, then run:"
    echo "  git rebase --continue"
    echo "  git push -f origin $COMPAT_BRANCH"
    exit 1
fi

echo "==> Rebase successful."
echo ""
echo "==> Pushing to origin..."
git push --force-with-lease origin "$COMPAT_BRANCH"

echo ""
echo "==> Done. Copy updated components to your project:"
echo "  cp -r components/microlink   <project>/components/microlink"
echo "  cp -r components/wireguard_lwip <project>/components/wireguard_lwip"
