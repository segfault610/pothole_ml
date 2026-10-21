# 10. Commands and links

Copy-paste reference. Explanations are in the other docs.
Run everything from the repository root unless a `cd` says otherwise.

## Links

| What | Link |
|---|---|
| Repository | https://github.com/segfault610/pothole_ml |
| Live map | `MapUrl` output of the stack (command below) |
| GeoJSON feed | `GeoJsonUrl` output (add `?type=POTHOLE` or `?min_reports=2`) |
| AWS IoT Core console (Mumbai) | https://ap-south-1.console.aws.amazon.com/iot/home?region=ap-south-1 |
| IoT MQTT test client | https://ap-south-1.console.aws.amazon.com/iot/home?region=ap-south-1#/test |
| CloudFormation stack | https://ap-south-1.console.aws.amazon.com/cloudformation/home?region=ap-south-1#/stacks |
| Lambda functions | https://ap-south-1.console.aws.amazon.com/lambda/home?region=ap-south-1#/functions |
| DynamoDB tables | https://ap-south-1.console.aws.amazon.com/dynamodbv2/home?region=ap-south-1#tables |
| CloudWatch log groups | https://ap-south-1.console.aws.amazon.com/cloudwatch/home?region=ap-south-1#logsV2:log-groups |
| API Gateway | https://ap-south-1.console.aws.amazon.com/apigateway/main/apis?region=ap-south-1 |
| IAM users / access keys | https://console.aws.amazon.com/iam/home#/users |
| Billing / Free Tier usage | https://console.aws.amazon.com/billing/home#/freetier |
| Amazon Root CA 1 | https://www.amazontrust.com/repository/AmazonRootCA1.pem |
| ESP-IDF docs | https://docs.espressif.com/projects/esp-idf/en/latest/esp32/ |
| AWS SAM CLI docs | https://docs.aws.amazon.com/serverless-application-model/latest/developerguide/ |
| AWS IoT Core developer guide | https://docs.aws.amazon.com/iot/latest/developerguide/ |
| Leaflet | https://leafletjs.com/ |
| QGIS | https://qgis.org/ |
| kepler.gl | https://kepler.gl/demo |

## Print the live URLs

```bash
cloud/.venv/bin/aws cloudformation describe-stacks --stack-name pothole-map --region ap-south-1 \
  --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output table
```

## ESP-IDF environment (Ubuntu, IDF on the Fedora partition)

```bash
udisksctl mount -b /dev/nvme0n1p3
# once only:
F=/media/jonny/fedora/home/jonny
ln -s $F/esp-idf ~/esp-idf; ln -s $F/.espressif ~/.espressif; ln -s $F/miniconda3 ~/miniconda3
# every new terminal:
export PATH=$HOME/miniconda3/bin:$PATH
. ~/esp-idf/export.sh
```

## Firmware

```bash
cd esp32_iot
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor          # exit monitor: Ctrl+]
idf.py -p /dev/ttyUSB0 monitor
idf.py fullclean                              # after moving the folder / changing sdkconfig.defaults
```

Bench test and data recorder:

```bash
cd esp32_inference && idf.py -p /dev/ttyUSB0 flash monitor
cd esp32_collector && idf.py -p /dev/ttyUSB0 flash
```

## AWS CLI + SAM (local venv)

```bash
cd cloud && python3 -m venv .venv && .venv/bin/pip install awscli aws-sam-cli boto3 && cd ..
export PATH=$PWD/cloud/.venv/bin:$PATH
aws configure                                 # region ap-south-1, output json
aws sts get-caller-identity
```

## Register the device (certs + endpoint into secrets.h)

```bash
cp esp32_iot/main/secrets.example.h esp32_iot/main/secrets.h     # then set WIFI_SSID / WIFI_PASS
cloud/provision_device.sh ap-south-1 pothole-esp32
```

## Deploy / update / delete the backend

```bash
cd cloud
sam validate --lint
sam build
sam deploy --stack-name pothole-map --region ap-south-1 --resolve-s3 --capabilities CAPABILITY_IAM
sam delete --stack-name pothole-map --region ap-south-1
```

## Test and inspect the cloud

```bash
# publish a fake pothole (AWS CLI v1; on v2 add --cli-binary-format raw-in-base64-out)
ENDPOINT=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text --region ap-south-1)
aws iot-data publish --region ap-south-1 --endpoint-url https://$ENDPOINT --topic pothole/alerts \
  --payload '{"device_id":"cli-test","event":"POTHOLE","confidence":0.9,"severity_g":1.5,"lat":12.9716,"lon":79.1588,"gps":{"fix":true,"hdop":1.0}}'

# GeoJSON
curl -s "<GeoJsonUrl>"
curl -s "<GeoJsonUrl>?type=POTHOLE&min_reports=2"

# Lambda logs
aws logs tail /aws/lambda/$(aws cloudformation describe-stack-resource --stack-name pothole-map \
  --logical-resource-id IngestFunction --region ap-south-1 \
  --query StackResourceDetail.PhysicalResourceId --output text) --follow --region ap-south-1

# raw reports in DynamoDB
TABLE=$(aws cloudformation describe-stacks --stack-name pothole-map --region ap-south-1 \
  --query "Stacks[0].Outputs[?OutputKey=='TableName'].OutputValue" --output text)
aws dynamodb scan --region ap-south-1 --table-name $TABLE \
  --filter-expression "begins_with(PK,:p)" --expression-attribute-values '{":p":{"S":"REPORT#"}}'

# delete one map cell (e.g. a test point)
aws dynamodb delete-item --region ap-south-1 --table-name $TABLE \
  --key '{"PK":{"S":"CELL#<geohash>"},"SK":{"S":"TYPE#POTHOLE"}}'
```

## ML pipeline

```bash
pip install -r ml/requirements.txt
python ml/collect.py --label pothole --guided           # record (esp32_collector flashed)
python ml/collect.py --label normal --minutes 10
python ml/review.py data/raw/pothole/<session>.csv --plot   # inspect a recording
python ml/simulate.py --out data/synthetic --sessions 8 --segments 40
python ml/augment.py  --src data/bench --out data/bench_aug
python ml/train.py    --data data/synthetic data/bench_aug data/bench   # writes pothole_model.h + model/report.md
```

## Tests

```bash
python -m pytest ml/tests -q
python cloud/tests/test_potholes.py
```

## Git

```bash
git status
git add -A && git commit -m "message"
git push origin main
```
