#!/usr/bin/env bash
# run-webui.sh —— CGI WebUI 本地起服（discuss 17 脚手架）
#
# 说明：
#   · wfr_cgi 编译产物在 build/dev/examples/wfr_cgi（CMake examples target）。
#   · web/ 下需要一个 cgi-bin/ 可执行映射：CGI 目录里放 wfr_cgi。
#     这里用 symlink。若改用 Apache/nginx 的 CGI 配置，把 ScriptAlias 指到 cgi-bin/ 即可。
#   · 起服：python3 -m http.server --cgi --directory web <port>（python 的 http.server 支持 --cgi）。
#   · wfr 二进制路径（CGI 里 run_argv 用）：默认 build/dev/bin/wfr（改源码/内置 workflow 后要重编）。
#     可用 WFR_BIN 覆盖。
#
# 用法：
#   ./run-webui.sh            # 起在 8000
#   ./run-webui.sh 9000       # 指定端口
set -euo pipefail
cd "$(dirname "$0")"

PORT="${1:-8000}"
BIN="${WFR_BIN:-$PWD/build/dev/bin/wfr}"
mkdir -p web/cgi-bin
# wfr_cgi 产物若已构建则软链；未构建则先编
if [[ ! -x build/dev/bin/wfr_cgi ]]; then
  source ~/opt/gcc16/env.sh
  cmake --build --preset dev --target wfr_cgi wfr
fi
ln -sf ../../build/dev/bin/wfr_cgi web/cgi-bin/wfr_cgi
ln -sf ../../build/dev/bin/wfr web/wfr_bin          # 供 CGI 默认路径走 web 相对（可选）

export WFR_BIN="${WFR_BIN:-$PWD/build/dev/bin/wfr}"
# GCC16 libstdc++ 运行时（wfr_cgi / wfr 都需 GLIBCXX_3.4.35）；http.server --cgi 会继承此 env
if [[ -f "$HOME/opt/gcc16/env.sh" ]]; then
  source "$HOME/opt/gcc16/env.sh" 2>/dev/null || true
elif [[ -d "$HOME/opt/gcc16/usr/lib" ]]; then
  export LD_LIBRARY_PATH="$HOME/opt/gcc16/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
# 局域网可访问：监听 0.0.0.0（所有接口），并打印本机 IP 供其他设备访问。
# 注意：绑 0.0.0.0 会让同网段设备都能访问此页面（含 CGI 调 wfr 的能力）——仅限信任网络。
LAN_IPS="$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -v '^$' | head -5)"
echo "启动 CGI WebUI: http://localhost:${PORT}/（本机）"
if [[ -n "$LAN_IPS" ]]; then
  for ip in $LAN_IPS; do
    echo "  局域网访问: http://${ip}:${PORT}/"
  done
fi
echo "  wfr=${WFR_BIN}"
cd web
python3 -m http.server --cgi --bind 0.0.0.0 "${PORT}"