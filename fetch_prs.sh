#!/bin/sh

git branch -D pr-1297
git branch -D pr-1338
git branch -D pr-1398

# fetch PRs
git fetch upstream pull/1398/head:pr-1398
git fetch upstream pull/1338/head:pr-1338
git fetch upstream pull/1297/head:pr-1297
