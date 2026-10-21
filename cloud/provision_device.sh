#!/usr/bin/env bash
# Register the ESP32 with *your* AWS IoT Core account:
#   thing + certificate + policy -> esp32_iot/certs/, endpoint -> secrets.h
#
# Usage: cloud/provision_device.sh [region] [thing-name]
# Needs: aws CLI configured (aws configure), curl.
# Re-running reuses existing certs in esp32_iot/certs/ and only refreshes the endpoint.

set -euo pipefail

REGION="${1:-ap-south-1}"
THING="${2:-pothole-esp32}"
POLICY="PotholeDevicePolicy"
TOPIC="pothole/alerts"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CERTS="$ROOT/esp32_iot/certs"
SECRETS="$ROOT/esp32_iot/main/secrets.h"
AWS="aws --region $REGION"

ACCOUNT=$(aws sts get-caller-identity --query Account --output text)
echo "AWS account $ACCOUNT, region $REGION, thing $THING"

mkdir -p "$CERTS"

# --- thing --------------------------------------------------------------
$AWS iot describe-thing --thing-name "$THING" >/dev/null 2>&1 \
    || $AWS iot create-thing --thing-name "$THING" >/dev/null

# --- policy -------------------------------------------------------------
if ! $AWS iot get-policy --policy-name "$POLICY" >/dev/null 2>&1; then
    $AWS iot create-policy --policy-name "$POLICY" --policy-document "{
      \"Version\": \"2012-10-17\",
      \"Statement\": [
        {\"Effect\": \"Allow\", \"Action\": \"iot:Connect\",
         \"Resource\": \"arn:aws:iot:$REGION:$ACCOUNT:client/*\"},
        {\"Effect\": \"Allow\", \"Action\": \"iot:Publish\",
         \"Resource\": \"arn:aws:iot:$REGION:$ACCOUNT:topic/$TOPIC\"}
      ]}" >/dev/null
fi

# --- certificate --------------------------------------------------------
if [[ -f "$CERTS/certificate.pem.crt" && -f "$CERTS/private.pem.key" ]]; then
    echo "certs already in $CERTS - keeping them"
else
    CERT_ARN=$($AWS iot create-keys-and-certificate --set-as-active \
        --certificate-pem-outfile "$CERTS/certificate.pem.crt" \
        --private-key-outfile "$CERTS/private.pem.key" \
        --query certificateArn --output text)
    chmod 600 "$CERTS/private.pem.key"
    $AWS iot attach-policy --policy-name "$POLICY" --target "$CERT_ARN"
    $AWS iot attach-thing-principal --thing-name "$THING" --principal "$CERT_ARN"
    echo "created certificate $CERT_ARN"
fi

[[ -f "$CERTS/aws_root_ca.pem" ]] \
    || curl -fsSL https://www.amazontrust.com/repository/AmazonRootCA1.pem -o "$CERTS/aws_root_ca.pem"

# --- endpoint -> secrets.h ----------------------------------------------
ENDPOINT=$($AWS iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text)

if [[ ! -f "$SECRETS" ]]; then
    cp "$ROOT/esp32_iot/main/secrets.example.h" "$SECRETS"
    echo "created $SECRETS - fill in WIFI_SSID / WIFI_PASS"
fi
sed -i "s|^#define AWS_IOT_ENDPOINT .*|#define AWS_IOT_ENDPOINT    \"$ENDPOINT\"|" "$SECRETS"

echo "endpoint $ENDPOINT written to $SECRETS"
echo "next: build + flash esp32_iot"
