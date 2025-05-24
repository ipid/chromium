#!/bin/bash

# 为 macOS 调试签名 Chromium 二进制文件的脚本

BINARY_PATH="/Volumes/code/chromium/src/out/Default/blink_unittests"

# 创建一个临时的 entitlements 文件
cat > /tmp/debug.entitlements <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.debugger</key>
    <true/>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
    <key>com.apple.security.get-task-allow</key>
    <true/>
</dict>
</plist>
EOF

# 使用调试权限重新签名二进制文件
echo "正在为调试签名 $BINARY_PATH..."
codesign --force --deep --sign - --entitlements /tmp/debug.entitlements "$BINARY_PATH"

# 验证签名
echo "验证签名..."
codesign -dv "$BINARY_PATH" 2>&1 | grep -E "Signature|Authority|Entitlements"

# 清理临时文件
rm /tmp/debug.entitlements

echo "签名完成！" 