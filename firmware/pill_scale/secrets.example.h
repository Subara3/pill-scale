// このファイルを secrets.h という名前でコピーして、家のWi-Fiの情報を書き込む。
// secrets.h は .gitignore で除外しているので、GitHub には上がらない。
#pragma once

// 家のWi-Fi（2.4GHz）。ESP32-S3 は 5GHz につながらない
#define WIFI_SSID     "ここにSSID"
#define WIFI_PASSWORD "ここにパスワード"

// 家のWi-Fiにつながらなかったとき、本体が自分で出すWi-Fiのパスワード（8文字以上）
#define SETUP_AP_PASSWORD "pillscale-setup"
