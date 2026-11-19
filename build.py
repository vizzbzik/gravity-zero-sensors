
import os
import sys
import subprocess
import shutil
from pathlib import Path

ROOT = Path(__file__).parent.resolve()
MAIN = ROOT / "main.py"
DIST = ROOT / "dist"
BUILD = ROOT / "build"

APP_NAME = "GravityZero"

def run(command):
    print("\n[G0 BUILD] Выполняю:", " ".join(map(str, command)))
    subprocess.check_call(command, cwd=ROOT)

def main():
    print("=" * 55)
    print("          GRAVITYZERO CLI BUILD SYSTEM")
    print("                 VERSION 1.0")
    print("=" * 55)

    if not MAIN.exists():
        print("[ERROR] main.py не найден!")
        input("Нажми Enter для выхода...")
        return

    print("\n[1/4] Проверка PyInstaller...")

    try:
        import PyInstaller
    except ImportError:
        print("[INFO] Устанавливаю PyInstaller...")
        run([sys.executable, "-m", "pip", "install", "pyinstaller"])

    print("\n[2/4] Очистка старой сборки...")

    for folder in (DIST, BUILD):
        if folder.exists():
            shutil.rmtree(folder)

    spec = ROOT / f"{APP_NAME}.spec"
    if spec.exists():
        spec.unlink()

    print("\n[3/4] Компиляция EXE...")

    command = [
        sys.executable,
        "-m", "PyInstaller",
        "--onefile",
        "--console",
        "--clean",
        "--noconfirm",
        "--name", APP_NAME,
        str(MAIN)
    ]

    try:
        run(command)
    except subprocess.CalledProcessError:
        print("\n[ERROR] Сборка завершилась с ошибкой!")
        input("Нажми Enter для выхода...")
        return

    print("\n[4/4] Проверка результата...")

    exe = DIST / f"{APP_NAME}.exe"

    if exe.exists():
        print("\n" + "=" * 55)
        print("[SUCCESS] GRAVITYZERO EXE СОБРАН!")
        print(f"[FILE] {exe}")
        print(f"[SIZE] {exe.stat().st_size / 1024 / 1024:.2f} MB")
        print("=" * 55)
    else:
        print("[ERROR] EXE не найден!")

    input("\nНажми Enter для выхода...")

if __name__ == "__main__":
    main()
