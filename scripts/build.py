#!/usr/bin/env python3
"""Build in a private temporary sketch; never execute .env as shell code."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FQBN = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,UploadSpeed=115200'
DEFAULTS = dict(WIFI_SSID='', WIFI_PASSWORD='', ROOM_NAME='', REPORT_MODE='http',
                REPORT_URL='', REPORT_TOKEN='', REPORT_INTERVAL_SECONDS='60',
                THINGSBOARD_URL='', THINGSBOARD_ACCESS_TOKEN='')


def read_env(path, required=False):
    result = dict(DEFAULTS)
    if not path.exists():
        if required:
            raise ValueError('指定的配置文件不存在')
        return result
    seen = set()
    for number, line in enumerate(path.read_text(encoding='utf-8-sig').splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        match = re.fullmatch(r'([A-Z][A-Z0-9_]*)\s*=\s*(.*)', line)
        if not match:
            raise ValueError(f'.env 第 {number} 行格式错误，需要 KEY=value')
        key, value = match.groups()
        if key not in DEFAULTS or key in seen:
            raise ValueError(f'.env 第 {number} 行包含未知或重复字段')
        seen.add(key)
        if value.startswith(('"', "'")):
            if len(value) < 2 or value[-1] != value[0]:
                raise ValueError(f'.env 第 {number} 行引号不匹配')
            value = value[1:-1]
        # Values are literal: no expansion, escape processing or inline comments.
        if any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError(f'.env 第 {number} 行包含控制字符')
        result[key] = value
    return result


def valid_url(value, base=False):
    if not value:
        return True
    match = re.fullmatch(r'https?://([A-Za-z0-9.-]+)(?::([0-9]{1,5}))?([^\s]*)', value)
    if not match or any(ord(c) >= 127 or c in '#\\' for c in value):
        return False
    host, port, tail = match.groups()
    if port and not 1 <= int(port) <= 65535:
        return False
    if base:
        return tail in ('', '/')
    return not tail or tail.startswith(('/', '?'))


def validate(env):
    for key, limit in [('WIFI_SSID',32), ('WIFI_PASSWORD',63), ('ROOM_NAME',63),
                       ('REPORT_URL',256), ('REPORT_TOKEN',256),
                       ('THINGSBOARD_URL',256), ('THINGSBOARD_ACCESS_TOKEN',256)]:
        if len(env[key].encode('utf-8')) > limit:
            raise ValueError(f'{key} 超过 {limit} 字节')
    if env['WIFI_PASSWORD'] and (not env['WIFI_SSID'] or len(env['WIFI_PASSWORD'].encode()) < 8):
        raise ValueError('WIFI_PASSWORD 需要至少 8 字节，且 WIFI_SSID 不能为空')
    if env['REPORT_MODE'] not in ('http', 'thingsboard'):
        raise ValueError('REPORT_MODE 必须是 http 或 thingsboard')
    interval = env['REPORT_INTERVAL_SECONDS']
    if not re.fullmatch(r'[0-9]{1,5}', interval) or not 10 <= int(interval) <= 86400:
        raise ValueError('REPORT_INTERVAL_SECONDS 必须为 10–86400 整数秒')
    for key in ('REPORT_TOKEN', 'THINGSBOARD_ACCESS_TOKEN'):
        if any(not 0x21 <= ord(c) <= 0x7e for c in env[key]):
            raise ValueError(f'{key} 仅支持无空白 ASCII 字符')
    if not valid_url(env['REPORT_URL']) or not valid_url(env['THINGSBOARD_URL'], base=True):
        raise ValueError('URL 无效；ThingsBoard 仅填写 http(s)://主机[:端口]')
    if env['REPORT_MODE'] == 'thingsboard':
        if env['REPORT_URL'] or env['REPORT_TOKEN']:
            raise ValueError('ThingsBoard 模式请清空 REPORT_URL/REPORT_TOKEN，改填 THINGSBOARD_*')
        if env['THINGSBOARD_URL'] and not env['THINGSBOARD_ACCESS_TOKEN']:
            raise ValueError('ThingsBoard 服务器地址已填写，但缺少 Device Access Token')
    elif env['THINGSBOARD_URL'] or env['THINGSBOARD_ACCESS_TOKEN']:
        raise ValueError('填写 THINGSBOARD_* 后请设置 REPORT_MODE=thingsboard')
    return env


def literal(value):
    # Fixed-width octal encodes UTF-8 and prevents quotes/backslashes from becoming C++ code.
    return '"' + ''.join('\\%03o' % b for b in value.encode('utf-8')) + '"'


def header(env):
    tb = env['REPORT_MODE'] == 'thingsboard'
    fields = {'WIFI_SSID':env['WIFI_SSID'], 'WIFI_PASSWORD':env['WIFI_PASSWORD'],
              'ROOM_NAME':env['ROOM_NAME'],
              'REPORT_URL':env['THINGSBOARD_URL'] if tb else env['REPORT_URL'],
              'REPORT_TOKEN':env['THINGSBOARD_ACCESS_TOKEN'] if tb else env['REPORT_TOKEN']}
    return '#pragma once\n' + ''.join(f'#define C3_{k} {literal(v)}\n' for k,v in fields.items()) + \
        f'#define C3_REPORT_SECONDS {int(env["REPORT_INTERVAL_SECONDS"])}\n#define C3_THINGSBOARD {int(tb)}\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--env', type=Path, help='默认使用项目 .env；指定路径不存在则报错')
    parser.add_argument('--upload', action='store_true', help='成功编译后上传本次固件')
    parser.add_argument('--port', help='上传串口，例如 /dev/cu.usbmodem101')
    args = parser.parse_args()
    if args.upload and not args.port:
        parser.error('--upload 需要 --port')
    if args.port and not args.upload:
        parser.error('--port 需要 --upload')
    os.umask(0o077)
    try:
        env = validate(read_env(args.env or ROOT / '.env', required=args.env is not None))
        if not shutil.which('arduino-cli'):
            raise ValueError('未找到 arduino-cli，请先安装并配置 ESP32 Core 和依赖库')
        with tempfile.TemporaryDirectory(prefix='c3-build-') as temp:
            base = Path(temp)
            sketch = base / 'c3_temperature'
            sketch.mkdir()
            for pattern in ('*.ino', '*.h', '*.cpp'):
                for source in ROOT.glob(pattern):
                    shutil.copy2(source, sketch / source.name)
            (sketch / 'BuildDefaults.h').write_text(header(env))
            build = base / 'build'
            # No secret values are passed as CLI flags or printed.
            subprocess.run(['arduino-cli','compile','--fqbn',FQBN,'--build-path',str(build),str(sketch)], check=True)
            output = ROOT / 'build' / 'firmware'
            output.mkdir(parents=True, exist_ok=True)
            for artifact in build.glob('*.bin'):
                shutil.copy2(artifact, output / artifact.name)
            print('编译成功。固件位于 build/firmware（可能包含凭据，请勿公开）。', flush=True)
            if args.upload:
                subprocess.run(['arduino-cli','upload','--fqbn',FQBN,'--port',args.port,
                                '--input-dir',str(build),str(sketch)], check=True)
    except (ValueError, OSError, UnicodeError) as exc:
        # Parser errors never include config values. Avoid subprocess argument dumps.
        print('构建失败：' + (str(exc) if type(exc) is ValueError else '无法读取配置或执行构建工具'))
        return 1
    except subprocess.CalledProcessError:
        print('编译或上传失败；未继续执行后续步骤。')
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
