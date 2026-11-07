#!/bin/sh
# Brick OS Optional Pak: hide the feature again (its data is kept).
set -u
: "${XTRAS_STATE_DIR:=$SDCARD_PATH/.userdata/shared/xtras}"
rm -f "$XTRAS_STATE_DIR/radio-podcasts.version"
echo "@100 Done"
exit 0
