"""
Offline tests of the Lambda logic with an in-memory stand-in for DynamoDB.

    python cloud/tests/test_potholes.py
"""

import json
import os
import re
import sys
from decimal import Decimal

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import potholes  # noqa: E402


class FakeTable:
    """Implements just the UpdateExpression subset potholes.py uses."""

    def __init__(self):
        self.items = {}

    def put_item(self, Item):
        self.items[(Item["PK"], Item["SK"])] = dict(Item)

    def update_item(self, Key, UpdateExpression, ExpressionAttributeValues, ConditionExpression=None):
        k = (Key["PK"], Key["SK"])
        item = self.items.setdefault(k, dict(Key))
        v = ExpressionAttributeValues

        if ConditionExpression:
            if "max_severity" in item and not item["max_severity"] < v[":s"]:
                from botocore.exceptions import ClientError
                raise ClientError({"Error": {"Code": "ConditionalCheckFailedException"}}, "UpdateItem")
            item["max_severity"] = v[":s"]
            return

        add, _, set_part = UpdateExpression.partition(" SET ")
        for clause in add.replace("ADD ", "").split(","):
            name, ref = clause.split()
            val = v[ref]
            if isinstance(val, set):
                item[name] = item.get(name, set()) | val
            else:
                item[name] = item.get(name, 0) + val
        for clause in re.split(r",(?![^(]*\))", set_part):
            name, expr = [s.strip() for s in clause.split("=", 1)]
            if expr.startswith("if_not_exists"):
                item.setdefault(name, v[expr.split(",")[1].strip(" )")])  # if_not_exists(x, :v)
            else:
                item[name] = v[expr]

    def scan(self, **kwargs):
        return {"Items": [i for (pk, _), i in self.items.items() if pk.startswith("CELL#")]}


def alert(lat, lon, event="POTHOLE", device="pothole-aaaaaa", sev=1.2, hdop=1.0, t=1000):
    return {
        "device_id": device, "event": event, "confidence": 0.9, "severity_g": sev,
        "coordinates": [lat, lon], "lat": lat, "lon": lon,
        "gps": {"fix": True, "sats": 8, "hdop": hdop, "speed_kmh": 30.0, "age_ms": 100},
        "received_ms": t,
    }


def run():
    potholes._table = FakeTable()

    assert potholes.geohash(57.64911, 10.40744, 11) == "u4pruydqqvj"

    # same pothole seen 3x by two cars, a few metres apart
    potholes.ingest_handler(alert(12.971600, 79.158800, t=1000))
    potholes.ingest_handler(alert(12.971630, 79.158820, device="pothole-bbbbbb", sev=2.0, t=2000))
    potholes.ingest_handler(alert(12.971610, 79.158790, sev=0.8, t=3000))
    # a speed hump 200 m away
    potholes.ingest_handler(alert(12.973400, 79.158800, event="SPEED_HUMP", t=4000))
    # rejected: no fix, bad hdop, normal events
    assert not potholes.ingest_handler({**alert(0, 0), "lat": None, "lon": None, "coordinates": None})["mapped"]
    assert not potholes.ingest_handler(alert(12.97, 79.15, hdop=9))["mapped"]
    assert not potholes.ingest_handler(alert(12.97, 79.15, event="NORMAL"))["mapped"]
    # v1 firmware payload (only "coordinates") is still accepted
    assert potholes.ingest_handler({"event": "POTHOLE", "coordinates": [12.98, 79.16]})["mapped"]

    res = potholes.api_handler({"rawPath": "/potholes", "queryStringParameters": {"min_reports": "2"}})
    fc = json.loads(res["body"])
    assert res["statusCode"] == 200
    assert len(fc["features"]) == 1, fc
    p = fc["features"][0]["properties"]
    assert p["reports"] == 3 and p["devices"] == 2 and p["max_severity_g"] == 2.0
    lon, lat = fc["features"][0]["geometry"]["coordinates"]
    assert abs(lat - 12.97161) < 1e-4 and abs(lon - 79.1588) < 1e-4

    res = potholes.api_handler({"rawPath": "/potholes", "queryStringParameters": {"type": "SPEED_HUMP"}})
    assert len(json.loads(res["body"])["features"]) == 1

    res = potholes.api_handler({"rawPath": "/potholes", "queryStringParameters": {"type": "bogus"}})
    assert res["statusCode"] == 400

    res = potholes.api_handler({"rawPath": "/"})
    assert res["statusCode"] == 200 and "leaflet" in res["body"]

    assert isinstance(potholes._table.items[("CELL#" + potholes.geohash(12.9716, 79.1588), "TYPE#POTHOLE")]["lat_sum"], Decimal)
    print("cloud tests: all passed")


if __name__ == "__main__":
    run()
