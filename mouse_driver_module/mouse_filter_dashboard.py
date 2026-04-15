#!/usr/bin/env python3
"""
Giao diện demo cho kernel module mouse_input_filter.

- Đọc realtime từ /proc/mouse_log và /proc/mouse_entropy
- Hiển thị log theo loại sự kiện
- Vẽ biểu đồ dx/dy theo thời gian
- Xuất snapshot trạng thái ra file TXT
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import datetime as dt
import re
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

LOG_PATH = "/proc/mouse_log"
ENTROPY_PATH = "/proc/mouse_entropy"

LOG_RE = re.compile(r"^\[(\s*\d+)\.(\d{3})\]\s+(.*)$")
ENTROPY_RE = re.compile(r"^\[(\s*\d+)\.(\d{3})\]\s+dx=(-?\d+)\s+dy=(-?\d+)$")


@dataclass(frozen=True)
class LogEntry:
    ts: str
    kind: str
    message: str


@dataclass(frozen=True)
class EntropyPoint:
    ts: str
    dx: int
    dy: int


class MouseFilterDashboard(tk.Tk):
    POLL_MS = 500
    MAX_LOG_ROWS = 320
    MAX_ENTROPY_POINTS = 240

    def __init__(self) -> None:
        super().__init__()
        self.title("Mouse Input Filter Dashboard")
        self.geometry("1280x760")
        self.minsize(980, 600)
        self.configure(bg="#eaf1f7")

        self.running = True
        self.last_error = ""

        self.log_seen_queue: deque[tuple[str, str]] = deque(maxlen=4096)
        self.log_seen_set: set[tuple[str, str]] = set()

        self.entropy_seen_queue: deque[tuple[str, int, int]] = deque(maxlen=4096)
        self.entropy_seen_set: set[tuple[str, int, int]] = set()

        self.entropy_points: deque[EntropyPoint] = deque(maxlen=self.MAX_ENTROPY_POINTS)

        self.metrics = {
            "total_events": 0,
            "converted_left": 0,
            "blocked_right": 0,
            "move_events": 0,
            "wheel_events": 0,
        }

        self.module_state_var = tk.StringVar(value="Trạng thái module: Chưa xác định")
        self.logging_state_var = tk.StringVar(value="Logging: Chưa rõ")
        self.data_source_var = tk.StringVar(value=f"Nguồn: {LOG_PATH} | {ENTROPY_PATH}")
        self.refresh_state_var = tk.StringVar(value="Cập nhật: Đang chạy")
        self.error_var = tk.StringVar(value="")

        self.metric_vars = {
            "total_events": tk.StringVar(value="0"),
            "converted_left": tk.StringVar(value="0"),
            "blocked_right": tk.StringVar(value="0"),
            "move_events": tk.StringVar(value="0"),
            "wheel_events": tk.StringVar(value="0"),
        }

        self._setup_styles()
        self._build_ui()

        self.after(150, self.poll_data)

    def _setup_styles(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")

        style.configure("Root.TFrame", background="#eaf1f7")
        style.configure("Panel.TLabelframe", background="#f8fbff", borderwidth=1)
        style.configure("Panel.TLabelframe.Label", background="#f8fbff", foreground="#12344d", font=("TkDefaultFont", 11, "bold"))
        style.configure("StatTitle.TLabel", background="#f8fbff", foreground="#36586e", font=("TkDefaultFont", 10, "bold"))
        style.configure("StatValue.TLabel", background="#f8fbff", foreground="#0b2e47", font=("TkDefaultFont", 16, "bold"))
        style.configure("Info.TLabel", background="#eaf1f7", foreground="#1b425d", font=("TkDefaultFont", 10))
        style.configure("Error.TLabel", background="#eaf1f7", foreground="#b02a37", font=("TkDefaultFont", 10, "bold"))
        style.configure("Action.TButton", font=("TkDefaultFont", 10, "bold"))

        style.configure(
            "Treeview",
            rowheight=24,
            background="#ffffff",
            fieldbackground="#ffffff",
            foreground="#163042",
            bordercolor="#cad7e2",
        )
        style.configure("Treeview.Heading", background="#d8e8f5", foreground="#1a3f5a", font=("TkDefaultFont", 10, "bold"))

    def _build_ui(self) -> None:
        root = ttk.Frame(self, style="Root.TFrame", padding=(12, 10, 12, 12))
        root.pack(fill=tk.BOTH, expand=True)

        header = ttk.Frame(root, style="Root.TFrame")
        header.pack(fill=tk.X)

        info_frame = ttk.Frame(header, style="Root.TFrame")
        info_frame.pack(side=tk.LEFT, fill=tk.X, expand=True)

        ttk.Label(info_frame, textvariable=self.module_state_var, style="Info.TLabel").pack(anchor="w")
        ttk.Label(info_frame, textvariable=self.logging_state_var, style="Info.TLabel").pack(anchor="w")
        ttk.Label(info_frame, textvariable=self.data_source_var, style="Info.TLabel").pack(anchor="w")
        ttk.Label(info_frame, textvariable=self.refresh_state_var, style="Info.TLabel").pack(anchor="w")
        ttk.Label(info_frame, textvariable=self.error_var, style="Error.TLabel").pack(anchor="w", pady=(2, 0))

        button_frame = ttk.Frame(header, style="Root.TFrame")
        button_frame.pack(side=tk.RIGHT, anchor="ne")

        self.toggle_btn = ttk.Button(button_frame, text="Tạm dừng", style="Action.TButton", command=self.toggle_polling)
        self.toggle_btn.grid(row=0, column=0, padx=4, pady=2, sticky="ew")

        clear_btn = ttk.Button(button_frame, text="Xóa màn hình", style="Action.TButton", command=self.clear_view)
        clear_btn.grid(row=0, column=1, padx=4, pady=2, sticky="ew")

        reset_btn = ttk.Button(button_frame, text="Reset số liệu", style="Action.TButton", command=self.reset_logged_results)
        reset_btn.grid(row=0, column=2, padx=4, pady=2, sticky="ew")

        snapshot_btn = ttk.Button(button_frame, text="Snapshot TXT", style="Action.TButton", command=self.export_snapshot)
        snapshot_btn.grid(row=0, column=3, padx=4, pady=2, sticky="ew")

        stats = ttk.Frame(root, style="Root.TFrame")
        stats.pack(fill=tk.X, pady=(10, 8))

        self._create_stat_card(stats, "Sự kiện tổng", self.metric_vars["total_events"], 0)
        self._create_stat_card(stats, "Left -> Right", self.metric_vars["converted_left"], 1)
        self._create_stat_card(stats, "Right bị chặn", self.metric_vars["blocked_right"], 2)
        self._create_stat_card(stats, "MOVE frame", self.metric_vars["move_events"], 3)
        self._create_stat_card(stats, "WHEEL", self.metric_vars["wheel_events"], 4)

        body = ttk.Panedwindow(root, orient=tk.HORIZONTAL)
        body.pack(fill=tk.BOTH, expand=True)

        left_panel = ttk.Labelframe(body, text="Log Realtime", style="Panel.TLabelframe", padding=8)
        right_panel = ttk.Labelframe(body, text="Biểu đồ Entropy (dx/dy)", style="Panel.TLabelframe", padding=8)
        body.add(left_panel, weight=6)
        body.add(right_panel, weight=4)

        self.log_tree = ttk.Treeview(left_panel, columns=("time", "kind", "message"), show="headings", selectmode="browse")
        self.log_tree.heading("time", text="Thời gian")
        self.log_tree.heading("kind", text="Loại")
        self.log_tree.heading("message", text="Nội dung")
        self.log_tree.column("time", width=130, anchor="center")
        self.log_tree.column("kind", width=120, anchor="center")
        self.log_tree.column("message", width=680, anchor="w")

        self.log_tree.tag_configure("MOVE", foreground="#006d5b")
        self.log_tree.tag_configure("CONVERT", foreground="#1f4e9f")
        self.log_tree.tag_configure("BLOCK", foreground="#ab3b00")
        self.log_tree.tag_configure("WHEEL", foreground="#7a4ea3")
        self.log_tree.tag_configure("LOGGING", foreground="#7a5f00")
        self.log_tree.tag_configure("WARN", foreground="#ad1d2b")

        scroll = ttk.Scrollbar(left_panel, orient=tk.VERTICAL, command=self.log_tree.yview)
        self.log_tree.configure(yscrollcommand=scroll.set)
        self.log_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)

        chart_top = ttk.Frame(right_panel, style="Root.TFrame")
        chart_top.pack(fill=tk.BOTH, expand=True)

        self.chart_canvas = tk.Canvas(
            chart_top,
            bg="#f7fbff",
            highlightbackground="#b7ccde",
            highlightthickness=1,
            relief=tk.FLAT,
        )
        self.chart_canvas.pack(fill=tk.BOTH, expand=True)
        self.chart_canvas.bind("<Configure>", lambda _event: self.redraw_chart())

        chart_bottom = ttk.Frame(right_panel, style="Root.TFrame")
        chart_bottom.pack(fill=tk.X, pady=(8, 0))

        self.entropy_info_var = tk.StringVar(value="Entropy: chưa có dữ liệu")
        ttk.Label(chart_bottom, textvariable=self.entropy_info_var, style="Info.TLabel").pack(anchor="w")
        ttk.Label(chart_bottom, text="Chú thích: dx (đỏ) | dy (xanh)", style="Info.TLabel").pack(anchor="w")

    def _create_stat_card(self, parent: ttk.Frame, title: str, value_var: tk.StringVar, column: int) -> None:
        card = ttk.Labelframe(parent, text=title, style="Panel.TLabelframe", padding=(10, 8))
        card.grid(row=0, column=column, padx=(0, 8), sticky="nsew")
        parent.grid_columnconfigure(column, weight=1)
        ttk.Label(card, text="Realtime", style="StatTitle.TLabel").pack(anchor="w")
        ttk.Label(card, textvariable=value_var, style="StatValue.TLabel").pack(anchor="w")

    def toggle_polling(self) -> None:
        self.running = not self.running
        if self.running:
            self.refresh_state_var.set("Cập nhật: Đang chạy")
            self.toggle_btn.config(text="Tạm dừng")
            self.poll_data()
        else:
            self.refresh_state_var.set("Cập nhật: Đang tạm dừng")
            self.toggle_btn.config(text="Tiếp tục")

    def clear_view(self) -> None:
        for item in self.log_tree.get_children():
            self.log_tree.delete(item)
        self.entropy_points.clear()
        self.redraw_chart()
        self.entropy_info_var.set("Entropy: đã xóa màn hình, chờ dữ liệu mới")

    def reset_logged_results(self) -> None:
        self.clear_view()

        for key in self.metrics:
            self.metrics[key] = 0
            self.metric_vars[key].set("0")

        self.entropy_info_var.set("Entropy: đã reset số liệu, chờ dữ liệu mới")

    def export_snapshot(self) -> None:
        default_name = f"mouse_filter_snapshot_{dt.datetime.now():%Y%m%d_%H%M%S}.txt"
        file_path = filedialog.asksaveasfilename(
            title="Lưu snapshot TXT",
            defaultextension=".txt",
            initialfile=default_name,
            filetypes=(("Text file", "*.txt"), ("All file", "*.*")),
        )
        if not file_path:
            return

        lines: list[str] = []
        lines.append("=== Mouse Input Filter Snapshot ===")
        lines.append(f"Thời điểm: {dt.datetime.now().isoformat(sep=' ', timespec='seconds')}")
        lines.append(self.module_state_var.get())
        lines.append(self.logging_state_var.get())
        lines.append(self.refresh_state_var.get())
        if self.last_error:
            lines.append(f"Lỗi gần nhất: {self.last_error}")
        lines.append("")
        lines.append("--- Thống kê ---")
        lines.append(f"Sự kiện tổng: {self.metrics['total_events']}")
        lines.append(f"Left -> Right: {self.metrics['converted_left']}")
        lines.append(f"Right bị chặn: {self.metrics['blocked_right']}")
        lines.append(f"MOVE frame: {self.metrics['move_events']}")
        lines.append(f"WHEEL: {self.metrics['wheel_events']}")

        lines.append("")
        lines.append("--- Log hiển thị ---")
        children = self.log_tree.get_children()
        if not children:
            lines.append("(không có dữ liệu log trên màn hình)")
        else:
            for item in children:
                ts, kind, msg = self.log_tree.item(item, "values")
                lines.append(f"[{ts}] [{kind}] {msg}")

        lines.append("")
        lines.append("--- Entropy gần nhất ---")
        if not self.entropy_points:
            lines.append("(không có dữ liệu entropy)")
        else:
            for p in self.entropy_points:
                lines.append(f"[{p.ts}] dx={p.dx} dy={p.dy}")

        try:
            with open(file_path, "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
        except OSError as exc:
            messagebox.showerror("Lỗi lưu file", f"Không thể lưu snapshot:\n{exc}")
            return

        messagebox.showinfo("Snapshot", f"Đã lưu snapshot TXT:\n{file_path}")

    def poll_data(self) -> None:
        if not self.running:
            return

        self._read_logs()
        self._read_entropy()
        self.redraw_chart()

        self.after(self.POLL_MS, self.poll_data)

    def _read_logs(self) -> None:
        lines = self._safe_read_lines(LOG_PATH)
        if lines is None:
            return

        for line in lines:
            parsed = self._parse_log_line(line)
            if not parsed:
                continue

            key = (parsed.ts, parsed.message)
            if not self._remember_key(self.log_seen_queue, self.log_seen_set, key):
                continue

            self._append_log_row(parsed)
            self._update_metrics_from_log(parsed)

    def _read_entropy(self) -> None:
        lines = self._safe_read_lines(ENTROPY_PATH)
        if lines is None:
            return

        new_points = 0
        for line in lines:
            parsed = self._parse_entropy_line(line)
            if not parsed:
                continue

            key = (parsed.ts, parsed.dx, parsed.dy)
            if not self._remember_key(self.entropy_seen_queue, self.entropy_seen_set, key):
                continue

            self.entropy_points.append(parsed)
            new_points += 1

        if new_points:
            max_abs = max(max(abs(p.dx), abs(p.dy)) for p in self.entropy_points)
            self.entropy_info_var.set(
                f"Entropy: {len(self.entropy_points)} điểm gần nhất | biên độ lớn nhất: {max_abs}"
            )

    def _safe_read_lines(self, path: str) -> list[str] | None:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                if self.last_error:
                    self.last_error = ""
                    self.error_var.set("")
                return [line.strip() for line in f if line.strip()]
        except FileNotFoundError:
            self._set_error(f"Không tìm thấy {path}. Hãy nạp module trước.")
            return None
        except PermissionError:
            self._set_error(f"Không đủ quyền đọc {path}.")
            return None
        except OSError as exc:
            self._set_error(f"Lỗi đọc {path}: {exc}")
            return None

    def _set_error(self, msg: str) -> None:
        if msg == self.last_error:
            return
        self.last_error = msg
        self.error_var.set(msg)

    def _append_log_row(self, entry: LogEntry) -> None:
        self.log_tree.insert("", tk.END, values=(entry.ts, entry.kind, entry.message), tags=(entry.kind,))

        children = self.log_tree.get_children()
        if len(children) > self.MAX_LOG_ROWS:
            self.log_tree.delete(children[0])

        self.log_tree.yview_moveto(1.0)

    def _update_metrics_from_log(self, entry: LogEntry) -> None:
        self.metrics["total_events"] += 1
        if entry.kind == "CONVERT":
            self.metrics["converted_left"] += 1
        elif entry.kind == "BLOCK":
            self.metrics["blocked_right"] += 1
        elif entry.kind == "MOVE":
            self.metrics["move_events"] += 1
        elif entry.kind == "WHEEL":
            self.metrics["wheel_events"] += 1

        msg = entry.message
        if msg.startswith("LOGGING "):
            self.logging_state_var.set(f"Logging: {msg.split(' ', 1)[1]}")
        elif msg.startswith("CONNECTED"):
            self.module_state_var.set("Trạng thái module: Có thiết bị chuột đang kết nối")
        elif msg.startswith("DISCONNECTED"):
            self.module_state_var.set("Trạng thái module: Thiết bị chuột đã ngắt")

        for key, var in self.metric_vars.items():
            var.set(str(self.metrics[key]))

    def _parse_log_line(self, line: str) -> LogEntry | None:
        match = LOG_RE.match(line)
        if not match:
            return None

        sec_raw, ms_raw, message = match.groups()
        ts = f"{int(sec_raw):d}.{int(ms_raw):03d}"
        return LogEntry(ts=ts, kind=self._classify_log(message), message=message)

    def _parse_entropy_line(self, line: str) -> EntropyPoint | None:
        match = ENTROPY_RE.match(line)
        if not match:
            return None

        sec_raw, ms_raw, dx_raw, dy_raw = match.groups()
        ts = f"{int(sec_raw):d}.{int(ms_raw):03d}"
        return EntropyPoint(ts=ts, dx=int(dx_raw), dy=int(dy_raw))

    @staticmethod
    def _classify_log(message: str) -> str:
        if message.startswith("MOVE:"):
            return "MOVE"
        if "converted -> BTN_RIGHT" in message:
            return "CONVERT"
        if message.startswith("BTN_RIGHT blocked"):
            return "BLOCK"
        if message.startswith("WHEEL:"):
            return "WHEEL"
        if message.startswith("LOGGING "):
            return "LOGGING"
        if message.startswith("WARN:"):
            return "WARN"
        if message.startswith("CONNECTED"):
            return "CONNECT"
        if message.startswith("DISCONNECTED"):
            return "DISCONNECT"
        return "OTHER"

    @staticmethod
    def _remember_key(
        queue_obj: deque[tuple],
        set_obj: set[tuple],
        key: tuple,
    ) -> bool:
        if key in set_obj:
            return False

        if len(queue_obj) == queue_obj.maxlen:
            old = queue_obj.popleft()
            set_obj.discard(old)

        queue_obj.append(key)
        set_obj.add(key)
        return True

    def redraw_chart(self) -> None:
        canvas = self.chart_canvas
        canvas.delete("all")

        width = max(canvas.winfo_width(), 60)
        height = max(canvas.winfo_height(), 60)

        margin_x = 36
        margin_y = 24

        x0, y0 = margin_x, margin_y
        x1, y1 = width - margin_x, height - margin_y

        canvas.create_rectangle(x0, y0, x1, y1, outline="#9fb4c6", width=1)
        mid_y = (y0 + y1) / 2
        canvas.create_line(x0, mid_y, x1, mid_y, fill="#c8d7e4", dash=(3, 3))

        if len(self.entropy_points) < 2:
            canvas.create_text(
                width / 2,
                height / 2,
                text="Chưa đủ dữ liệu entropy để vẽ biểu đồ",
                fill="#466179",
                font=("TkDefaultFont", 11),
            )
            return

        values_dx = [p.dx for p in self.entropy_points]
        values_dy = [p.dy for p in self.entropy_points]
        max_abs = max(1, max(abs(v) for v in values_dx + values_dy))

        def map_point(index: int, value: int) -> tuple[float, float]:
            span_x = max(1, len(self.entropy_points) - 1)
            x = x0 + (x1 - x0) * (index / span_x)
            amplitude = (y1 - y0) * 0.42
            y = mid_y - (value / max_abs) * amplitude
            return x, y

        points_dx: list[float] = []
        points_dy: list[float] = []
        for i, p in enumerate(self.entropy_points):
            x_dx, y_dx = map_point(i, p.dx)
            x_dy, y_dy = map_point(i, p.dy)
            points_dx.extend((x_dx, y_dx))
            points_dy.extend((x_dy, y_dy))

        canvas.create_line(points_dx, fill="#c0392b", width=2, smooth=True)
        canvas.create_line(points_dy, fill="#1f78c1", width=2, smooth=True)

        canvas.create_text(x0 + 8, y0 + 10, text=f"+{max_abs}", anchor="w", fill="#3f5f77")
        canvas.create_text(x0 + 8, y1 - 10, text=f"-{max_abs}", anchor="w", fill="#3f5f77")


if __name__ == "__main__":
    try:
        app = MouseFilterDashboard()
        app.mainloop()
    except tk.TclError as exc:
        print("Không thể mở giao diện Tkinter.")
        print(f"Chi tiết: {exc}")
        print("Gợi ý: chạy trong môi trường có desktop/X11.")
