#!/bin/bash
# Install or update camwall on this machine. Safe to run repeatedly (for example
# after every `git pull` or system update): it only changes what differs, never
# overwrites /etc/camwall/camwall.conf, and restarts the service only when something
# changed or the service is not running.
#
#   ./install.sh [--no-apt] [--no-restart] [--dry-run]
#
#   --no-apt       do not install missing packages (offline or locked-down systems)
#   --no-restart   install the files but leave the running service alone
#   --dry-run      print what would be done and change nothing (does not need root)

set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
APT=1; RESTART=1; DRY=0
for a in "$@"; do
  case $a in
    --no-apt) APT=0 ;;
    --no-restart) RESTART=0 ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $a (see --help)" >&2; exit 2 ;;
  esac
done

if [ "$DRY" = 0 ] && [ "$EUID" -ne 0 ]; then
  exec sudo "${BASH_SOURCE[0]}" "$@"
fi

SVC_USER=camwall
SVC_GROUPS=(video render i2c)
CONF_DIR=/etc/camwall
CONF=$CONF_DIR/camwall.conf
OLD_CONF=$CONF_DIR/tokens.env   # name used before camwall.conf
BIN=/usr/local/bin/camwall          # the GStreamer wall (WALL=gstreamer)
DAEMON=/usr/local/bin/camwalld      # supervisor, started by the unit
PLANES=/usr/local/bin/camwall-planes  # the plane wall (WALL=planes, default)
UNIT=/etc/systemd/system/camwall.service
PACKAGES=(ffmpeg gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
          gstreamer1.0-plugins-bad v4l-utils libdrm-tests ddcutil python3
          gcc pkg-config libdrm-dev libavformat-dev libavcodec-dev libavutil-dev)

changed=0; file_changed=0
say() { echo "==> $*"; }
warn() { echo "!!! $*" >&2; }
run() { if [ "$DRY" = 1 ]; then echo "    + $*"; else "$@"; fi; }

# install_file MODE SRC DEST: install only if DEST is missing or differs.
install_file() {
  file_changed=0
  if [ -f "$3" ] && cmp -s "$2" "$3" && [ "$(stat -c %a "$3")" = "${1#0}" ]; then
    echo "    up to date: $3"
  else
    say "installing $3"
    run install -m "$1" -o root -g root "$2" "$3"
    changed=1; file_changed=1
  fi
}

