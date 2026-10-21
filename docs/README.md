# Documentation

| # | Guide | Read when |
|---|---|---|
| 1 | [Hardware](01-hardware.md) | wiring the ESP32, ADXL345, NEO-7M; mounting; bring-up checks |
| 2 | [Firmware](02-firmware.md) | building/flashing the three ESP-IDF projects, credentials, MQTT payload |
| 3 | [Data collection](03-data-collection.md) | recording labelled drives with `ml/collect.py` |
| 4 | [ML pipeline](04-ml-pipeline.md) | how detection/features/training work, v1 problems, Q-learning & Pico answers |
| 5 | [Cloud & GIS](05-cloud-gis.md) | deploying the AWS backend and the live pothole map |
| 6 | [Security](06-security.md) | leaked Wi-Fi password, certificate rotation, IoT policy |
| 7 | [Status](07-status.md) | what works today, the deployed model, problems hit and fixed, next steps |
| 8 | [AWS deployment](08-aws-deployment.md) | services used, exact deploy commands, verification, problems and fixes |
| 9 | [Project report](09-project-report.md) | the whole project in one document: architecture, hardware, firmware, ML, cloud, tools, results |
| 10 | [Commands & links](10-commands-and-links.md) | copy-paste shell commands and web links only |
| — | [Presentation](Pothole_Detection_System.pptx) | 13-slide summary deck |
| — | [Capstone report](report/Capstone_Report.pdf) ([.docx](report/Capstone_Report.docx)) | formal project report: certificate, abstract, 7 chapters, references, appendices |
| — | [IEEE paper](report/IEEE_Paper.pdf) ([.docx](report/IEEE_Paper.docx), [LaTeX](report/ieee_latex/)) | conference-format paper (IEEEtran) |
| — | [figures/](figures/) | all figures used in the reports and slides |

Typical order for a new setup: 1 → 2 (flash `esp32_inference`, check wiring)
→ 3 (collect with `esp32_collector`) → 4 (train) → 2 (flash `esp32_iot`) → 5 / 8.
