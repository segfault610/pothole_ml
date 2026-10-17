"""
Pothole GIS backend (AWS Lambda, Python 3.12).

ingest_handler   invoked by the IoT Core rule on topic pothole/alerts.
                 Stores the raw report and folds it into a map "cell"
                 (geohash-8, ~38 m x 19 m) so many reports of the same
                 pothole become one marker with a report count.

api_handler      HTTP API:
                   GET /            Leaflet map (map.html)
                   GET /potholes    GeoJSON FeatureCollection
                                    ?type=POTHOLE|SPEED_HUMP  ?min_reports=2

DynamoDB single table (TABLE_NAME):
  PK="CELL#<geohash8>"     SK="TYPE#<event>"   aggregated map marker
  PK="REPORT#<device_id>"  SK="<received_ms>"  raw report (expires via ttl)
"""

import json
import math
import os
import time
from decimal import Decimal

import boto3
from botocore.exceptions import ClientError

TABLE_NAME = os.environ.get("TABLE_NAME", "PotholeMap")
MAX_HDOP = float(os.environ.get("MAX_HDOP", "5"))
REPORT_TTL_DAYS = int(os.environ.get("REPORT_TTL_DAYS", "90"))
MERGE_RADIUS_M = float(os.environ.get("MERGE_RADIUS_M", "15"))
EVENTS = ("POTHOLE", "SPEED_HUMP")

_table = None


def table():
    global _table
    if _table is None:
        _table = boto3.resource("dynamodb").Table(TABLE_NAME)
    return _table


# ============================================================
# GEO HELPERS
# ============================================================

_BASE32 = "0123456789bcdefghjkmnpqrstuvwxyz"


def geohash(lat, lon, precision=8):
    lat_rng, lon_rng = [-90.0, 90.0], [-180.0, 180.0]
    bits, bit, ch, even, out = 0, 0, 0, True, []

    while len(out) < precision:
        rng, val = (lon_rng, lon) if even else (lat_rng, lat)
        mid = (rng[0] + rng[1]) / 2
        if val >= mid:
            ch = (ch << 1) | 1
            rng[0] = mid
        else:
            ch <<= 1
            rng[1] = mid
        even = not even
        bit += 1
        if bit == 5:
            out.append(_BASE32[ch])
            bit, ch = 0, 0

    return "".join(out)


def distance_m(lat1, lon1, lat2, lon2):
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def _dec(x):
    return Decimal(str(round(float(x), 7)))


# ============================================================
# INGEST (IoT rule -> Lambda)
# ============================================================

def validate(msg):
    """Return (ok, reason)."""
    if msg.get("event") not in EVENTS:
        return False, f"event {msg.get('event')!r} not mapped"

    lat, lon = msg.get("lat"), msg.get("lon")
    if (lat is None or lon is None) and isinstance(msg.get("coordinates"), list):
        lat, lon = msg["coordinates"][:2]       # v1 firmware payload
        msg["lat"], msg["lon"] = lat, lon

    if lat is None or lon is None:
        return False, "no GPS fix"
    if not (-90 <= lat <= 90 and -180 <= lon <= 180) or (lat == 0 and lon == 0):
        return False, "invalid coordinates"

    gps = msg.get("gps") or {}
    if gps.get("hdop") and gps["hdop"] > MAX_HDOP:
        return False, f"poor GPS (hdop {gps['hdop']})"

    return True, ""


def ingest_handler(event, context=None):
    msg = dict(event)
    now_ms = int(msg.get("received_ms") or time.time() * 1000)
    device = str(msg.get("device_id") or msg.get("clientid") or "unknown")

    ok, reason = validate(msg)

    report = {
        "PK": f"REPORT#{device}",
        "SK": str(now_ms),
        "payload": json.dumps(event),
        "mapped": ok,
        "reason": reason,
        "ttl": int(now_ms / 1000) + REPORT_TTL_DAYS * 86400,
    }
    table().put_item(Item=report)

    if not ok:
        print(json.dumps({"skipped": reason, "device": device}))
        return {"mapped": False, "reason": reason}

    lat, lon = float(msg["lat"]), float(msg["lon"])
    cell = geohash(lat, lon, 8)
    key = {"PK": f"CELL#{cell}", "SK": f"TYPE#{msg['event']}"}
    severity = float(msg.get("severity_g") or 0)
    confidence = float(msg.get("confidence") or 1)

    table().update_item(
        Key=key,
        UpdateExpression=(
            "ADD n :one, lat_sum :lat, lon_sum :lon, conf_sum :conf, devices :dev "
            "SET event_type = :ev, cell = :cell, last_seen = :t, "
            "first_seen = if_not_exists(first_seen, :t)"
        ),
        ExpressionAttributeValues={
            ":one": 1, ":lat": _dec(lat), ":lon": _dec(lon), ":conf": _dec(confidence),
            ":dev": {device}, ":ev": msg["event"], ":cell": cell, ":t": now_ms,
        },
    )

    try:
        table().update_item(
            Key=key,
            UpdateExpression="SET max_severity = :s",
            ConditionExpression="attribute_not_exists(max_severity) OR max_severity < :s",
            ExpressionAttributeValues={":s": _dec(severity)},
        )
    except ClientError as e:
        if e.response["Error"]["Code"] != "ConditionalCheckFailedException":
            raise

    print(json.dumps({"mapped": msg["event"], "cell": cell, "device": device}))
    return {"mapped": True, "cell": cell}


