#!/system/bin/sh
# CorePatch-Zygisk post-install / pre-uninstall hooks.
# This module needs no heavy install logic: Zygisk auto-places the .so into
# the zygisk namespace and the native entry injects the bundled dex.

SKIPUNZIP=0

# Seed a default configuration file so the module works out of the box.
# NOTE: customize.sh runs at INSTALL time: MODPATH is defined, BOOTMODE is not.
CONFIG_DIR="$MODPATH/config"
CONFIG_FILE="$CONFIG_DIR/corepatch.conf"
if [ ! -f "$CONFIG_FILE" ]; then
  mkdir -p "$CONFIG_DIR"
  cat > "$CONFIG_FILE" <<'EOF'
# CorePatch-Zygisk configuration
# Each line: <key>=<0|1>  (1 = enabled). Keys match Config.kt.
downgrade=1
bypass_min_sdk=1
bypass_verification=1
bypass_resource_arsc_restrictions=1
bypass_digest=1
bypass_exact_sig_match=0
use_previous_signatures=0
allow_hidden_apis_for_system_apps=1
bypass_shared_user=1
disable_verification_agent=1
bypass_block=1
EOF
  chmod 644 "$CONFIG_FILE"
  ui_print "- Seeded config/corepatch.conf"
else
  ui_print "- Keep existing config/corepatch.conf"
fi

ui_print "- CorePatch-Zygisk installed"
