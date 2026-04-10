#!/usr/bin/env python3
import base64
import hashlib
import json
import os
import struct
import subprocess
import tempfile
import time
import tkinter as tk
import zipfile
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from cryptography.hazmat.primitives import hashes

ROOT_DIR = Path(__file__).resolve().parents[1]
CLI_PATH = ROOT_DIR / "app" / "crypto_mouse_cli"
KEYFILE_AAD = b"secure_usb_crypto:keyfile:v1"
MOUSE_ENTROPY_PROC = Path("/proc/mouse_entropy")


# Ham goi app CLI C hien co va tra ket qua stdout/stderr cho GUI xu ly.
def run_cli(args):
    cmd = [str(CLI_PATH)] + args
    return subprocess.run(cmd, capture_output=True, text=True)


# Ham parse dong status dang key=value key=value thanh dict de GUI de doc.
def parse_status_line(line):
    parts = line.strip().split()
    out = {}
    for part in parts:
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        out[k] = v
    return out


# Ham tao khoa KEK tu passphrase qua PBKDF2 de ma hoa key file an toan hon.
def derive_passphrase_key(passphrase, salt):
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(),
        length=32,
        salt=salt,
        iterations=200000,
    )
    return kdf.derive(passphrase.encode("utf-8"))


# Ham ma hoa key CAMELLIA bang AES-GCM va luu ra key file JSON.
def encrypt_keyfile(raw_key_hex, passphrase, out_path):
    raw_key = bytes.fromhex(raw_key_hex)
    salt = os.urandom(16)
    nonce = os.urandom(12)
    kek = derive_passphrase_key(passphrase, salt)
    ct = AESGCM(kek).encrypt(nonce, raw_key, KEYFILE_AAD)

    payload = {
        "version": 1,
        "kdf": "PBKDF2-HMAC-SHA256",
        "iterations": 200000,
        "cipher": "AES-256-GCM",
        "salt_b64": base64.b64encode(salt).decode("ascii"),
        "nonce_b64": base64.b64encode(nonce).decode("ascii"),
        "ciphertext_b64": base64.b64encode(ct).decode("ascii"),
    }

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)


# Ham mo key file JSON da ma hoa va giai ma lai key hex de decrypt file.
def decrypt_keyfile(keyfile_path, passphrase):
    with open(keyfile_path, "r", encoding="utf-8") as f:
        payload = json.load(f)

    salt = base64.b64decode(payload["salt_b64"])
    nonce = base64.b64decode(payload["nonce_b64"])
    ct = base64.b64decode(payload["ciphertext_b64"])

    kek = derive_passphrase_key(passphrase, salt)
    raw_key = AESGCM(kek).decrypt(nonce, ct, KEYFILE_AAD)
    return raw_key.hex()


# Ham kiem tra duong dan sysfs co thuoc USB VID/PID muc tieu hay khong.
def is_usb_vidpid_path(path_obj, vid_hex="1a81", pid_hex="101f"):
    cur = path_obj.resolve()
    for _ in range(12):
        vid_file = cur / "idVendor"
        pid_file = cur / "idProduct"
        if vid_file.exists() and pid_file.exists():
            try:
                vid = vid_file.read_text(encoding="utf-8").strip().lower()
                pid = pid_file.read_text(encoding="utf-8").strip().lower()
                return vid == vid_hex and pid == pid_hex
            except OSError:
                return False
        if cur.parent == cur:
            break
        cur = cur.parent
    return False


def has_rel_xy_cap(event_node):
    rel_file = event_node / "device" / "capabilities" / "rel"
    try:
        rel_mask = int(rel_file.read_text(encoding="utf-8").strip(), 16)
    except (OSError, ValueError):
        return False

    return (rel_mask & 0x3) == 0x3


def event_score(event_node):
    score = 0
    name_file = event_node / "device" / "name"
    try:
        name = name_file.read_text(encoding="utf-8").strip().lower()
    except OSError:
        name = ""

    if "mouse" in name:
        score += 2
    if has_rel_xy_cap(event_node):
        score += 3
    return score


# Ham tim event device /dev/input/eventX cua USB mouse dung VID/PID.
def find_usb_mouse_event_device():
    input_root = Path("/sys/class/input")
    candidates = []

    for event_node in sorted(input_root.glob("event*")):
        if is_usb_vidpid_path(event_node / "device"):
            candidates.append(event_node)

    if not candidates:
        return None

    # Uu tien interface chuot that su (co REL_X/REL_Y va ten chua "mouse").
    candidates.sort(key=event_score, reverse=True)
    return Path("/dev/input") / candidates[0].name


