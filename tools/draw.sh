#!/usr/bin/env bash
#
# Render the keymap to draw/keyball39.svg (per-layer) and draw/overview.svg
# (Base with Nav/Fn/Num/Sys as corner overlays, plus the Combos block).
#
#   tools/draw.sh
#
# keymap-drawer and python-yq are run with `uvx`, so they are resolved into
# uv's cache on first use and nothing is installed system-wide. Pin a different
# keymap-drawer with KEYMAP_DRAWER_VERSION=x.y.z.
#
# BOTH SVGs are committed and used in the README. Re-run this after ANY change
# that affects the rendered layout: key bindings, layer add/remove, moving an
# activator (&mo/&lt/&mt), matrix or position-label changes, combos.

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"

KD_VERSION="${KEYMAP_DRAWER_VERSION:-0.23.0}"

command -v uv >/dev/null 2>&1 || {
    echo "error: uv not found on PATH (needed to run keymap-drawer)." >&2
    exit 1
}

kd()  { uvx --quiet --from "keymap-drawer==$KD_VERSION" keymap "$@"; }
yq_() { uvx --quiet --from yq yq "$@"; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---------------------------------------------------------------------------
# 1. Parse the keymap into an intermediate YAML.
# ---------------------------------------------------------------------------
echo "--- parsing config/keyball39.keymap"
kd -c draw/config.yaml parse -z config/keyball39.keymap \
    --virtual-layers Combos >draw/keyball39.yaml

# ---------------------------------------------------------------------------
# 2. Fix up what keymap-drawer cannot infer from the source.
#
# It derives each layer's "held" activator marker from ZMK_CONDITIONAL_LAYER
# macros and certain hold-taps. It does NOT follow plain `&mo TARGET` on Base,
# and it does not see DT-level `/delete-node/` overrides. Four things come out
# wrong -- the firmware is correct in every case, only the diagram is not:
#
#   1. Num's held marker lands on position 36 (Magic Shift) instead of 37
#      (Smart Num).
#   2. Sys keeps stale held markers on 35/37 from base.keymap's
#      ZMK_CONDITIONAL_LAYER(sys, FN NUM, SYS), which we /delete-node/ away and
#      replace with `&mo SYS` at position 30.
#   3. TOTP gets no held marker at all for `&mo TOTP` at position 31.
#   4. `&mo TOTP` on Base renders as "Base": `#define TOTP 6` expands to
#      `&mo 6` before the matching `#undef`, so the layer name is unresolvable
#      and it falls back to layer 0. Forcing the string "TOTP" also restores
#      the <a href="#TOTP"> hyperlink and layer-activator class for free.
#
# The explicit layer dict also fixes the render order (Base, Plain, Combos, Num,
# Fn, TOTP, Sys, Nav, Mouse).
#
# If you move &mo SYS / &mo TOTP, re-swap the right thumbs, change the matrix
# or rename the TOTP layer, update these indices to match.
# ---------------------------------------------------------------------------
echo "--- patching held markers + layer order"
cat >"$tmp/patch.jq" <<'JQ'
. = {
  "layout": .layout,
  "combos": .combos,
  "layers": {
    "Base":   .layers.Base,
    "Plain":  .layers.Plain,
    "Combos": .layers.Combos,
    "Num":    .layers.Num,
    "Fn":     .layers.Fn,
    "TOTP":   .layers.TOTP,
    "Sys":    .layers.Sys,
    "Nav":    .layers.Nav,
    "Mouse":  .layers.Mouse
  }
}
| .combos[].l    = ["Combos"]
| .layers.Num[36]  = {type: "trans"}
| .layers.Num[37]  = {type: "held"}
| .layers.Sys[30]  = {type: "held"}
| .layers.Sys[35]  = {type: "trans"}
| .layers.Sys[36]  = {type: "trans"}
| .layers.Sys[37]  = {type: "trans"}
| .layers.TOTP[31] = {type: "held"}
| .layers.Base[31] = "TOTP"
JQ

# Fail loudly if a layer went missing (renamed layer, failed parse) rather than
# silently emitting an SVG full of nulls.
for layer in Base Plain Combos Num Fn TOTP Sys Nav Mouse; do
    yq_ -e ".layers.$layer | length > 0" draw/keyball39.yaml >/dev/null 2>&1 || {
        echo "error: layer '$layer' missing or empty in the parsed keymap." >&2
        echo "       did a layer get renamed? update tools/draw.sh." >&2
        exit 1
    }
done

yq_ -Y -i --from-file "$tmp/patch.jq" draw/keyball39.yaml

# ---------------------------------------------------------------------------
# 3. Per-layer SVG.
# ---------------------------------------------------------------------------
echo "--- drawing draw/keyball39.svg"
kd -c draw/config.yaml draw draw/keyball39.yaml \
    -j config/keyball39.json >draw/keyball39.svg

# ---------------------------------------------------------------------------
# 4. Overview: collapse Nav/Fn/Num/Sys onto Base as corner labels.
# ---------------------------------------------------------------------------
echo "--- building draw/overview.yaml"
cat >"$tmp/overview.jq" <<'JQ'
def extract_label: if type == "string" then . else .t end;
def is_transparent: type == "object" and (.type == "trans" or .type == "held");
.layers = {
  Base: [
    [.layers.Base, .layers.Nav, .layers.Fn, .layers.Num, .layers.Sys] | transpose[] |
    (.[0] | if type == "string" then {t: .} else . end) as $base |
    (.[1] | if is_transparent then null else extract_label end) as $nav |
    (.[2] | if is_transparent then null else extract_label end) as $fn  |
    (.[3] | if is_transparent then null else extract_label end) as $num |
    (.[4] | if is_transparent then null else extract_label end) as $sys |
    $base
    + (if $nav == null then {} else {tr: $nav} end)
    + (if $fn  == null then {} else {tl: $fn}  end)
    + (if $num == null then {} else {bl: $num} end)
    + (if $sys == null then {} else {br: $sys} end)
  ],
  Combos: .layers.Combos
}
| .combos = [.combos[] | .l = ["Combos"]]
JQ

yq_ -y --from-file "$tmp/overview.jq" draw/keyball39.yaml >draw/overview.yaml

# ---------------------------------------------------------------------------
# 5. Overview-only style overlay.
#
# Split-shade palette: the activator buttons on Base are filled with the DARK
# shade of each layer's hue (so off-white tap/hold text reads on top), while
# the corner overlay text uses the BRIGHT shade from --color-{nav,fn,num,sys}
# in draw/config.yaml. Retune both in lockstep to keep the hue identity.
#
#   pos 30 LH5  Sys    fuchsia-600   pos 35 LH0  Fn    lime-700
#   pos 31 LH4  TOTP   rose-600      pos 37 RH1  Num   orange-600
#   pos 32 LH3  Plain  indigo-600    pos 34 LH1  Nav   cyan-600
#
# Plain (the &tog HRM-off toggle) gets indigo — the widest free gap in the
# existing palette, which clusters around cyan/lime and fuchsia/rose.
# ---------------------------------------------------------------------------
echo "--- drawing draw/overview.svg"
cat >"$tmp/style.jq" <<'JQ'
.draw_config.n_columns = 1
| .draw_config.svg_extra_style += "
.layer-Base .keypos-30 rect.key { fill: #c026d3; }
.layer-Base .keypos-31 rect.key { fill: #e11d48; }
.layer-Base .keypos-32 rect.key { fill: #4f46e5; }
.layer-Base .keypos-34 rect.key { fill: #0891b2; }
.layer-Base .keypos-35 rect.key { fill: #4d7c0f; }
.layer-Base .keypos-37 rect.key { fill: #ea580c; }
text.tl, text.tr, text.bl, text.br { font-weight: bold; }"
JQ

yq_ -Y --from-file "$tmp/style.jq" draw/config.yaml >"$tmp/config-overview.yaml"

kd -c "$tmp/config-overview.yaml" draw draw/overview.yaml \
    -j config/keyball39.json >draw/overview.svg

# Strip the "Base" / "Combos" captions above each block for a clean overview.
sed -i '/<text[^>]*class="label"/d' draw/overview.svg

echo
echo "wrote:"
for f in draw/keyball39.svg draw/overview.svg; do
    printf '  %-24s %s\n' "$f" "$(du -h "$f" | cut -f1)"
done
