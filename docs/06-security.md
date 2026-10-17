# 6. Security

## ⚠ Action needed: the Wi-Fi password is public

The GitHub repository is **public**, and the first commit (`bb1dbd0`) had
the Wi-Fi SSID and password hard-coded in `esp32_iot/main/main.c`. It has
been moved to the git-ignored `esp32_iot/main/secrets.h`, but **it is still
in the git history**. Treat it as leaked:

1. Change the password of that hotspot/network.
2. Put the new one in `esp32_iot/main/secrets.h` (never in a tracked file).

The AWS certificate and private key were **not** committed (they were
already git-ignored). Keep it that way — the root `.gitignore` ignores
`**/certs/*.pem|*.crt|*.key`.

### Optional: remove it from history

Changing the password is what actually protects you. If you also want it out
of the history, rewrite it and force-push (this changes every commit hash;
anyone with a clone must re-clone):

```bash
pip install git-filter-repo
echo '<old-password>==>REMOVED' > /tmp/replacements.txt      # put the real old password here
git filter-repo --replace-text /tmp/replacements.txt
git remote add origin git@github.com:segfault610/pothole_ml.git
git push --force origin main
```

## AWS IoT certificate hygiene

- One certificate **per device**; a leaked key can then be revoked without
  touching the others.
- To rotate: IoT Core → *Security → Certificates* → *Create certificate*
  (auto-generate) → download the device certificate, private key and Amazon
  Root CA 1 → *Activate* → attach your policy and thing → copy the files to
  `esp32_iot/certs/` as `certificate.pem.crt`, `private.pem.key`,
  `aws_root_ca.pem` → rebuild/flash → **deactivate and delete the old certificate**.
- Least-privilege policy (replace region/account; the client ID must match —
  esp-mqtt's default is `ESP32_<last 3 MAC bytes in hex>`, visible in the IoT
  console's connection logs, or set `.credentials.client_id` in `aws_iot_init()`):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": "iot:Connect",
      "Resource": "arn:aws:iot:ap-south-1:<ACCOUNT_ID>:client/ESP32_*" },
    { "Effect": "Allow", "Action": "iot:Publish",
      "Resource": "arn:aws:iot:ap-south-1:<ACCOUNT_ID>:topic/pothole/alerts" }
  ]
}
```

## Cloud

- The map API is public and **read-only**. It exposes pothole locations
  and timestamps, not device keys. If the device IDs or timing are
  sensitive, put the API behind a Cognito/JWT authorizer.
- The ingest Lambda only accepts messages coming through the IoT rule — it
  has no public endpoint.
- Never commit `cloud/samconfig.toml` if you add secrets to it (git-ignored).