# --- packages ---------------------------------------------------------------
missing=()
for p in "${PACKAGES[@]}"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
if [ ${#missing[@]} -eq 0 ]; then
  say "packages: all installed"
elif [ "$APT" = 1 ]; then
  say "installing packages: ${missing[*]}"
  run apt-get update
  run apt-get install -y "${missing[@]}"
else
  warn "missing packages (skipped, --no-apt): ${missing[*]}"
fi

# --- service account --------------------------------------------------------
for g in "${SVC_GROUPS[@]}"; do
  getent group "$g" >/dev/null || { say "creating group $g"; run groupadd --system "$g"; }
done
if ! id "$SVC_USER" >/dev/null 2>&1; then
  say "creating user $SVC_USER"
  run useradd --system --no-create-home --shell /usr/sbin/nologin \
      --groups "$(IFS=,; echo "${SVC_GROUPS[*]}")" "$SVC_USER"
  changed=1
else
  for g in "${SVC_GROUPS[@]}"; do
    id -nG "$SVC_USER" | tr ' ' '\n' | grep -qx "$g" || {
      say "adding $SVC_USER to group $g"; run usermod -aG "$g" "$SVC_USER"; changed=1; }
  done
fi

# --- kernel module for the display check (DDC/CI over HDMI) -----------------
if [ "$(cat /etc/modules-load.d/i2c.conf 2>/dev/null)" != i2c-dev ]; then
  say "loading i2c-dev at boot"
  if [ "$DRY" = 1 ]; then echo "    + echo i2c-dev > /etc/modules-load.d/i2c.conf"
  else echo i2c-dev > /etc/modules-load.d/i2c.conf; fi
fi
if ! lsmod | grep -q '^i2c_dev'; then
  say "loading i2c-dev"
  run modprobe i2c-dev || warn "could not load i2c-dev; the display check will not work until it is loaded"
fi

# --- configuration (never overwritten) --------------------------------------
run install -d -m 0755 "$CONF_DIR"
configured=1
if [ -e "$OLD_CONF" ] && [ ! -e "$CONF" ]; then
  say "migrating $OLD_CONF to $CONF"
  run mv "$OLD_CONF" "$CONF"; changed=1
  [ "$DRY" = 0 ] || CONF=$OLD_CONF   # dry run: inspect the file that would be moved
elif [ -e "$OLD_CONF" ]; then
  warn "$OLD_CONF is no longer used; $CONF is. Remove the old file once you are sure nothing needs it."
fi
if [ ! -e "$CONF" ]; then
  say "creating $CONF from the example"
  run install -m 0600 -o root -g root "$SRC/config/camwall.conf.example" "$CONF"
  configured=0
else
  if [ "$(stat -c '%a %U:%G' "$CONF")" != "600 root:root" ]; then
    say "fixing permissions of $CONF (expected 0600 root:root)"
    run chmod 0600 "$CONF"; run chown root:root "$CONF"
  fi
  if [ ! -r "$CONF" ]; then
    echo "    cannot read $CONF as $(id -un); skipping the placeholder check (run as root to include it)"
  elif grep -qE '=(replace-me|unifi\.example\.com)$' "$CONF"; then
    configured=0
  fi
fi

# --- build the plane wall ---------------------------------------------------
# Built outside the checkout so no root-owned files end up in it.
BUILD=$(mktemp -d); trap 'rm -rf "$BUILD"' EXIT
if [ "$DRY" = 1 ] && ! pkg-config --exists libdrm libavformat libavcodec libavutil 2>/dev/null; then
  echo "    + build camwall-planes (skipped in dry run: build packages not installed)"
  cp "$SRC/planes/camwall-planes.c" "$BUILD/camwall-planes"   # stand-in so the compare below runs
else
  say "building camwall-planes"
  # shellcheck disable=SC2046  # pkg-config output is a word list
  gcc -O2 -Wall -o "$BUILD/camwall-planes" "$SRC/planes/camwall-planes.c" \
      $(pkg-config --cflags --libs libdrm libavformat libavcodec libavutil) -lpthread
fi

# --- scripts, binary and unit -----------------------------------------------
install_file 0755 "$SRC/bin/camwalld" "$DAEMON"
install_file 0755 "$BUILD/camwall-planes" "$PLANES"
install_file 0755 "$SRC/bin/camwall" "$BIN"
install_file 0644 "$SRC/systemd/camwall.service" "$UNIT"
[ "$file_changed" = 0 ] || run systemctl daemon-reload

# --- enable and (re)start ---------------------------------------------------
if [ "$configured" = 0 ]; then
  warn "$CONF still has placeholder values. Edit it (sudoedit $CONF) and run this script again."
  warn "Not enabling or starting the service."
  exit 0
fi

run systemctl enable camwall >/dev/null 2>&1 || true
if [ "$DRY" = 1 ]; then
  say "dry run: would restart camwall if anything changed or it is not active"
elif [ "$RESTART" = 0 ]; then
  say "--no-restart: not restarting (changes apply at the next restart)"
elif [ "$changed" = 1 ] || ! systemctl is-active --quiet camwall; then
  say "restarting camwall"
  systemctl restart camwall
else
  say "nothing changed, service left running"
fi

if [ "$DRY" = 0 ]; then
  sleep 2
  say "status: $(systemctl is-active camwall) (NRestarts=$(systemctl show camwall -p NRestarts --value))"
  echo "    logs: journalctl -u camwall -n 30 --no-pager"
fi