def parse_proc_entropy_line(line):
    # Dinh dang line: [sec.msec] dx=... dy=...
    raw = line.strip()
    if not raw.startswith("[") or "]" not in raw:
        return None

    ts_part, _, rest = raw.partition("]")
    ts_part = ts_part[1:]
    if "." not in ts_part:
        return None

    sec_s, msec_s = ts_part.split(".", 1)
    sec_s = sec_s.strip()
    msec_s = msec_s.strip()
    if not sec_s or not msec_s:
        return None

    vals = {}
    for token in rest.strip().split():
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        vals[k.strip()] = v.strip()

    if "dx" not in vals or "dy" not in vals:
        return None

    try:
        sec = int(sec_s)
        msec = int(msec_s)
        dx = int(vals["dx"])
        dy = int(vals["dy"])
    except ValueError:
        return None

    return sec, msec, dx, dy


def read_proc_entropy_samples():
    try:
        lines = MOUSE_ENTROPY_PROC.read_text(encoding="utf-8", errors="ignore").splitlines()
    except OSError:
        return None

    samples = []
    for line in lines:
        sample = parse_proc_entropy_line(line)
        if sample:
            samples.append(sample)
    return samples


# Ham lay entropy tu driver mouse_input_filter qua /proc/mouse_entropy trong 5 giay.
def collect_mouse_entropy_key_hex(duration_sec=5):
    if not MOUSE_ENTROPY_PROC.exists():
        raise RuntimeError(
            "Khong tim thay /proc/mouse_entropy. Hay nap module mouse_input_filter truoc"
        )

    baseline = read_proc_entropy_samples()
    if baseline is None:
        raise RuntimeError("Khong doc duoc /proc/mouse_entropy. Hay chay GUI bang sudo")

    seen = set(baseline)
    collected = []
    deadline = time.time() + duration_sec

    while time.time() < deadline:
        time.sleep(0.2)
        current = read_proc_entropy_samples()
        if current is None:
            continue
        for sample in current:
            if sample in seen:
                continue
            seen.add(sample)
            collected.append(sample)

    material = bytearray()
    for sec, msec, dx, dy in collected:
        usec = msec * 1000
        material.extend(struct.pack("<qiii", int(sec), int(usec), int(dx), int(dy)))

    if len(material) < 128:
        raise RuntimeError(
            "Khong du mau tu /proc/mouse_entropy. Hay di chuyen chuot trong luc tao key"
        )

    digest = hashlib.sha256(bytes(material)).digest()
    return digest[:16].hex()


# Ham gom danh sach target encrypt theo mode files/folder.
def collect_encrypt_targets(mode, selected_files, selected_folder):
    if mode == "files":
        return [Path(p) for p in selected_files]

    if mode == "folder":
        return [Path(selected_folder)] if selected_folder else []

    return []


# Ham gom danh sach target decrypt theo mode files/folder va chi lay file co duoi .enc.
def collect_decrypt_targets(mode, selected_files, selected_folder):
    targets = []
    if mode == "files":
        for p in selected_files:
            path = Path(p)
            if path.suffix == ".enc":
                targets.append(path)
        return targets

    if mode == "folder" and selected_folder:
        for root, _, files in os.walk(selected_folder):
            for name in files:
                p = Path(root) / name
                if p.suffix == ".enc":
                    targets.append(p)
    return targets


# Ham dat ten output khi encrypt: them duoi .enc.
def enc_output_path(in_path):
    return in_path.with_name(in_path.name + ".enc")


# Ham dat ten output khi decrypt: bo .enc neu co, nguoc lai them .dec.
def dec_output_path(in_path):
    if in_path.suffix == ".enc":
        return in_path.with_suffix("")
    return in_path.with_name(in_path.name + ".dec")


# Ham nen toan bo thu muc thanh mot file zip de ma hoa 1 lan.
def zip_folder_to_file(folder_path, zip_path):
    folder_path = Path(folder_path)
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in os.walk(folder_path):
            for name in files:
                src = Path(root) / name
                # Luu duong dan tu muc cha de giu ten thu muc goc khi giai nen.
                arcname = src.relative_to(folder_path.parent)
                zf.write(src, arcname=str(arcname))


# Ham giai nen file zip sau khi decrypt, tra ve thu muc da giai nen.
def extract_zip_to_folder(zip_path):
    zip_path = Path(zip_path)
    base_dir = zip_path.parent / zip_path.stem
    out_dir = base_dir
    idx = 1
    while out_dir.exists():
        out_dir = zip_path.parent / f"{base_dir.name}_extracted_{idx}"
        idx += 1

    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(out_dir)

    return out_dir