# ============================================================
# API (HTTP API -> Lambda)
# ============================================================

def load_cells():
    items, kwargs = [], {
        "FilterExpression": "begins_with(PK, :p)",
        "ExpressionAttributeValues": {":p": "CELL#"},
    }
    while True:
        page = table().scan(**kwargs)
        items += page.get("Items", [])
        if "LastEvaluatedKey" not in page:
            return items
        kwargs["ExclusiveStartKey"] = page["LastEvaluatedKey"]


def build_features(cells, event_filter=None, min_reports=1):
    """Merge neighbouring cells (geohash edges) and return GeoJSON features."""
    points = []
    for c in cells:
        n = int(c["n"])
        if event_filter and c["event_type"] != event_filter:
            continue
        points.append({
            "event": c["event_type"], "n": n,
            "lat": float(c["lat_sum"]) / n, "lon": float(c["lon_sum"]) / n,
            "confidence": float(c.get("conf_sum", n)) / n,
            "max_severity": float(c.get("max_severity", 0)),
            "devices": set(c.get("devices", [])),
            "first_seen": int(c["first_seen"]), "last_seen": int(c["last_seen"]),
        })

    points.sort(key=lambda p: -p["n"])
    merged = []
    for p in points:
        for m in merged:
            if m["event"] == p["event"] and distance_m(m["lat"], m["lon"], p["lat"], p["lon"]) <= MERGE_RADIUS_M:
                total = m["n"] + p["n"]
                m["lat"] = (m["lat"] * m["n"] + p["lat"] * p["n"]) / total
                m["lon"] = (m["lon"] * m["n"] + p["lon"] * p["n"]) / total
                m["confidence"] = (m["confidence"] * m["n"] + p["confidence"] * p["n"]) / total
                m["n"] = total
                m["max_severity"] = max(m["max_severity"], p["max_severity"])
                m["devices"] |= p["devices"]
                m["first_seen"] = min(m["first_seen"], p["first_seen"])
                m["last_seen"] = max(m["last_seen"], p["last_seen"])
                break
        else:
            merged.append(dict(p))

    return [
        {
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": [round(m["lon"], 7), round(m["lat"], 7)]},
            "properties": {
                "event": m["event"], "reports": m["n"], "devices": len(m["devices"]),
                "confidence": round(m["confidence"], 2), "max_severity_g": round(m["max_severity"], 2),
                "first_seen": m["first_seen"], "last_seen": m["last_seen"],
            },
        }
        for m in merged if m["n"] >= min_reports
    ]


def _response(status, body, content_type="application/json"):
    return {
        "statusCode": status,
        "headers": {"Content-Type": content_type, "Access-Control-Allow-Origin": "*"},
        "body": body,
    }


def api_handler(event, context=None):
    path = event.get("rawPath") or event.get("path") or "/"
    params = event.get("queryStringParameters") or {}

    if path in ("/", "/map"):
        with open(os.path.join(os.path.dirname(__file__), "map.html")) as f:
            return _response(200, f.read(), "text/html; charset=utf-8")

    if path == "/potholes":
        event_filter = params.get("type")
        if event_filter and event_filter not in EVENTS:
            return _response(400, json.dumps({"error": f"type must be one of {EVENTS}"}))
        try:
            min_reports = max(int(params.get("min_reports", 1)), 1)
        except ValueError:
            return _response(400, json.dumps({"error": "min_reports must be an integer"}))

        features = build_features(load_cells(), event_filter, min_reports)
        return _response(200, json.dumps({"type": "FeatureCollection", "features": features}))

    return _response(404, json.dumps({"error": "not found"}))
