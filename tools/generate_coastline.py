#!/usr/bin/env python3
"""
Generate coastline_data.h for ESP32 PROGMEM from Natural Earth GeoJSON.

Uses ne_10m_coastline for the UK/Ireland region (tight D-P tolerance) and
ne_50m_coastline for the rest of the world (coarse tolerance), writing a C++
header with flat float pairs (lat, lon) in PROGMEM. Segment separators are
999.0f, 999.0f.
"""

import sys
import json
import math
import urllib.request

# ── Configuration ────────────────────────────────────────────────────────────
EPSILON_GLOBAL  = 0.35      # D-P tolerance for world coastlines (~39 km)
EPSILON_EUROPE  = 0.08      # D-P tolerance for European coastlines (~9 km)
EPSILON_UK      = 0.018     # D-P tolerance for UK/Ireland (~2 km)
OUTPUT_PATH = r"C:\Dev\PIO\TouchController\include\coastline_data.h"
URL_50M = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector"
    "/master/geojson/ne_50m_coastline.geojson"
)
URL_10M = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector"
    "/master/geojson/ne_10m_coastline.geojson"
)

# Bounding boxes (lat_min, lat_max, lon_min, lon_max)
UK_BOX     = (49.0,  62.0, -11.0,  2.5)
EUROPE_BOX = (34.0,  72.0, -12.0,  45.0)
# ─────────────────────────────────────────────────────────────────────────────


def perpendicular_distance(px, py, ax, ay, bx, by):
    """Perpendicular distance from point P to line segment A-B."""
    dx, dy = bx - ax, by - ay
    if dx == 0 and dy == 0:
        return math.hypot(px - ax, py - ay)
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def rdp(points, epsilon):
    """Ramer-Douglas-Peucker polyline simplification (iterative, no recursion limit)."""
    if len(points) < 3:
        return list(points)

    stack = [(0, len(points) - 1)]
    keep = set([0, len(points) - 1])

    while stack:
        start, end = stack.pop()
        if end - start < 2:
            continue
        ax, ay = points[start]
        bx, by = points[end]
        dmax, idx = 0.0, start
        for i in range(start + 1, end):
            d = perpendicular_distance(points[i][0], points[i][1], ax, ay, bx, by)
            if d > dmax:
                dmax, idx = d, i
        if dmax > epsilon:
            keep.add(idx)
            stack.append((start, idx))
            stack.append((idx, end))

    return [points[i] for i in sorted(keep)]


def download_geojson(url):
    print(f"Downloading: {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    print(f"  {len(data['features'])} features downloaded.")
    return data


def extract_linestrings(geojson):
    """Yield lists of (lon, lat) from LineString / MultiLineString features."""
    for feat in geojson["features"]:
        geom = feat["geometry"]
        if geom["type"] == "LineString":
            yield geom["coordinates"]
        elif geom["type"] == "MultiLineString":
            for part in geom["coordinates"]:
                yield part


def segment_in_box(pts, box):
    """Return True if any point in pts falls inside the bounding box."""
    lat_min, lat_max, lon_min, lon_max = box
    return any(lat_min <= p[0] <= lat_max and lon_min <= p[1] <= lon_max for p in pts)


def epsilon_for(pts):
    """Choose D-P epsilon based on where the segment is located."""
    if segment_in_box(pts, UK_BOX):
        return EPSILON_UK
    if segment_in_box(pts, EUROPE_BOX):
        return EPSILON_EUROPE
    return EPSILON_GLOBAL


def main():
    # Download 10m data for high-detail regions
    try:
        data_10m = download_geojson(URL_10M)
    except Exception as exc:
        print(f"WARNING: 10m download failed ({exc}), falling back to 50m everywhere")
        data_10m = None

    # Download 50m data for global coverage
    try:
        data_50m = download_geojson(URL_50M)
    except Exception as exc:
        print(f"ERROR: 50m download failed — {exc}")
        sys.exit(1)

    total_in = 0
    total_out = 0
    segments = []

    # ── Pass 1: UK/Europe from 10m source (tight epsilon) ────────────────────
    if data_10m:
        for coords in extract_linestrings(data_10m):
            if len(coords) < 2:
                continue
            pts = [(c[1], c[0]) for c in coords]
            if not segment_in_box(pts, EUROPE_BOX):
                continue  # skip non-European segments in 10m pass
            total_in += len(pts)
            eps = epsilon_for(pts)
            simplified = rdp(pts, eps)
            if len(simplified) >= 2:
                segments.append(simplified)
                total_out += len(simplified)
        print(f"  10m UK/Europe segments: {len(segments)}, {total_out} pts")

    # ── Pass 2: Rest of world from 50m source (coarse epsilon) ───────────────
    uk_segs_before = len(segments)
    for coords in extract_linestrings(data_50m):
        if len(coords) < 2:
            continue
        pts = [(c[1], c[0]) for c in coords]
        # Skip only if the segment is ENTIRELY within Europe (all points in box).
        # Segments that merely touch Europe but extend further (e.g. full African
        # coast) must NOT be skipped — otherwise lower Africa disappears.
        if data_10m and all(
            (EUROPE_BOX[0] <= p[0] <= EUROPE_BOX[1] and EUROPE_BOX[2] <= p[1] <= EUROPE_BOX[3])
            for p in pts
        ):
            continue  # purely European — already covered by 10m pass
        total_in += len(pts)
        simplified = rdp(pts, EPSILON_GLOBAL)
        if len(simplified) >= 2:
            segments.append(simplified)
            total_out += len(simplified)

    world_segs = len(segments) - uk_segs_before
    print(f"  50m world segments: {world_segs}")
    print(f"  Total: {total_in} → {total_out} points  ({len(segments)} segments)")
    print(f"  Estimated PROGMEM: {total_out * 8 / 1024:.1f} KB")

    with open(OUTPUT_PATH, "w", newline="\n") as f:
        f.write("// Auto-generated by tools/generate_coastline.py\n")
        f.write("// UK/Ireland: Natural Earth 10m, D-P tol = {:.3f} deg (~{:.0f} km)\n".format(
            EPSILON_UK, EPSILON_UK * 111))
        f.write("// Europe:     Natural Earth 10m, D-P tol = {:.2f} deg (~{:.0f} km)\n".format(
            EPSILON_EUROPE, EPSILON_EUROPE * 111))
        f.write("// World:      Natural Earth 50m, D-P tol = {:.2f} deg (~{:.0f} km)\n".format(
            EPSILON_GLOBAL, EPSILON_GLOBAL * 111))
        f.write(f"// {len(segments)} segments, {total_out} coordinate pairs\n")
        f.write("// Format: flat float pairs (lat, lon); 999.0f,999.0f = segment end.\n")
        f.write("\n")
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n")
        f.write("\n")
        f.write("static const float COASTLINE_DATA[] PROGMEM = {\n")

        for seg in segments:
            items = list(seg) + [(999.0, 999.0)]
            line_buf = []
            for lat, lon in items:
                line_buf.append(f"{lat:.3f}f,{lon:.3f}f")
                if len(line_buf) == 4:
                    f.write("    " + ", ".join(line_buf) + ",\n")
                    line_buf = []
            if line_buf:
                f.write("    " + ", ".join(line_buf) + ",\n")

        f.write("};\n")
        f.write("\n")
        f.write("static const int COASTLINE_POINTS = sizeof(COASTLINE_DATA) / sizeof(float) / 2;\n")

    print(f"Written → {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
