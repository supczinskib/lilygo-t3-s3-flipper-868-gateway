# Changelog

All notable changes to this project are documented in this file.

## 1.0.2

- Added automatic Wi-Fi reconnect attempts every 15 seconds.
- Added a Wi-Fi interface, web server, and mDNS restart after 60 seconds without connectivity.
- Added an ESP32-S3 restart after five minutes of uninterrupted Wi-Fi loss.
- Kept the setup access point active when the saved network is unavailable at startup while continuing background reconnect attempts.
- Normalized compiler paths in release binaries so local build-directory names are not embedded in published images.
