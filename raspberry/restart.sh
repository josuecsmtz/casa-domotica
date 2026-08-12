#!/usr/bin/env bash
set -e
sudo systemctl restart pyhome-hotspot.service
sudo systemctl restart casa-dashboard.service
echo "Servicios reiniciados."
