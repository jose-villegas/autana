"""Optional notifications for a human reservation."""

import base64
import json
import os
import subprocess


TOAST_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$payload = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('PAYLOAD')) | ConvertFrom-Json
$title = [Security.SecurityElement]::Escape("Autana $($payload.port) - $($payload.owner)")
$body = [Security.SecurityElement]::Escape($payload.note)
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null
[Windows.UI.Notifications.ToastNotification, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null
[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] > $null
$xml = New-Object Windows.Data.Xml.Dom.XmlDocument
$xml.LoadXml("<toast><visual><binding template='ToastGeneric'><text>$title</text><text>$body</text></binding></visual></toast>")
$toast = [Windows.UI.Notifications.ToastNotification]::new($xml)
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('Microsoft.Windows.PowerShell').Show($toast)
"""


def windows_toast(port, owner, note):
    payload = base64.b64encode(json.dumps({"port": port, "owner": owner, "note": note}).encode())
    script = base64.b64encode(TOAST_SCRIPT.replace("PAYLOAD", payload.decode()).encode("utf-16le"))
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", script.decode()],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10,
        creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    if result.returncode:
        raise RuntimeError(result.stderr.strip().splitlines()[-1] if result.stderr.strip()
                           else f"PowerShell exited {result.returncode}")


SINKS = (windows_toast,)


def notify_human(port, owner, note):
    if os.name != "nt" or os.environ.get("AUTANA_NOTIFY") == "0":
        return False
    for sink in SINKS:
        sink(port, owner, note)
    return True
