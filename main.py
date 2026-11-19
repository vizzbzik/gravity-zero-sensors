import json
import time
import shlex
import sys

import serial
import serial.tools.list_ports

from rich.console import Console
from rich.table import Table
from rich.panel import Panel
from rich.prompt import Prompt
from rich import box


console = Console()

BAUD_RATE = 115200
TIMEOUT = 2

VERSION = "1.1.0"


class GravityZeroCLI:

    def __init__(self):
        self.serial = None
        self.port = None
        self.running = True

    # -----------------------------------------------
    # UI
    # -----------------------------------------------

    def banner(self):
        console.clear()

        console.print(
            Panel.fit(
                "[bold cyan]GRAVITYZERO[/bold cyan]\n"
                "[white]Environmental Monitoring System[/white]\n"
                f"[dim]CLI Version {VERSION} | USB Serial[/dim]",
                border_style="cyan"
            )
        )

    def prompt(self):
        if self.serial and self.serial.is_open:
            return "[bold green]root@gravityzero[/bold green]:[cyan]~[/cyan]# "

        return "[bold red]root@g0[/bold red]:[cyan]~[/cyan]# "

    def connected(self):
        return self.serial is not None and self.serial.is_open

    # -----------------------------------------------
    # PORTS
    # -----------------------------------------------

    def list_ports(self):
        ports = list(serial.tools.list_ports.comports())

        table = Table(
            title="Available Serial Ports",
            box=box.ROUNDED
        )

        table.add_column("PORT", style="cyan")
        table.add_column("DEVICE")
        table.add_column("DESCRIPTION")

        for p in ports:
            table.add_row(
                p.device,
                p.name,
                p.description or "Unknown"
            )

        console.print(table)

        if not ports:
            console.print("[yellow]No COM ports found.[/yellow]")

    def connect(self, port=None):
        if self.connected():
            console.print("[yellow]Already connected.[/yellow]")
            return

        ports = list(serial.tools.list_ports.comports())

        if not ports:
            console.print("[red]No serial ports detected.[/red]")
            return

        if port is None:
            self.list_ports()

            port = Prompt.ask(
                "Select COM port",
                default=ports[0].device
            )

        try:
            self.serial = serial.Serial(
                port=port,
                baudrate=BAUD_RATE,
                timeout=TIMEOUT,
                write_timeout=TIMEOUT
            )

            self.port = port

            # ESP32 may reboot when the serial port opens.
            time.sleep(2)

            self.serial.reset_input_buffer()

            console.print(
                f"[green][+] Connected to {port}[/green]"
            )

            console.print(
                "[green][+] USB Serial connection established[/green]"
            )

            result = self.request({"cmd": "status"})

            if result and result.get("ok"):
                console.print(
                    f"[cyan][+] Device: "
                    f"{result.get('data', {}).get('device', 'Unknown')}[/cyan]"
                )

        except (serial.SerialException, OSError) as e:
            self.serial = None
            self.port = None

            console.print(f"[red]Connection failed: {e}[/red]")

    def disconnect(self):
        if not self.connected():
            console.print("[yellow]Not connected.[/yellow]")
            return

        try:
            self.serial.close()
        except Exception:
            pass

        self.serial = None
        self.port = None

        console.print("[yellow]Disconnected.[/yellow]")

    # -----------------------------------------------
    # SERIAL JSON PROTOCOL
    # -----------------------------------------------

    def request(self, payload):
        if not self.connected():
            console.print("[red]Not connected. Use connect first.[/red]")
            return None

        try:
            self.serial.reset_input_buffer()

            message = json.dumps(payload) + "\n"

            self.serial.write(message.encode("utf-8"))
            self.serial.flush()

            deadline = time.time() + TIMEOUT + 2

            while time.time() < deadline:
                line = self.serial.readline()

                if not line:
                    continue

                try:
                    decoded = line.decode(
                        "utf-8",
                        errors="ignore"
                    ).strip()

                    if not decoded.startswith("{"):
                        continue

                    result = json.loads(decoded)

                    if isinstance(result, dict) and (
                        "ok" in result or "data" in result
                    ):
                        return result

                except json.JSONDecodeError:
                    continue

            console.print("[red]Device response timeout.[/red]")

        except (serial.SerialException, OSError) as e:
            console.print(f"[red]Serial error: {e}[/red]")

        return None

    def check_result(self, result):
        if result is None:
            return False

        if not result.get("ok", False):
            console.print(
                f"[red]Error: {result.get('error', 'Unknown error')}[/red]"
            )
            return False

        return True

    # -----------------------------------------------
    # STATUS
    # -----------------------------------------------

    def show_status(self):
        result = self.request({"cmd": "status"})

        if not self.check_result(result):
            return

        data = result.get("data", {})

        table = Table(
            title="GravityZero System Status",
            box=box.ROUNDED
        )

        table.add_column("PARAMETER", style="cyan")
        table.add_column("VALUE", style="white")

        table.add_row("Device", str(data.get("device")))
        table.add_row("Firmware", str(data.get("version")))
        table.add_row(
            "Temperature",
            self.format_sensor(data.get("temperature"),
                               data.get("temperatureValid"), "°C")
        )
        table.add_row(
            "Humidity",
            self.format_sensor(data.get("humidity"),
                               data.get("humidityValid"), "%")
        )
        table.add_row(
            "MQ-2 RAW",
            str(data.get("gas", "--"))
        )
        table.add_row(
            "MQ-2 Voltage",
            f"{data.get('voltage', 0):.2f} V"
        )
        table.add_row(
            "Alarm",
            "[red]ACTIVE[/red]" if data.get("alarm")
            else "[green]NORMAL[/green]"
        )
        table.add_row(
            "Alarm enabled",
            str(data.get("alarmEnabled"))
        )
        table.add_row(
            "Uptime",
            f"{data.get('uptime', 0)} seconds"
        )
        table.add_row(
            "Free heap",
            f"{data.get('freeHeap', 0) // 1024} KB"
        )

        console.print(table)

    @staticmethod
    def format_sensor(value, valid, unit):
        if not valid or value is None:
            return "[red]OFFLINE[/red]"

        return f"{value} {unit}"

    def show_sensors(self):
        result = self.request({"cmd": "sensors"})

        if not self.check_result(result):
            return

        data = result.get("data", {})

        console.print(
            Panel(
                f"""
[cyan]DHT22[/cyan]

Temperature: {self.format_sensor(
    data.get('temperature'),
    data.get('temperatureValid'),
    '°C'
)}

Humidity: {self.format_sensor(
    data.get('humidity'),
    data.get('humidityValid'),
    '%'
)}

[cyan]MQ-2[/cyan]

Raw ADC: {data.get('gas', '--')}
Voltage: {data.get('voltage', 0):.2f} V

[cyan]ALARM[/cyan]

Status: {'ACTIVE' if data.get('alarm') else 'NORMAL'}
                """,
                title="Sensor Diagnostics",
                border_style="cyan"
            )
        )

    # -----------------------------------------------
    # PINS
    # -----------------------------------------------

    def pins_list(self):
        result = self.request({"cmd": "pins_list"})

        if not self.check_result(result):
            return

        pins = result.get("pins", {})

        table = Table(
            title="GPIO Configuration",
            box=box.ROUNDED
        )

        table.add_column("DEVICE", style="cyan")
        table.add_column("GPIO", style="green")

        for device, pin in pins.items():
            table.add_row(device.upper(), str(pin))

        console.print(table)

    def pins_set(self, device, pin):
        try:
            pin = int(pin)
        except ValueError:
            console.print("[red]GPIO must be a number.[/red]")
            return

        result = self.request({
            "cmd": "pins_set",
            "device": device.lower(),
            "pin": pin
        })

        if self.check_result(result):
            console.print(
                f"[green][+] {device.upper()} moved to GPIO{pin}[/green]"
            )

    # -----------------------------------------------
    # CONFIGURATION
    # -----------------------------------------------

    def config_show(self):
        result = self.request({"cmd": "config_get"})

        if not self.check_result(result):
            return

        table = Table(
            title="Configuration",
            box=box.ROUNDED
        )

        table.add_column("SETTING", style="cyan")
        table.add_column("VALUE", style="green")

        table.add_row("Gas threshold", str(result.get("gas")))
        table.add_row("Temperature threshold", str(result.get("temp")))
        table.add_row("Humidity threshold", str(result.get("humidity")))
        table.add_row("Alarm enabled", str(result.get("alarm")))

        console.print(table)

    def config_set(self, key, value):
        try:
            value = float(value)
        except ValueError:
            console.print("[red]Value must be numeric.[/red]")
            return

        result = self.request({
            "cmd": "config_set",
            "key": key.lower(),
            "value": value
        })

        if self.check_result(result):
            console.print("[green][+] Configuration updated.[/green]")
            console.print("[dim]Use config save to persist it.[/dim]")

    def config_save(self):
        result = self.request({"cmd": "config_save"})

        if self.check_result(result):
            console.print("[green][+] Configuration saved.[/green]")

    # -----------------------------------------------
    # ALARM
    # -----------------------------------------------

    def alarm(self, action):
        if action not in ("enable", "disable"):
            console.print("[red]Usage: alarm enable|disable[/red]")
            return

        if action == "disable":
            console.print(
                "[yellow]WARNING: This disables audible and visual "
                "alarm output. This is a prototype, not a certified "
                "safety detector.[/yellow]"
            )

        result = self.request({
            "cmd": "alarm",
            "enabled": action == "enable"
        })

        if self.check_result(result):
            console.print(f"[green][+] Alarm {action}d.[/green]")

    # -----------------------------------------------
    # LOGS
    # -----------------------------------------------

    def show_logs(self):
        result = self.request({"cmd": "logs"})

        if not self.check_result(result):
            return

        console.print(
            Panel(
                result.get("logs", "No logs"),
                title="GravityZero Event Log",
                border_style="cyan"
            )
        )

    # -----------------------------------------------
    # MONITOR
    # -----------------------------------------------

    def monitor(self):
        if not self.connected():
            console.print("[red]Connect first.[/red]")
            return

        console.print("[yellow]Monitoring started. Press Ctrl+C to stop.[/yellow]")

        try:
            while True:
                result = self.request({"cmd": "status"})

                if result and result.get("ok"):
                    data = result.get("data", {})

                    temp = self.format_sensor(
                        data.get("temperature"),
                        data.get("temperatureValid"),
                        "°C"
                    )

                    hum = self.format_sensor(
                        data.get("humidity"),
                        data.get("humidityValid"),
                        "%"
                    )

                    gas = data.get("gas", "--")
                    alarm = data.get("alarm", False)

                    console.print(
                        f"[cyan]{time.strftime('%H:%M:%S')}[/cyan] "
                        f"TEMP: {temp} | "
                        f"HUM: {hum} | "
                        f"MQ2: {gas} | "
                        f"ALARM: "
                        f"{'[red]ACTIVE[/red]' if alarm else '[green]NORMAL[/green]'}"
                    )

                time.sleep(1)

        except KeyboardInterrupt:
            console.print("\n[yellow]Monitoring stopped.[/yellow]")

    # -----------------------------------------------
    # SYSTEM
    # -----------------------------------------------

    def reboot(self):
        confirm = Prompt.ask(
            "Reboot GravityZero?",
            choices=["y", "n"],
            default="n"
        )

        if confirm != "y":
            return

        result = self.request({"cmd": "reboot"})

        if result:
            console.print("[yellow]Reboot command sent.[/yellow]")

        time.sleep(1)

    # -----------------------------------------------
    # HELP
    # -----------------------------------------------

    def help(self):
        table = Table(
            title="GravityZero CLI Commands",
            box=box.ROUNDED
        )

        table.add_column("COMMAND", style="cyan")
        table.add_column("DESCRIPTION")

        commands = [
            ("ports", "List available COM ports"),
            ("connect [COMx]", "Connect to ESP32"),
            ("disconnect", "Disconnect USB"),
            ("status", "System status"),
            ("sensors", "Sensor diagnostics"),
            ("monitor", "Live monitoring"),
            ("pins list", "List GPIO assignments"),
            ("pins set DEVICE GPIO", "Change sensor GPIO"),
            ("config show", "Show configuration"),
            ("config set KEY VALUE", "Change threshold"),
            ("config save", "Save settings to NVS"),
            ("alarm enable", "Enable alarm"),
            ("alarm disable", "Disable alarm"),
            ("logs", "Show event log"),
            ("system reboot", "Restart ESP32"),
            ("help", "Show this help"),
            ("exit", "Exit CLI"),
        ]

        for command, description in commands:
            table.add_row(command, description)

        console.print(table)

    # -----------------------------------------------
    # COMMAND ROUTER
    # -----------------------------------------------

    def execute(self, line):
        try:
            args = shlex.split(line)
        except ValueError:
            console.print("[red]Invalid command syntax.[/red]")
            return

        if not args:
            return

        cmd = args[0].lower()

        if cmd == "help":
            self.help()

        elif cmd == "ports":
            self.list_ports()

        elif cmd == "connect":
            self.connect(args[1] if len(args) > 1 else None)

        elif cmd == "disconnect":
            self.disconnect()

        elif cmd == "status":
            self.show_status()

        elif cmd == "sensors":
            self.show_sensors()

        elif cmd == "monitor":
            self.monitor()

        elif cmd == "pins":
            if len(args) >= 2 and args[1] == "list":
                self.pins_list()

            elif len(args) >= 4 and args[1] == "set":
                self.pins_set(args[2], args[3])

            else:
                console.print("[red]Usage: pins list | pins set DEVICE GPIO[/red]")

        elif cmd == "config":
            if len(args) >= 2 and args[1] == "show":
                self.config_show()

            elif len(args) >= 4 and args[1] == "set":
                self.config_set(args[2], args[3])

            elif len(args) >= 2 and args[1] == "save":
                self.config_save()

            else:
                console.print(
                    "[red]Usage: config show | config set KEY VALUE | config save[/red]"
                )

        elif cmd == "alarm":
            if len(args) >= 2:
                self.alarm(args[1])
            else:
                console.print("[red]Usage: alarm enable|disable[/red]")

        elif cmd == "logs":
            self.show_logs()

        elif cmd == "system":
            if len(args) >= 2 and args[1] == "reboot":
                self.reboot()
            else:
                console.print("[red]Usage: system reboot[/red]")

        elif cmd in ("exit", "quit"):
            self.running = False

        else:
            console.print(f"[red]Unknown command: {cmd}[/red]")

    # -----------------------------------------------
    # MAIN LOOP
    # -----------------------------------------------

    def run(self):
        self.banner()
        self.help()

        while self.running:
            try:
                line = Prompt.ask(self.prompt(), default="")
                self.execute(line)

            except KeyboardInterrupt:
                console.print("\n[yellow]Use exit to close CLI.[/yellow]")

            except EOFError:
                break

        self.disconnect()

        console.print("[cyan]GravityZero CLI closed.[/cyan]")


if __name__ == "__main__":
    app = GravityZeroCLI()
    app.run()