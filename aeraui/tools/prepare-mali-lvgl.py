#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Apply the LVGL changes used by the Mali testing branch."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def git(folder, *args):
    return subprocess.run(['git', '-C', str(folder), *args],
                          capture_output=True, text=True)


def main():
    source = Path(__file__).resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lvgl', type=Path,
                        default=source.parents[4] / 'external/lvgl')
    parser.add_argument('--check', action='store_true',
                        help='check compatibility without changing LVGL')
    args = parser.parse_args()
    lvgl = args.lvgl.resolve()
    repository = git(lvgl, 'rev-parse', '--show-toplevel')
    if repository.returncode or Path(repository.stdout.strip()).resolve() != lvgl:
        parser.error('--lvgl must point to the external/lvgl Git checkout')
    patches = sorted((source.parents[1] / 'patches/lvgl').glob('*.patch'))
    if len(patches) != 4:
        parser.error('the four LVGL patches are missing')

    paths = set()
    for patch in patches:
        for line in patch.read_text().splitlines():
            if line.startswith(('--- a/', '+++ b/')):
                relative = Path(line[6:])
                if relative.is_absolute() or '..' in relative.parts:
                    parser.error('invalid path in ' + patch.name)
                paths.add(relative)

    # Check the complete sequence before changing the real checkout.
    with tempfile.TemporaryDirectory(prefix='aera-lvgl-') as temporary:
        error = ''
        for applied in range(len(patches), -1, -1):
            shadow = Path(temporary) / str(applied)
            subprocess.run(['git', 'init', '-q', str(shadow)], check=True)
            for relative in paths:
                original = lvgl / relative
                if original.is_file():
                    target = shadow / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(original, target)
            valid = True
            for patch in reversed(patches[:applied]):
                if git(shadow, 'apply', '--reverse', str(patch)).returncode:
                    valid = False
                    break
            if not valid:
                continue
            for patch in patches:
                result = git(shadow, 'apply', str(patch))
                if result.returncode:
                    error = 'Cannot apply ' + patch.name + ':\n' + result.stderr
                    valid = False
                    break
            if valid:
                pending = patches[applied:]
                for patch in patches[:applied]:
                    print('Already applied:', patch.name)
                break
        else:
            raise SystemExit(error + 'LVGL was left unchanged.')

    if args.check:
        print('Compatible;', len(pending), 'patches pending.')
        return
    for patch in pending:
        subprocess.run(['git', '-C', str(lvgl), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(lvgl), 'apply', str(patch)], check=True)
        print('Applied:', patch.name)
    print('LVGL is ready.')


if __name__ == '__main__':
    main()