# Ham chon nhieu file theo kieu lap lai tung file, tranh phu thuoc multi-select native dialog.
def pick_files_one_by_one(parent, title, filetypes=None, must_enc=False):
    selected = []
    seen = set()

    while True:
        path = filedialog.askopenfilename(parent=parent, title=title, filetypes=filetypes)
        if not path:
            break

        p = Path(path)
        if must_enc and p.suffix != ".enc":
            messagebox.showwarning("Canh bao", f"Bo qua file khong phai .enc: {p}", parent=parent)
        else:
            key = str(p)
            if key not in seen:
                seen.add(key)
                selected.append(key)

        if not messagebox.askyesno("Chon tiep", "Ban co muon them file nua khong?", parent=parent):
            break

    return selected


class PassphraseDialog(tk.Toplevel):
    def __init__(self, parent, title_text):
        super().__init__(parent)
        self.title(title_text)
        self.resizable(False, False)
        self.result = None

        ttk.Label(self, text="Nhap passphrase:").grid(row=0, column=0, sticky="w", padx=10, pady=(10, 4))
        self.e1 = ttk.Entry(self, show="*")
        self.e1.grid(row=1, column=0, padx=10, pady=4, ipadx=80)

        ttk.Label(self, text="Nhap lai passphrase:").grid(row=2, column=0, sticky="w", padx=10, pady=(8, 4))
        self.e2 = ttk.Entry(self, show="*")
        self.e2.grid(row=3, column=0, padx=10, pady=4, ipadx=80)

        btn_row = ttk.Frame(self)
        btn_row.grid(row=4, column=0, pady=10)
        ttk.Button(btn_row, text="OK", command=self.on_ok).grid(row=0, column=0, padx=5)
        ttk.Button(btn_row, text="Cancel", command=self.on_cancel).grid(row=0, column=1, padx=5)

        self.bind("<Return>", lambda _: self.on_ok())
        self.bind("<Escape>", lambda _: self.on_cancel())

    def on_ok(self):
        p1 = self.e1.get()
        p2 = self.e2.get()
        if len(p1) < 6:
            messagebox.showerror("Loi", "Passphrase toi thieu 6 ky tu", parent=self)
            return
        if p1 != p2:
            messagebox.showerror("Loi", "Passphrase khong trung nhau", parent=self)
            return
        self.result = p1
        self.destroy()

    def on_cancel(self):
        self.result = None
        self.destroy()


