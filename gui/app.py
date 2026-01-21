import json
import sys
from collections import deque
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import (
    Qt, QTimer, QEasingCurve, QPropertyAnimation, QByteArray, QMargins
)
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QLabel,
    QProgressBar, QFrame, QTableWidget, QTableWidgetItem, QAbstractItemView
)
from PySide6.QtCharts import QChart, QChartView, QLineSeries
from PySide6.QtGui import QPainter
from PySide6.QtCore import QProcess


APP_QSS = """
* { font-family: Segoe UI; }
QMainWindow { background: #0b0f14; }

QFrame#Card {
  background: rgba(255,255,255,0.04);
  border: 1px solid rgba(255,255,255,0.08);
  border-radius: 16px;
}
QFrame#Card:hover {
  border: 1px solid rgba(120,180,255,0.28);
  background: rgba(255,255,255,0.05);
}

QLabel { color: rgba(255,255,255,0.86); }
QLabel#Title { font-size: 22px; font-weight: 800; }
QLabel#Sub { color: rgba(255,255,255,0.55); }

QProgressBar {
  background: rgba(255,255,255,0.06);
  border: 1px solid rgba(255,255,255,0.08);
  border-radius: 10px;
  text-align: center;
  color: rgba(255,255,255,0.85);
  height: 18px;
}
QProgressBar::chunk {
  border-radius: 10px;
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                              stop:0 rgba(120,180,255,0.95),
                              stop:1 rgba(190,120,255,0.85));
}

QTableWidget {
  background: transparent;
  border: 1px solid rgba(255,255,255,0.08);
  border-radius: 12px;
  gridline-color: rgba(255,255,255,0.06);
  color: rgba(255,255,255,0.85);
}
QTableWidget::item {
  padding: 6px;
}
QTableWidget::item:hover {
  background: rgba(120,180,255,0.10);
}
QTableWidget::item:selected {
  background: rgba(190,120,255,0.18);
}

QHeaderView::section {
  background: rgba(255,255,255,0.05);
  border: none;
  padding: 8px;
  color: rgba(255,255,255,0.72);
  font-weight: 700;
}
"""


def clamp01(x: float) -> float:
    return max(0.0, min(100.0, float(x)))

def collector_path() -> str:
    """
    Dev: ../c-core/collector.exe
    Packaged: collector.exe might end up in a few places depending on PyInstaller
    """
    if getattr(sys, "frozen", False):
        exe_dir = Path(sys.executable).resolve().parent

        candidates = [
            exe_dir / "collector.exe",
            exe_dir / "_internal" / "collector.exe",
            exe_dir / "_internal" / "_internal" / "collector.exe",  # <- your case
        ]

        for p in candidates:
            if p.exists():
                return str(p)

        return str(candidates[0])

    return str((Path(__file__).resolve().parent / "../c-core/collector.exe").resolve())

class Sparkline(QWidget):
    def __init__(self, title: str, max_points=120, parent=None):
        super().__init__(parent)
        self.max_points = max_points
        self.data = deque([0.0] * max_points, maxlen=max_points)

        self.series = QLineSeries()
        self.chart = QChart()
        self.chart.addSeries(self.series)
        self.chart.legend().hide()
        self.chart.setBackgroundVisible(False)
        self.chart.setMargins(QMargins(0, 0, 0, 0))
        self.chart.layout().setContentsMargins(0, 0, 0, 0)
        self.chart.createDefaultAxes()

        ax_x = self.chart.axes(Qt.Horizontal)[0]
        ax_y = self.chart.axes(Qt.Vertical)[0]
        ax_x.setVisible(False)
        ax_y.setVisible(False)

        self.view = QChartView(self.chart)
        self.view.setRenderHint(QPainter.Antialiasing)
        self.view.setStyleSheet("background: transparent;")

        self.label = QLabel(title)
        self.label.setStyleSheet("color: rgba(255,255,255,0.70); font-weight: 800;")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(6)
        layout.addWidget(self.label)
        layout.addWidget(self.view)

        self._rebuild_series()

    def push(self, value: float):
        self.data.append(clamp01(value))
        self._rebuild_series()

    def _rebuild_series(self):
        self.series.clear()
        for i, v in enumerate(self.data):
            self.series.append(i, v)

        ax_x = self.chart.axes(Qt.Horizontal)[0]
        ax_y = self.chart.axes(Qt.Vertical)[0]
        ax_x.setRange(0, self.max_points - 1)
        ax_y.setRange(0, 100)


