#!/bin/sh

git merge pr-1338 --no-edit -m "Integration of upstrem PR #1338"
git merge pr-1297 --no-edit -m "Integration of upstrem PR #1297"

git merge pio-ini-adjustments -m "platformio.ini: Adjust defaults for LoRa frequncies and advert interval limits"