class KeyUnlockDialog(tk.Toplevel):
    def __init__(self, parent):
        super().__init__(parent)
        self.title("Mo key file")
        self.resizable(False, False)
        self.result = None

        self.keyfile_var = tk.StringVar()

        ttk.Label(self, text="Key file (.key/.json):").grid(row=0, column=0, sticky="w", padx=10, pady=(10, 4))
        row1 = ttk.Frame(self)
        row1.grid(row=1, column=0, padx=10, pady=4, sticky="we")
        ttk.Entry(row1, textvariable=self.keyfile_var, width=44).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(row1, text="Browse", command=self.pick_keyfile).grid(row=0, column=1)

        ttk.Label(self, text="Passphrase:").grid(row=2, column=0, sticky="w", padx=10, pady=(8, 4))
        self.pass_entry = ttk.Entry(self, show="*", width=48)
        self.pass_entry.grid(row=3, column=0, padx=10, pady=4)

        btn_row = ttk.Frame(self)
        btn_row.grid(row=4, column=0, pady=10)
        ttk.Button(btn_row, text="OK", command=self.on_ok).grid(row=0, column=0, padx=5)
        ttk.Button(btn_row, text="Cancel", command=self.on_cancel).grid(row=0, column=1, padx=5)

    def pick_keyfile(self):
        path = filedialog.askopenfilename(
            title="Chon key file",
            filetypes=[("Key files", "*.key *.json"), ("All files", "*.*")],
        )
        if path:
            self.keyfile_var.set(path)

    def on_ok(self):
        keyfile = self.keyfile_var.get().strip()
        p = self.pass_entry.get().strip()
        if not keyfile or not p:
            messagebox.showerror("Loi", "Can key file va passphrase", parent=self)
            return
        self.result = (keyfile, p)
        self.destroy()

    def on_cancel(self):
        self.result = None
        self.destroy()


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Secure USB Crypto GUI")
        self.geometry("880x560")

        self.encrypt_mode = tk.StringVar(value="files")
        self.decrypt_mode = tk.StringVar(value="files")
        self.encrypt_files = []
        self.decrypt_files = []
        self.encrypt_folder = ""
        self.decrypt_folder = ""
        self.modify_target = ""
        self.modify_key_hex = ""
        self.modify_keyfile = ""
        self.generated_key_hex = ""
        self.active_key_path = ""
        self.last_output_dir = ""
        self.last_mouse_present = None

        self._build_ui()
        self.poll_mouse_status()

    def _build_ui(self):
        top = ttk.Frame(self, padding=10)
        top.pack(fill="x")

        ttk.Label(top, text="Driver CLI:").pack(side="left")
        ttk.Label(top, text=str(CLI_PATH)).pack(side="left", padx=(6, 16))
        ttk.Button(top, text="Open Last Output Folder", command=self.open_last_output_folder).pack(side="left", padx=8)
        self.mouse_status_label = tk.Label(top, text="USB Mouse: Dang kiem tra...", fg="#b36b00")
        self.mouse_status_label.pack(side="left", padx=12)
        notebook = ttk.Notebook(self)
        notebook.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.notebook = notebook

        enc_tab = ttk.Frame(notebook, padding=10)
        dec_tab = ttk.Frame(notebook, padding=10)
        mod_tab = ttk.Frame(notebook, padding=10)
        notebook.add(enc_tab, text="Encrypt")
        notebook.add(dec_tab, text="Decrypt")
        notebook.add(mod_tab, text="Modify")
        notebook.bind("<<NotebookTabChanged>>", self.on_tab_changed)

        self._build_encrypt_tab(enc_tab)
        self._build_decrypt_tab(dec_tab)
        self._build_modify_tab(mod_tab)

        progress_wrap = ttk.Frame(self, padding=(10, 0, 10, 6))
        progress_wrap.pack(fill="x")
        self.progress_label = ttk.Label(progress_wrap, text="Progress: idle")
        self.progress_label.pack(fill="x", pady=(0, 4))
        self.progress_var = tk.DoubleVar(value=0)
        self.progress_bar = ttk.Progressbar(progress_wrap, mode="determinate", variable=self.progress_var, maximum=100)
        self.progress_bar.pack(fill="x")

        self.log_box = tk.Text(self, height=7)
        self.log_box.pack(fill="both", padx=10, pady=(0, 10))

    # Ham khoi tao progress bar cho mot batch encrypt/decrypt.
    def start_progress(self, title, total):
        self.progress_var.set(0)
        self.progress_bar.configure(maximum=max(1, total))
        self.progress_label.config(text=f"{title}: 0/{total}")
        self.update_idletasks()

    # Ham cap nhat progress sau moi file de user thay tien trinh realtime.
    def step_progress(self, title, done, total, current_path=""):
        self.progress_var.set(done)
        tail = f" | {current_path}" if current_path else ""
        self.progress_label.config(text=f"{title}: {done}/{total}{tail}")
        self.update_idletasks()

    # Ham ket thuc progress va dua thanh trang thai idle.
    def finish_progress(self):
        self.progress_label.config(text="Progress: idle")
        self.progress_var.set(0)
        self.update_idletasks()

    # Ham mo thu muc output gan nhat de user xem file ket qua nhanh.
    def open_last_output_folder(self):
        if not self.last_output_dir:
            messagebox.showinfo("Thong bao", "Chua co output folder nao duoc tao")
            return

        out_dir = Path(self.last_output_dir)
        if not out_dir.exists():
            messagebox.showerror("Loi", f"Thu muc khong ton tai: {out_dir}")
            return

        try:
            subprocess.Popen(["xdg-open", str(out_dir)])
        except Exception as exc:
            messagebox.showerror("Loi", f"Khong mo duoc folder: {exc}")

    def _build_mode_picker(self, parent, var, on_change):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=(0, 8))
        ttk.Label(row, text="Scope:").pack(side="left")
        ttk.Radiobutton(row, text="File(s)", variable=var, value="files", command=on_change).pack(side="left", padx=8)
        ttk.Radiobutton(row, text="Folder recursive", variable=var, value="folder", command=on_change).pack(side="left", padx=8)

    # Ham reset danh sach target o tab encrypt khi doi scope/chuyen tab.
    def reset_encrypt_selection(self):
        self.encrypt_files = []
        self.encrypt_folder = ""
        self.refresh_encrypt_targets()

    # Ham reset danh sach target o tab decrypt khi doi scope/chuyen tab.
    def reset_decrypt_selection(self):
        self.decrypt_files = []
        self.decrypt_folder = ""
        self.refresh_decrypt_targets()

    # Ham xu ly doi scope Encrypt: reset target cu de tranh giu file khong mong muon.
    def on_encrypt_mode_change(self):
        self.reset_encrypt_selection()

    # Ham xu ly doi scope Decrypt: reset target cu de tranh giu file khong mong muon.
    def on_decrypt_mode_change(self):
        self.reset_decrypt_selection()

    # Ham xu ly doi tab Encrypt/Decrypt: reset danh sach da chon de user chon moi.
    def on_tab_changed(self, _event):
        self.reset_encrypt_selection()
        self.reset_decrypt_selection()

    def _build_modify_tab(self, parent):
        btns = ttk.Frame(parent)
        btns.pack(fill="x", pady=(0, 8))
        ttk.Button(btns, text="Select .enc Text File", command=self.pick_modify_target).pack(side="left")
        ttk.Button(btns, text="Load For Edit", command=self.load_modify_content).pack(side="left", padx=8)
        ttk.Button(btns, text="Save (Overwrite .enc)", command=self.save_modify_content).pack(side="left", padx=8)

        self.modify_target_label = ttk.Label(parent, text="Modify target: none")
        self.modify_target_label.pack(fill="x", pady=(0, 4))
        self.modify_key_label = ttk.Label(parent, text="Modify key: not loaded")
        self.modify_key_label.pack(fill="x", pady=(0, 8))

        self.modify_text = tk.Text(parent, height=18)
        self.modify_text.pack(fill="both", expand=True)

    def _build_encrypt_tab(self, parent):
        self._build_mode_picker(parent, self.encrypt_mode, self.on_encrypt_mode_change)

        btns = ttk.Frame(parent)
        btns.pack(fill="x", pady=(0, 8))
        ttk.Button(btns, text="Select Target", command=self.pick_encrypt_target).pack(side="left")
        ttk.Button(btns, text="Generate Key From USB Mouse", command=self.generate_key_from_mouse).pack(side="left", padx=8)
        ttk.Button(btns, text="Use Existing Key File", command=self.load_existing_key_for_encrypt).pack(
            side="left", padx=8
        )
        ttk.Button(btns, text="Encrypt", command=self.run_encrypt).pack(side="left", padx=8)

        self.enc_target_label = ttk.Label(parent, text="No target selected")
        self.enc_target_label.pack(fill="x", pady=(0, 4))

        self.key_label = ttk.Label(parent, text="Key status: not generated")
        self.key_label.pack(fill="x", pady=(0, 8))

        self.enc_list = tk.Listbox(parent, height=12)
        self.enc_list.pack(fill="both", expand=True)

    def _build_decrypt_tab(self, parent):
        self._build_mode_picker(parent, self.decrypt_mode, self.on_decrypt_mode_change)

        btns = ttk.Frame(parent)
        btns.pack(fill="x", pady=(0, 8))
        ttk.Button(btns, text="Select Target", command=self.pick_decrypt_target).pack(side="left")
        ttk.Button(btns, text="Decrypt (Using Key File)", command=self.run_decrypt).pack(side="left", padx=8)

        self.dec_target_label = ttk.Label(parent, text="No target selected")
        self.dec_target_label.pack(fill="x", pady=(0, 8))

        self.dec_list = tk.Listbox(parent, height=14)
        self.dec_list.pack(fill="both", expand=True)

    def log(self, msg):
        self.log_box.insert("end", msg + "\n")
        self.log_box.see("end")

    # Ham query trang thai mouse_present tu CLI, tra ve True/False/None neu loi.
    def query_mouse_present(self):
        if not CLI_PATH.exists():
            return None

        res = run_cli(["status"])
        if res.returncode != 0:
            return None

        status = parse_status_line(res.stdout.strip())
        return status.get("mouse_present") == "1"

    # Ham cap nhat nhan trang thai ket noi chuot tren GUI va log khi co thay doi.
    def refresh_mouse_status_label(self):
        mouse_present = self.query_mouse_present()

        if mouse_present is None:
            self.mouse_status_label.config(text="USB Mouse: Khong doc duoc status", fg="#aa0000")
            return

        if mouse_present:
            self.mouse_status_label.config(text="USB Mouse: Da ket noi", fg="#0a7a1f")
        else:
            self.mouse_status_label.config(text="USB Mouse: Chua ket noi", fg="#aa0000")

        if self.last_mouse_present is None:
            self.last_mouse_present = mouse_present
            return

        if mouse_present != self.last_mouse_present:
            if mouse_present:
                self.log("USB Mouse status changed: DA KET NOI")
            else:
                self.log("USB Mouse status changed: DA RUT")
            self.last_mouse_present = mouse_present

    # Ham polling dinh ky de GUI tu dong phan anh cam/rut chuot theo thoi gian thuc.
    def poll_mouse_status(self):
        self.refresh_mouse_status_label()
        self.after(1000, self.poll_mouse_status)

    def pick_encrypt_target(self):
        mode = self.encrypt_mode.get()
        if mode == "files":
            native_paths = filedialog.askopenfilenames(
                parent=self,
                title="Chon nhieu file can encrypt",
            )
            if native_paths:
                paths = list(native_paths)
            else:
                paths = pick_files_one_by_one(
                    parent=self,
                    title="Chon file can encrypt (chon tung file)",
                )
            if paths:
                # O mode files, moi lan bam Select Target se cong don them file moi.
                existing = set(self.encrypt_files)
                for p in paths:
                    if p not in existing:
                        self.encrypt_files.append(p)
                        existing.add(p)
                self.encrypt_folder = ""
        else:
            folder = filedialog.askdirectory(parent=self, title="Chon folder can encrypt")
            if folder:
                self.encrypt_folder = folder
                self.encrypt_files = []

        self.refresh_encrypt_targets()

    def refresh_encrypt_targets(self):
        self.enc_list.delete(0, "end")
        mode = self.encrypt_mode.get()
        targets = collect_encrypt_targets(mode, self.encrypt_files, self.encrypt_folder)
        for p in targets:
            self.enc_list.insert("end", str(p))

        if mode == "folder":
            self.enc_target_label.config(text=f"Folder: {self.encrypt_folder or 'none'}")
        else:
            self.enc_target_label.config(text=f"Files: {len(targets)} selected")

    def pick_decrypt_target(self):
        mode = self.decrypt_mode.get()
        if mode == "files":
            native_paths = filedialog.askopenfilenames(
                parent=self,
                title="Chon nhieu file .enc can decrypt",
                filetypes=[("Encrypted file", "*.enc"), ("All files", "*.*")],
            )
            if native_paths:
                paths = list(native_paths)
            else:
                paths = pick_files_one_by_one(
                    parent=self,
                    title="Chon file .enc can decrypt (chon tung file)",
                    filetypes=[("Encrypted file", "*.enc"), ("All files", "*.*")],
                    must_enc=True,
                )
            if paths:
                # O mode files, moi lan bam Select Target se cong don them file .enc moi.
                existing = set(self.decrypt_files)
                for p in paths:
                    if p.endswith(".enc") and p not in existing:
                        self.decrypt_files.append(p)
                        existing.add(p)
                self.decrypt_folder = ""
        else:
            folder = filedialog.askdirectory(parent=self, title="Chon folder co file .enc")
            if folder:
                self.decrypt_folder = folder
                self.decrypt_files = []

        self.refresh_decrypt_targets()

    def refresh_decrypt_targets(self):
        self.dec_list.delete(0, "end")
        mode = self.decrypt_mode.get()
        targets = collect_decrypt_targets(mode, self.decrypt_files, self.decrypt_folder)
        for p in targets:
            self.dec_list.insert("end", str(p))

        if mode == "folder":
            self.dec_target_label.config(text=f"Folder: {self.decrypt_folder or 'none'}")
        else:
            self.dec_target_label.config(text=f"Files(.enc): {len(targets)} selected")

    def pick_modify_target(self):
        path = filedialog.askopenfilename(
            parent=self,
            title="Chon file .enc can sua noi dung text",
            filetypes=[("Encrypted file", "*.enc"), ("All files", "*.*")],
        )
        if not path:
            return

        p = Path(path)
        if p.suffix != ".enc":
            messagebox.showwarning("Canh bao", "Chi ho tro file .enc")
            return

        if p.name.endswith(".zip.enc"):
            messagebox.showwarning(
                "Chua ho tro",
                "Tab Modify hien tai chi ho tro file text .enc don, chua ho tro .zip.enc",
            )
            return

        self.modify_target = str(p)
        self.modify_target_label.config(text=f"Modify target: {p}")

    # Ham decrypt file .enc text vao editor de user sua noi dung.
    def load_modify_content(self):
        if not CLI_PATH.exists():
            messagebox.showerror("Loi", "Khong tim thay crypto_mouse_cli. Hay build app truoc")
            return

        if not self.modify_target:
            messagebox.showwarning("Can target", "Hay chon file .enc can modify")
            return

        dlg = KeyUnlockDialog(self)
        self.wait_window(dlg)
        if not dlg.result:
            return

        keyfile, passphrase = dlg.result
        try:
            key_hex = decrypt_keyfile(keyfile, passphrase)
        except Exception as exc:
            messagebox.showerror("Loi key file", f"Khong mo duoc key file: {exc}")
            return

        fd, tmp_name = tempfile.mkstemp(prefix="modify_plain_", suffix=".txt")
        os.close(fd)
        tmp_plain = Path(tmp_name)
        try:
            res = run_cli(["decrypt-file", key_hex, self.modify_target, str(tmp_plain)])
            if res.returncode != 0:
                err = (res.stderr or res.stdout).strip()
                messagebox.showerror("Loi decrypt", err or "Khong decrypt duoc file")
                return

            try:
                text = tmp_plain.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                messagebox.showerror("Khong ho tro", "File giai ma khong phai text UTF-8")
                return

            self.modify_text.delete("1.0", "end")
            self.modify_text.insert("1.0", text)
            self.modify_key_hex = key_hex
            self.modify_keyfile = keyfile
            self.modify_key_label.config(text=f"Modify key: loaded -> {keyfile}")
            self.log(f"MODIFY LOAD OK: {self.modify_target}")
        finally:
            tmp_plain.unlink(missing_ok=True)

    # Ham luu noi dung text dang sua va ma hoa de ghi de file .enc cu.
    def save_modify_content(self):
        if not CLI_PATH.exists():
            messagebox.showerror("Loi", "Khong tim thay crypto_mouse_cli. Hay build app truoc")
            return

        if not self.modify_target:
            messagebox.showwarning("Can target", "Hay chon file .enc can modify")
            return

        if not self.modify_key_hex:
            messagebox.showwarning("Can key", "Hay Load For Edit truoc de nap key")
            return

        content = self.modify_text.get("1.0", "end-1c")

        fd, tmp_name = tempfile.mkstemp(prefix="modify_save_", suffix=".txt")
        os.close(fd)
        tmp_plain = Path(tmp_name)
        try:
            tmp_plain.write_text(content, encoding="utf-8")

            res = run_cli(["encrypt-file", self.modify_key_hex, str(tmp_plain), self.modify_target])
            if res.returncode != 0:
                err = (res.stderr or res.stdout).strip()
                messagebox.showerror("Loi encrypt", err or "Khong encrypt duoc file")
                self.log(f"MODIFY SAVE FAIL: {self.modify_target}")
                return

            self.last_output_dir = str(Path(self.modify_target).parent)
            self.log(f"MODIFY SAVE OK: overwrite {self.modify_target}")
            messagebox.showinfo("Thanh cong", "Da luu va ma hoa de file .enc cu")
        finally:
            tmp_plain.unlink(missing_ok=True)

    # Ham tao key hex random dua tren entropy tu du lieu di chuyen chuot USB.
    def generate_key_from_mouse(self):
        if not CLI_PATH.exists():
            messagebox.showerror("Loi", "Khong tim thay crypto_mouse_cli. Hay build app truoc")
            return

        self.log("Dang thu key ngau nhien tu /proc/mouse_entropy trong 5s... hay di chuyen chuot")
        self.update_idletasks()

        try:
            key_hex = collect_mouse_entropy_key_hex(duration_sec=5)
        except Exception as exc:
            messagebox.showerror("Loi tao key", str(exc))
            return

        dlg = PassphraseDialog(self, "Dat passphrase cho secret.key")
        self.wait_window(dlg)
        if not dlg.result:
            return

        key_path = filedialog.asksaveasfilename(
            title="Luu key file",
            defaultextension=".key",
            initialfile="secret.key",
            filetypes=[("Key file", "*.key"), ("JSON", "*.json"), ("All", "*.*")],
        )
        if not key_path:
            return

        try:
            encrypt_keyfile(key_hex, dlg.result, key_path)
        except Exception as exc:
            messagebox.showerror("Loi luu key file", str(exc))
            return

        self.generated_key_hex = key_hex
        self.active_key_path = key_path
        self.key_label.config(text=f"Key status: generated and saved -> {key_path}")
        self.log(f"Key generated from USB mouse and saved: {key_path}")
        messagebox.showinfo("Thanh cong", "Da tao va ma hoa key file thanh cong")

    # Ham nap lai key file cu de dung cho encrypt sau khi mo lai GUI.
    def load_existing_key_for_encrypt(self):
        if not CLI_PATH.exists():
            messagebox.showerror("Loi", "Khong tim thay crypto_mouse_cli. Hay build app truoc")
            return

        dlg = KeyUnlockDialog(self)
        self.wait_window(dlg)
        if not dlg.result:
            return

        keyfile, passphrase = dlg.result
        try:
            key_hex = decrypt_keyfile(keyfile, passphrase)
        except Exception as exc:
            messagebox.showerror("Loi key file", f"Khong mo duoc key file: {exc}")
            return

        self.generated_key_hex = key_hex
        self.active_key_path = keyfile
        self.key_label.config(text=f"Key status: loaded from existing key file -> {keyfile}")
        self.log(f"Key loaded for encrypt: {keyfile}")
        messagebox.showinfo("Thanh cong", "Da nap key file cu, san sang encrypt")

    # Ham encrypt theo batch va cap nhat tien trinh theo tung file.
    def run_encrypt(self):
        if not self.generated_key_hex:
            if messagebox.askyesno(
                "Can key",
                "Chua co key dang hoat dong. Ban co muon nap key file cu de encrypt khong?",
            ):
                self.load_existing_key_for_encrypt()

            if not self.generated_key_hex:
                return

        mode = self.encrypt_mode.get()
        targets = collect_encrypt_targets(mode, self.encrypt_files, self.encrypt_folder)
        if not targets:
            messagebox.showwarning("Can target", "Hay chon file/folder can encrypt")
            return

        ok = 0
        skipped = 0
        failed = []
        total = len(targets)

        self.start_progress("Encrypt", total)

        for idx, in_path in enumerate(targets, start=1):
            self.step_progress("Encrypt", idx, total, str(in_path))

            if mode == "folder":
                if not in_path.exists() or not in_path.is_dir():
                    failed.append((str(in_path), "Folder khong ton tai hoac khong hop le"))
                    self.log(f"ENCRYPT FAIL: {in_path}")
                    continue

                tmp_zip = None
                try:
                    fd, tmp_name = tempfile.mkstemp(prefix="secure_usb_crypto_", suffix=".zip")
                    os.close(fd)
                    tmp_zip = Path(tmp_name)
                    zip_folder_to_file(in_path, tmp_zip)

                    out_path = in_path.parent / f"{in_path.name}.zip.enc"
                    res = run_cli(["encrypt-file", self.generated_key_hex, str(tmp_zip), str(out_path)])
                    if res.returncode == 0:
                        ok += 1
                        self.last_output_dir = str(out_path.parent)
                        self.log(f"ENCRYPT OK: {in_path} -> {out_path}")
                    else:
                        err = (res.stderr or res.stdout).strip()
                        failed.append((str(in_path), err))
                        self.log(f"ENCRYPT FAIL: {in_path}")
                finally:
                    if tmp_zip and tmp_zip.exists():
                        tmp_zip.unlink(missing_ok=True)
                continue

            if in_path.suffix == ".enc":
                skipped += 1
                continue

            out_path = enc_output_path(in_path)
            res = run_cli(["encrypt-file", self.generated_key_hex, str(in_path), str(out_path)])
            if res.returncode == 0:
                ok += 1
                self.last_output_dir = str(out_path.parent)
                self.log(f"ENCRYPT OK: {in_path} -> {out_path}")
            else:
                err = (res.stderr or res.stdout).strip()
                failed.append((str(in_path), err))
                self.log(f"ENCRYPT FAIL: {in_path}")

        summary = f"Encrypt done. success={ok}, skipped={skipped}, failed={len(failed)}"
        self.finish_progress()
        self.log(summary)

        if failed:
            detail = "\n".join([f"- {p}: {e}" for p, e in failed[:8]])
            messagebox.showwarning("Hoan tat co loi", summary + "\n" + detail)
        else:
            messagebox.showinfo("Hoan tat", summary)

    # Ham decrypt theo batch, loi file nao chi dung file do va van tiep tuc file khac.
    def run_decrypt(self):
        if not CLI_PATH.exists():
            messagebox.showerror("Loi", "Khong tim thay crypto_mouse_cli. Hay build app truoc")
            return

        targets = collect_decrypt_targets(self.decrypt_mode.get(), self.decrypt_files, self.decrypt_folder)
        if not targets:
            messagebox.showwarning("Can target", "Hay chon file .enc can decrypt")
            return

        dlg = KeyUnlockDialog(self)
        self.wait_window(dlg)
        if not dlg.result:
            return

        keyfile, passphrase = dlg.result
        try:
            key_hex = decrypt_keyfile(keyfile, passphrase)
        except Exception as exc:
            messagebox.showerror("Loi key file", f"Khong mo duoc key file: {exc}")
            return

        ok = 0
        failed = []
        total = len(targets)

        self.start_progress("Decrypt", total)

        for idx, in_path in enumerate(targets, start=1):
            self.step_progress("Decrypt", idx, total, str(in_path))

            out_path = dec_output_path(in_path)
            res = run_cli(["decrypt-file", key_hex, str(in_path), str(out_path)])
            if res.returncode == 0:
                if in_path.name.endswith(".zip.enc") and out_path.suffix == ".zip":
                    try:
                        extracted_dir = extract_zip_to_folder(out_path)
                        self.log(f"UNZIP OK: {out_path} -> {extracted_dir}")
                    except Exception as exc:
                        failed.append((str(in_path), f"Decrypt ok nhung unzip loi: {exc}"))
                        self.log(f"DECRYPT FAIL: {in_path}")
                        continue
                ok += 1
                self.last_output_dir = str(out_path.parent)
                self.log(f"DECRYPT OK: {in_path} -> {out_path}")
            else:
                err = (res.stderr or res.stdout).strip()
                failed.append((str(in_path), err))
                self.log(f"DECRYPT FAIL: {in_path}")

        summary = f"Decrypt done. success={ok}, failed={len(failed)}"
        self.finish_progress()
        self.log(summary)

        if failed:
            detail = "\n".join([f"- {p}: {e}" for p, e in failed[:8]])
            messagebox.showwarning("Hoan tat co loi", summary + "\n" + detail)
        else:
            messagebox.showinfo("Hoan tat", summary)


def main():
    if not CLI_PATH.exists():
        print("Khong tim thay app/crypto_mouse_cli. Hay build app truoc: make -C app")
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()