class AnimatedBar(QProgressBar):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._anim = QPropertyAnimation(self, b"value", self)
        self._anim.setDuration(220)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)

    def set_value_smooth(self, v: float):
        v_int = int(round(clamp01(v)))
        self._anim.stop()
        self._anim.setStartValue(self.value())
        self._anim.setEndValue(v_int)
        self._anim.start()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("SysMon")
        self.resize(1150, 760)

        title = QLabel("SysMon")
        title.setObjectName("Title")

        sub = QLabel("Live system stats • 500ms refresh • Dark UI • Graphs + Table")
        sub.setObjectName("Sub")

        self.status = QLabel("Starting collector…")
        self.status.setObjectName("Sub")

        header = QHBoxLayout()
        header.addWidget(title)
        header.addStretch(1)
        header.addWidget(self.status)

        header_wrap = QVBoxLayout()
        header_wrap.addLayout(header)
        header_wrap.addWidget(sub)

        # bars + lines
        self.cpu_bar = AnimatedBar()
        self.ram_bar = AnimatedBar()
        self.disk_bar = AnimatedBar()

        self.cpu_line = QLabel("CPU: —")
        self.ram_line = QLabel("RAM: —")
        self.disk_line = QLabel("Disk: —")
        self.net_line = QLabel("NET: —")
        for lbl in (self.cpu_line, self.ram_line, self.disk_line, self.net_line):
            lbl.setStyleSheet("color: rgba(255,255,255,0.72);")

        # charts
        self.cpu_chart = Sparkline("CPU (%)")
        self.ram_chart = Sparkline("RAM (%)")
        self.disk_chart = Sparkline("Disk Active (%)")

        left_card = self._card(
            "Core",
            widgets=[
                ("CPU", self.cpu_bar, self.cpu_line),
                ("RAM", self.ram_bar, self.ram_line),
                ("DISK (Active)", self.disk_bar, self.disk_line),
                ("NETWORK", None, self.net_line),
            ],
        )

        charts_card = self._card(
            "History",
            widgets=[("CPU", self.cpu_chart, None),
                     ("RAM", self.ram_chart, None),
                     ("DISK", self.disk_chart, None)]
        )

        # processes table
        self.proc_table = QTableWidget(0, 3)
        self.proc_table.setHorizontalHeaderLabels(["Process", "RAM (MB)", "CPU (%)"])
        self.proc_table.horizontalHeader().setStretchLastSection(True)
        self.proc_table.verticalHeader().setVisible(False)
        self.proc_table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.proc_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.proc_table.setSortingEnabled(False)

        procs_card = self._frame_card()
        procs_layout = QVBoxLayout(procs_card)
        procs_layout.setContentsMargins(14, 14, 14, 14)
        procs_layout.setSpacing(10)

        procs_title = QLabel("Top Processes")
        procs_title.setStyleSheet("color: rgba(255,255,255,0.80); font-weight: 900; font-size: 14px;")
        procs_layout.addWidget(procs_title)
        procs_layout.addWidget(self.proc_table)

        # main layout
        root = QWidget()
        root_layout = QVBoxLayout(root)
        root_layout.setContentsMargins(18, 18, 18, 18)
        root_layout.setSpacing(14)
        root_layout.addLayout(header_wrap)

        row = QHBoxLayout()
        row.setSpacing(14)
        row.addWidget(left_card, 1)
        row.addWidget(charts_card, 2)

        root_layout.addLayout(row, 2)
        root_layout.addWidget(procs_card, 2)

        self.setCentralWidget(root)

        # streaming collector
        self.proc = QProcess(self)
        self.proc.readyReadStandardOutput.connect(self._on_stdout)
        self.proc.readyReadStandardError.connect(self._on_stderr)
        self.proc.finished.connect(self._on_finished)

        self._buffer = QByteArray()

        exe = collector_path()
        self.status.setText(f"Collector: {Path(exe).name} (stream 500ms)")
        self.proc.start(exe, ["--stream", "500"])

        # watchdog (if collector dies, show status)
        self._watch = QTimer(self)
        self._watch.timeout.connect(self._watchdog)
        self._watch.start(1000)

    def _frame_card(self):
        frame = QFrame()
        frame.setObjectName("Card")
        frame.setFrameShape(QFrame.NoFrame)
        return frame

    def _card(self, title_text, widgets):
        card = self._frame_card()
        layout = QVBoxLayout(card)
        layout.setContentsMargins(14, 14, 14, 14)
        layout.setSpacing(10)

        title = QLabel(title_text)
        title.setStyleSheet("color: rgba(255,255,255,0.78); font-weight: 900; font-size: 14px;")
        layout.addWidget(title)

        for name, w1, w2 in widgets:
            row = QVBoxLayout()
            row.setSpacing(6)

            label = QLabel(name)
            label.setStyleSheet("color: rgba(255,255,255,0.60); font-weight: 800; font-size: 12px;")
            row.addWidget(label)

            if w1 is not None:
                row.addWidget(w1)
            if w2 is not None:
                row.addWidget(w2)

            wrap = QWidget()
            wrap.setLayout(row)
            layout.addWidget(wrap)

        layout.addStretch(1)
        return card

    def _fmt_speed(self, kbps: float) -> str:
        if kbps >= 1000.0:
            return f"{kbps/1000.0:.2f} Mbps"
        return f"{kbps:.1f} kbps"

    def _apply_data(self, data: dict):
        cpu = float(data.get("cpu_percent", 0.0))
        ram = float(data.get("mem_used_percent", 0.0))
        disk = float(data.get("disk_active_percent", 0.0))
        down = float(data.get("net_down_kbps", 0.0))
        up = float(data.get("net_up_kbps", 0.0))

        total_mb = int(data.get("mem_total_mb", 0))
        used_mb = int(data.get("mem_used_mb", 0))

        disk_total = float(data.get("disk_total_gb", 0.0))
        disk_used = float(data.get("disk_used_gb", 0.0))
        disk_free = float(data.get("disk_free_gb", 0.0))

        self.cpu_bar.set_value_smooth(cpu)
        self.ram_bar.set_value_smooth(ram)
        self.disk_bar.set_value_smooth(disk)

        self.cpu_line.setText(f"CPU: {cpu:.1f}%")
        self.ram_line.setText(f"RAM: {used_mb} / {total_mb} MB ({ram:.1f}%)")
        self.disk_line.setText(f"Disk: Active {disk:.1f}%  •  {disk_used:.1f}/{disk_total:.1f} GB (Free {disk_free:.1f} GB)")
        self.net_line.setText(f"NET: ↓ {self._fmt_speed(down)}   ↑ {self._fmt_speed(up)}")

        self.cpu_chart.push(cpu)
        self.ram_chart.push(ram)
        self.disk_chart.push(disk)

        # processes
        procs = data.get("top_procs", [])
        self._update_process_table(procs)

        self.status.setText(datetime.now().strftime("Updated %H:%M:%S.%f")[:-3])

    def _update_process_table(self, procs):
        # procs: [{name, ram_mb, cpu_percent(null)}...]
        self.proc_table.setUpdatesEnabled(False)

        self.proc_table.setRowCount(len(procs))
        for r, p in enumerate(procs):
            name = str(p.get("name", ""))
            ram_mb = p.get("ram_mb", 0)
            cpu_p = p.get("cpu_percent", None)

            it_name = QTableWidgetItem(name)
            it_ram = QTableWidgetItem(str(ram_mb))
            it_cpu = QTableWidgetItem("—" if cpu_p is None else f"{float(cpu_p):.1f}")

            # right align numbers
            it_ram.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)
            it_cpu.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)

            self.proc_table.setItem(r, 0, it_name)
            self.proc_table.setItem(r, 1, it_ram)
            self.proc_table.setItem(r, 2, it_cpu)

        self.proc_table.resizeColumnsToContents()
        self.proc_table.setUpdatesEnabled(True)

    def _on_stdout(self):
        self._buffer += self.proc.readAllStandardOutput()
        data_bytes = self._buffer.data()

        while b"\n" in data_bytes:
            raw, _, rest = data_bytes.partition(b"\n")
            self._buffer = QByteArray(rest)
            data_bytes = self._buffer.data()

            line = raw.strip()
            if not line:
                continue
            try:
                obj = json.loads(line.decode("utf-8", errors="replace"))
                if obj.get("ok"):
                    self._apply_data(obj)
            except Exception:
                pass

    def _on_stderr(self):
        err = bytes(self.proc.readAllStandardError()).decode("utf-8", errors="replace").strip()
        if err:
            self.status.setText(err[:90])

    def _on_finished(self):
        self.status.setText("Collector stopped.")

    def _watchdog(self):
        if self.proc.state() != QProcess.Running:
            self.status.setText("Collector not running. (Packaging will include it next.)")

    def closeEvent(self, event):
        try:
            if hasattr(self, "proc") and self.proc is not None:
                if self.proc.state() == QProcess.Running:
                    self.proc.terminate()
                    if not self.proc.waitForFinished(800):
                        self.proc.kill()
                        self.proc.waitForFinished(800)
        except Exception:
            pass
        super().closeEvent(event)



def main():
    app = QApplication(sys.argv)
    app.setStyleSheet(APP_QSS)

    w = MainWindow()
    w.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
