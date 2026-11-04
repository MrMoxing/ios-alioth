#!/bin/bash
# Show the context of the check_all_exports failure to judge whether it is fatal.
grep -B5 "check_all_exports" /tmp/build2.log | head -40
echo ====
grep -B3 -m2 "Error 1" /tmp/build2.log | head -30
