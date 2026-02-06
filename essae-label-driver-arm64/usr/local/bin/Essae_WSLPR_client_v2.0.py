#!/usr/bin/env python3

import sys
import os
import socket
import threading
import time
import sqlite3
import json

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QPushButton,
    QVBoxLayout, QFormLayout, QLabel, QFileDialog, QTextEdit,
    QAction, QStatusBar, QLineEdit, QHBoxLayout,
    QTabWidget, QGridLayout, QSplitter, QMessageBox,
    QComboBox, QInputDialog, QDialog, QDialogButtonBox,
    QListWidget, QListWidgetItem
)
from PyQt5.QtGui import QFont, QColor
from PyQt5.QtCore import Qt, pyqtSignal

# --- Version reader ---
def get_driver_version():
    import subprocess
    try:
        output = subprocess.check_output(['/usr/local/bin/Essae_WSLPR_server_v2.0', '--version'])
        return output.decode().strip()
    except Exception as e:
        return f"Error: {e}"


# Constants
PORT = 8888
DEFAULT_HOST = '0.0.0.0'
POLL_INTERVAL = 1.0       # seconds between raw polls normally
CAL_POLL_INTERVAL = 0.2   # seconds between calibration raw polls
RECV_TIMEOUT = 2.0        # socket recv timeout
#DB_PATH = 'SQL_LFT_Files.db'
DB_PATH = "/usr/local/bin/SQL_LFT_Files.db"
MAX_SLOTS = 99

class LFTEditorDialog(QDialog):
    def __init__(self, name, content, save_callback, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"Edit LFT: {name}")
        self.resize(600, 400)
        self.save_callback = save_callback
        layout = QVBoxLayout(self)
        self.text = QTextEdit(self)
        self.text.setPlainText(content.decode('utf-8', errors='ignore'))
        layout.addWidget(self.text)
        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._save)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _save(self):
        new_content = self.text.toPlainText().encode('utf-8')
        self.save_callback(new_content)
        self.accept()

class BarcodeEditorDialog(QDialog):
    def __init__(self, data=None, save_callback=None, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Barcode Editor")
        self.resize(500, 400)
        self.save_callback = save_callback
        layout = QFormLayout(self)
        self.fields = {}
        labels = [
            "barcode_number", "barcode_name", "barcode_type", "barcode_data",
            "barcode_fld1", "fld1_condition", "fld1_shift",
            "barcode_fld2", "fld2_condition", "fld2_shift"
        ]
        for key in labels:
            entry = QLineEdit()
            layout.addRow(key + ":", entry)
            self.fields[key] = entry
        if data:
            for key in data:
                if key in self.fields:
                    self.fields[key].setText(str(data[key]))
        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._save)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _save(self):
        result = {k: self.fields[k].text().strip() for k in self.fields}
        self.save_callback(result)
        self.accept()

class LabelAndScaleGUI(QMainWindow):
    scale_response = pyqtSignal(str, str)
    raw_data       = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.setWindowTitle("📋 Label Printer & ⚖️ Weighing Scale")
        self.resize(950, 550)
        self.setMinimumSize(800, 450)
        self.version_label = QLabel("Driver Version: unknown")

        # Networking state
        self.server_host = DEFAULT_HOST
        self.connected   = False
        self.polling     = False

        # Initialize database
        self._init_db()

        # Connect signals
        self.scale_response.connect(self._on_scale_response)
        self.raw_data.connect(self._on_raw_data)

        # Build UI
        self._create_menu()
        self._create_main_layout()
        self._create_statusbar()
        self._apply_styles()
        
    def _init_db(self):
        self.conn = sqlite3.connect(DB_PATH)
        c = self.conn.cursor()
        c.execute("""
            CREATE TABLE IF NOT EXISTS lft_files (
                slot INTEGER PRIMARY KEY,
                name TEXT UNIQUE,
                content BLOB
            )
        """
        )
        c.execute("""
        CREATE TABLE IF NOT EXISTS barcode_templates (
        barcode_number INTEGER PRIMARY KEY,
        barcode_name TEXT,
        barcode_type TEXT,
        barcode_data TEXT,
        barcode_fld1 TEXT,
        fld1_condition TEXT,
        fld1_shift TEXT,
        barcode_fld2 TEXT,
        fld2_condition TEXT,
        fld2_shift TEXT
        )
    """)
        self.conn.commit()

    def _create_menu(self):
        mb = self.menuBar()
        fm = mb.addMenu("&File")
        exit_act = QAction("E&xit", self)
        exit_act.setShortcut("Ctrl+Q")
        exit_act.triggered.connect(self.close)
        fm.addAction(exit_act)

        hm = mb.addMenu("&Help")
        about = QAction("&About", self)
        about.triggered.connect(lambda: self.statusBar().showMessage("Label+Scale GUI v2.0"))
        hm.addAction(about)

    def _on_scale_response(self, cmd, response):
       print(f"Scale response: {cmd} → {response}")

    def _on_raw_data(self, data):
       print(f"Raw scale data: {data}")
    
    def print_label(self):
        if not self.connected:
            self.log.append("⚠️ Error: Not connected.")
            return

        if not hasattr(self, 'json_path'):
            self.log.append("⚠️ Error: Select JSON file first.")
            return

        slot = self.slot_select.currentData()
        if not slot:
            self.log.append("⚠️ Error: Select an LFT slot first.")
            return

        try:
            # Check if LFT slot exists
            c = self.conn.cursor()
            c.execute('SELECT content FROM lft_files WHERE slot=?', (slot,))
            row = c.fetchone()
            if not row:
                self.log.append("⚠️ Error: Selected LFT slot is empty.")
                return

            # Barcode number from dropdown
            barcode_number = self.barcode_select.currentData()
            if barcode_number is None:
                self.log.append("⚠️ Error: Select a barcode number.")
                return

            # ✅ Now send JSON path, slot, and barcode number to server
            packet = (
                f"MODE:PRINTER\n"
                f"{self.json_path}\n"
                f"{slot}\n"
                f"{barcode_number}\n"
            ).encode()

            # Send to TCP server
            with socket.create_connection((self.server_host, PORT), timeout=5) as s:
                s.sendall(packet)
                resp = s.recv(1024).decode().strip()
                self.log.append(f"🖨️ Printer → {resp}")
        except Exception as e:
            self.log.append(f"❌ Print error: {e}")


    def _create_main_layout(self):
        container = QWidget(self)
        self.setCentralWidget(container)
        splitter = QSplitter(Qt.Horizontal)
        layout = QVBoxLayout(container)
        layout.addWidget(splitter)

        # Label Printer Pane
        left = QWidget()
        left_layout = QVBoxLayout(left)
        # Connection row
        conn_row = QHBoxLayout()
        self.ip_input = QLineEdit(DEFAULT_HOST)
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.toggle_connection)
        conn_row.addWidget(self.ip_input)
        conn_row.addWidget(self.connect_btn)
        left_layout.addLayout(conn_row)
        # Title
        title = QLabel("Label Printer")
        self.version_label.setAlignment(Qt.AlignCenter)
        left_layout.addWidget(self.version_label)
        title.setFont(QFont("Roboto", 18, QFont.Bold))
        title.setAlignment(Qt.AlignCenter)
        left_layout.addWidget(title)
        # LFT slot + JSON
        form = QFormLayout()
        self.slot_select = QComboBox()
        # Show full selected text clearly
        self.slot_select.setEditable(True)
        self.slot_select.lineEdit().setReadOnly(True)
        self.slot_select.setSizeAdjustPolicy(QComboBox.AdjustToContents)
        self._refresh_slots()
        self.slot_select.setStyleSheet("QComboBox { background: white; }")
        form.addRow("Select LFT Slot:", self.slot_select)
        btn_add = QPushButton("Add LFT to SQL")
        btn_add.clicked.connect(self._add_lft)
        # REMOVED: Load LFT Slot to Printer button
        btn_edit = QPushButton("Edit LFT File Slot")
        btn_edit.clicked.connect(self._edit_lft)
        btn_del = QPushButton("Delete LFT File Slot")
        btn_del.clicked.connect(self._delete_lft)
        btn_row = QHBoxLayout()
        for b in (btn_add, btn_edit, btn_del): btn_row.addWidget(b)
        form.addRow(btn_row)
        
        # ─── Barcode Template Section ───
        # Barcode list widget
        self.barcode_list = QListWidget()
        self.barcode_list.setFixedHeight(150)
        self._refresh_barcodes()
        form.addRow("Barcodes:", self.barcode_list)
        
        # Barcode management buttons
        barcode_btn_row = QHBoxLayout()
        self.btn_add_barcode = QPushButton("Add Barcode")
        self.btn_add_barcode.clicked.connect(self._add_barcode)
        self.btn_edit_barcode = QPushButton("Edit Selected")
        self.btn_edit_barcode.clicked.connect(self._edit_barcode)
        self.btn_edit_barcode.setEnabled(False)
        self.btn_del_barcode = QPushButton("Delete Selected")
        self.btn_del_barcode.clicked.connect(self._delete_barcode)
        self.btn_del_barcode.setEnabled(False)
        
        barcode_btn_row.addWidget(self.btn_add_barcode)
        barcode_btn_row.addWidget(self.btn_edit_barcode)
        barcode_btn_row.addWidget(self.btn_del_barcode)
        form.addRow(barcode_btn_row)
        
        # Connect selection change signal
        self.barcode_list.itemSelectionChanged.connect(self._on_barcode_selection_changed)
        
        # JSON browse
        btn_json = QPushButton("Browse JSON")
        btn_json.clicked.connect(self.select_json)
        self.json_label = QLabel("none")
        form.addRow(btn_json, self.json_label)
        # ─── New: Select which barcode entry to print (1–99) ───
        # ─── Enhanced: Select Barcode Number (Styled) ───
        barcode_container = QHBoxLayout()

        barcode_label = QLabel("Select Barcode Number #:")
        barcode_label.setFont(QFont("Segoe UI", 10, QFont.Bold))
        barcode_label.setStyleSheet("color: #003D4C;")

        self.barcode_select = QComboBox()
        self.barcode_select.setFixedHeight(30)
        self.barcode_select.setFixedWidth(150)
        self.barcode_select.setStyleSheet("""
            QComboBox {
                background-color: #68def2;
                border: 2px solid #4dbebf;
                border-radius: 6px;
                padding: 4px;
                font-weight: bold;
                color: #003D4C;
            }
            QComboBox::drop-down {
                subcontrol-origin: padding;
                subcontrol-position: top right;
                width: 20px;
                border-left-width: 1px;
                border-left-color: #00CED1;
                border-left-style: solid;
            }
            QComboBox QAbstractItemView {
                background: #ffffff;
                selection-background-color: #00CED1;
                selection-color: #002B36;
                border-radius: 4px;
            }
        """)
        for i in range(1, MAX_SLOTS + 1):
            self.barcode_select.addItem(f"{i:02}", i)

        barcode_container.addWidget(barcode_label)
        barcode_container.addWidget(self.barcode_select)
        form.addRow(barcode_container)
        # ─────────────────────────────────────────────

        # ────────────────────────────────────────────────────────
        
        left_layout.addLayout(form)
        # Print + debug label
        self.lft_label = QLabel("Select an LFT slot to print")
        left_layout.addWidget(self.lft_label)
        self.print_btn = QPushButton("Print Label")
        self.print_btn.setEnabled(False)
        self.print_btn.clicked.connect(self.print_label)
        left_layout.addWidget(self.print_btn)
        self.log = QTextEdit()
        self.log.setReadOnly(True)
        left_layout.addWidget(self.log)
        splitter.addWidget(left)
        splitter.setStretchFactor(0, 2)

        # Weighing Scale Pane
        right = QWidget()
        rl = QVBoxLayout(right)
        header = QLabel("Weighing Scale")
        header.setFont(QFont("Roboto", 18, QFont.Bold))
        header.setAlignment(Qt.AlignCenter)
        rl.addWidget(header)
        self.tabs = QTabWidget()
        self._init_normal_tab()
        self._init_calibration_tab()
        self._init_tech_tab()
        rl.addWidget(self.tabs)
        splitter.addWidget(right)
        splitter.setStretchFactor(1, 3)
        
    def _on_barcode_selection_changed(self):
        selected = self.barcode_list.currentItem()
        self.btn_edit_barcode.setEnabled(selected is not None)
        self.btn_del_barcode.setEnabled(selected is not None)

    # LFT slot methods
    def _refresh_slots(self):
        c = self.conn.cursor()
        c.execute('SELECT slot, name FROM lft_files ORDER BY slot')
        items = c.fetchall()
        self.slot_select.clear()
        for slot, name in items:
            self.slot_select.addItem(f"{slot}: {name}", slot)
            
    def _refresh_barcodes(self):
        self.barcode_list.clear()
        c = self.conn.cursor()
        c.execute("SELECT barcode_number, barcode_name FROM barcode_templates ORDER BY barcode_number")
        barcodes = c.fetchall()
        for num, name in barcodes:
            item = QListWidgetItem(f"{num}: {name}")
            item.setData(Qt.UserRole, num)
            self.barcode_list.addItem(item)

    def _add_lft(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select LFT to Add", "", "LFT Files (*.LFT *.lft)")
        if not path: return
        name = os.path.basename(path)
        content = open(path, 'rb').read()
        slot, ok = QInputDialog.getInt(self, "Slot Number", f"Choose slot (1-{MAX_SLOTS}):", 1, 1, MAX_SLOTS)
        if not ok: return
        try:
            c = self.conn.cursor()
            c.execute('INSERT INTO lft_files (slot, name, content) VALUES (?, ?, ?)', (slot, name, content))
            self.conn.commit()
            QMessageBox.information(self, "Add LFT", "File added successfully.")
            self._refresh_slots()
        except sqlite3.IntegrityError as e:
            QMessageBox.warning(self, "Add LFT", f"Error: {e}")

    # REMOVED: _load_lft method

    def _edit_lft(self):
        slot = self.slot_select.currentData()
        if not slot:
            QMessageBox.warning(self, "Edit LFT", "Select a slot first.")
            return
            
        # load blob for dialog
        c = self.conn.cursor()
        c.execute('SELECT content, name FROM lft_files WHERE slot=?', (slot,))
        row = c.fetchone()
        if row:
            dlg = LFTEditorDialog(row[1], row[0], save_callback=self._save_edited_lft, parent=self)
            dlg.exec_()
        else:
            QMessageBox.warning(self, "Edit LFT", "Selected slot is empty.")

    def _save_edited_lft(self, new_blob):
        slot = self.slot_select.currentData()
        c = self.conn.cursor()
        c.execute('UPDATE lft_files SET content=? WHERE slot=?', (new_blob, slot))
        self.conn.commit()
        QMessageBox.information(self, "Edit LFT", "Changes saved to slot.")
        self._refresh_slots()

    def _add_barcode(self):
        def save(data):
            try:
                c = self.conn.cursor()
                c.execute("""
                    INSERT INTO barcode_templates (
                        barcode_number, barcode_name, barcode_type, barcode_data,
                        barcode_fld1, fld1_condition, fld1_shift,
                        barcode_fld2, fld2_condition, fld2_shift
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """, (
                    int(data["barcode_number"]), data["barcode_name"], data["barcode_type"], data["barcode_data"],
                    data["barcode_fld1"], data["fld1_condition"], data["fld1_shift"],
                    data["barcode_fld2"], data["fld2_condition"], data["fld2_shift"]
                ))
                self.conn.commit()
                QMessageBox.information(self, "Barcode", "Added successfully.")
                self._refresh_barcodes()
            except Exception as e:
                QMessageBox.warning(self, "Barcode", f"Failed: {e}")

        dlg = BarcodeEditorDialog(save_callback=save, parent=self)
        dlg.exec_()

    def _edit_barcode(self):
        selected_item = self.barcode_list.currentItem()
        if not selected_item:
            return
            
        num = selected_item.data(Qt.UserRole)
        c = self.conn.cursor()
        c.execute("SELECT * FROM barcode_templates WHERE barcode_number=?", (num,))
        row = c.fetchone()
        if not row:
            QMessageBox.warning(self, "Edit Barcode", "Not found.")
            return

        keys = [d[0] for d in c.description]
        data = dict(zip(keys, row))

        def save(new_data):
            c.execute("""
                UPDATE barcode_templates SET
                    barcode_name=?, barcode_type=?, barcode_data=?,
                    barcode_fld1=?, fld1_condition=?, fld1_shift=?,
                    barcode_fld2=?, fld2_condition=?, fld2_shift=?
                WHERE barcode_number=?
            """, (
                new_data["barcode_name"], new_data["barcode_type"], new_data["barcode_data"],
                new_data["barcode_fld1"], new_data["fld1_condition"], new_data["fld1_shift"],
                new_data["barcode_fld2"], new_data["fld2_condition"], new_data["fld2_shift"],
                num
            ))
            self.conn.commit()
            QMessageBox.information(self, "Barcode", "Updated.")
            self._refresh_barcodes()

        dlg = BarcodeEditorDialog(data, save_callback=save, parent=self)
        dlg.exec_()

    def _delete_barcode(self):
        selected_item = self.barcode_list.currentItem()
        if not selected_item:
            return
            
        num = selected_item.data(Qt.UserRole)
        resp = QMessageBox.question(self, "Delete Barcode", f"Delete barcode #{num}?")
        if resp == QMessageBox.Yes:
            c = self.conn.cursor()
            c.execute("DELETE FROM barcode_templates WHERE barcode_number=?", (num,))
            self.conn.commit()
            self._refresh_barcodes()
            QMessageBox.information(self, "Barcode", f"Deleted barcode #{num}")

    def _delete_lft(self):
        slot = self.slot_select.currentData()
        if not slot:
            QMessageBox.warning(self, "Delete LFT", "Select a slot first.")
            return
            
        resp = QMessageBox.question(self, "Delete LFT", f"Delete slot {slot}? ")
        if resp == QMessageBox.Yes:
            c = self.conn.cursor()
            c.execute('DELETE FROM lft_files WHERE slot=?', (slot,))
            self.conn.commit()
            self._refresh_slots()
            self.lft_label.setText("Select an LFT slot to print")

    def _init_normal_tab(self):
        w = QWidget()
        g = QGridLayout(w)
        g.addWidget(QLabel("Current Weight:"), 0, 0)
        self.weight_display = QLineEdit("0")
        self.weight_display.setReadOnly(True)
        g.addWidget(self.weight_display, 0, 1)
        buttons = [
            ("Read Weight", "RD_WEIGHT"),
            ("Tare",       "XC_TARE"),
            ("Re-Zero",    "XC_REZERO"),
            ("Restart-Scale",    "XC_RESTART"),
        ]
        self.norm_btns = []
        for i, (lbl, cmd) in enumerate(buttons):
            b = QPushButton(lbl)
            b.clicked.connect(lambda _, c=cmd: self._threaded_scale_cmd(c))
            self.norm_btns.append(b)
            g.addWidget(b, 1 + i//2, i%2)
        self.normal_log = QTextEdit()
        self.normal_log.setReadOnly(True)
        g.addWidget(self.normal_log, 3, 0, 1, 2)
        self.tabs.addTab(w, "Normal")

    def _init_calibration_tab(self):
        w = QWidget()
        g = QGridLayout(w)
        self.cal1 = QPushButton("1. Start Calibration 'XC_SON'")
        self.cal1.clicked.connect(self._start_calibration)
        g.addWidget(self.cal1, 0, 0, 1, 2)
        g.addWidget(QLabel("2. Weight (kg):"), 1, 0)
        self.cal_weight = QLineEdit("2")
        g.addWidget(self.cal_weight, 1, 1)
        self.cal2 = QPushButton("2. Set Calibration Weight 'XC_KEYCAL'")
        self.cal2.clicked.connect(self._set_cal_weight)
        g.addWidget(self.cal2, 2, 0, 1, 2)
        self.cal3 = QPushButton("3. Zero the Scale 'XC_CALZERO'")
        self.cal3.clicked.connect(self._zero_scale)
        g.addWidget(self.cal3, 3, 0, 1, 2)
        self.cal4 = QPushButton("4. Span Calibration 'XC_CALSPAN'")
        self.cal4.clicked.connect(self._span_calibration)
        g.addWidget(self.cal4, 4, 0, 1, 2)
        self.cal5 = QPushButton("5. Finalize Calibration 'XC_CALIBRATE'")
        self.cal5.clicked.connect(self._finalize_calibration)
        g.addWidget(self.cal5, 5, 0, 1, 2)
        self.cal6 = QPushButton("Read Raw Count")
        self.cal6.setCheckable(True)
        self.cal6.toggled.connect(self._toggle_raw_poll)
        g.addWidget(self.cal6, 6, 0, 1, 2)
        self.raw_log = QTextEdit()
        self.raw_log.setReadOnly(True)
        self.raw_log.setMaximumHeight(120)
        g.addWidget(self.raw_log, 7, 0, 1, 2)
        self.tabs.addTab(w, "Calibration")

    def _init_tech_tab(self):
        w = QWidget()
        g = QGridLayout(w)
        buttons = [
            ("Write Tech Spec",   "WR_TECHSPEC"),
            ("Read Tech Spec",    "RD_TECHSPEC"),
            ("Write Custom Spec", "WR_CUSSPEC"),
            ("Read Custom Spec",  "RD_CUSSPEC"),
        ]
        self.tech_btns = []
        for i, (lbl, cmd) in enumerate(buttons):
            b = QPushButton(lbl)
            b.clicked.connect(lambda _, c=cmd: self._threaded_scale_cmd(c))
            self.tech_btns.append(b)
            g.addWidget(b, i//2, i%2)
        self.tech_log = QTextEdit()
        self.tech_log.setReadOnly(True)
        g.addWidget(self.tech_log, 2, 0, 1, 2)
        self.tabs.addTab(w, "Tech Specs")


    def _start_calibration(self):
        self._threaded_scale_cmd("XC_SON", start_poll=True)
        QMessageBox.information(self, "Calibration", "Calibration mode initiated.")

    def _set_cal_weight(self):
        try:
            grams = int(float(self.cal_weight.text()) * 1000)
            cmd = f"XC_KEYCAL{grams:06d}"
            self._threaded_scale_cmd(cmd)
            QMessageBox.information(self, "Calibration", f"Set calibration weight: {self.cal_weight.text()} kg")
        except ValueError:
            QMessageBox.warning(self, "Calibration", "Invalid weight entered.")

    def _zero_scale(self):
        self._threaded_scale_cmd("XC_CALZERO")
        QMessageBox.information(self, "Calibration", "Entering CALZERO...")

    def _span_calibration(self):
        # Send span calibration command
        self._threaded_scale_cmd("XC_CALSPAN")
        # Inform user of next step
        QMessageBox.information(
            self,
            "Calibration",
            "Entering CALSPAN..."
        )
        QMessageBox.information(
            self,
            "Calibration Instructions",
            "Keep the plotter (weighing scale) clean or empty."
        )

    def _finalize_calibration(self):
        # Prompt user to place weight
        QMessageBox.information(
            self,
            "Calibration Instructions",
            "Place the calibration weight on the plotter (weighing scale), then send the calibration command."
        )
        # Send finalize command
        self._threaded_scale_cmd("XC_CALIBRATE", stop_poll=True)
        # Confirm completion
        QMessageBox.information(
            self,
            "Calibration",
            "Calibration Step: Completed. Returning to weighing mode..."
        )

    def _toggle_raw_poll(self, checked):
        self.polling = checked
        if checked:
            self._start_raw_poll_thread(interval=CAL_POLL_INTERVAL)
        else:
            self._start_raw_poll_thread(interval=POLL_INTERVAL)

    def _start_raw_poll_thread(self, interval=None):
        if interval is None:
            interval = POLL_INTERVAL
        def poll():
            while self.polling:
                try:
                    with socket.create_connection((self.server_host, PORT), timeout=3) as s:
                        s.sendall(b"MODE:WEIGHT\nXC_RDRAWCT\n")
                        d = s.recv(1024).decode().strip()
                        self.raw_data.emit(d or "<no response>")
                except:
                    pass
                time.sleep(interval)
        threading.Thread(target=poll, daemon=True).start()

    def toggle_connection(self):
        if not self.connected:
            host = self.ip_input.text().strip() or DEFAULT_HOST
            try:
                with socket.create_connection((host, PORT), timeout=3): pass
                self.server_host = host
                self.connected   = True
                self.connect_btn.setText("Disconnect")
                self.ip_input.setEnabled(False)
                self.print_btn.setEnabled(True)
                self.statusBar().showMessage(f"Connected to {host}:{PORT}")
                self.log.append(f"✅ Connected to {host}:{PORT}")
                version = get_driver_version()
                self.version_label.setText(f"Driver Version: {version}")
            except Exception as e:
                self.log.append(f"❌ Connect failed: {e}")
        else:
            self.connected = False
            self.connect_btn.setText("Connect")
            self.ip_input.setEnabled(True)
            self.print_btn.setEnabled(False)
            self.statusBar().showMessage("Disconnected")
            self.log.append("🔌 Disconnected")

    def select_json(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select JSON", "", "JSON Files (*.json)")
        if path:
            self.json_path = path
            self.json_label.setText(os.path.basename(path))
            self.log.append("📄 JSON selected")

    def _threaded_scale_cmd(self, cmd, start_poll=False, stop_poll=False):
        self._set_scale_buttons_enabled(False)
        def task():
            if not self.connected:
                self.scale_response.emit(cmd, "Error: Not connected.")
            else:
                try:
                    s = socket.create_connection((self.server_host, PORT), timeout=3)
                    s.settimeout(RECV_TIMEOUT)
                    s.sendall(b"MODE:WEIGHT\n")
                    try:
                        _ack = s.recv(1024)
                    except socket.timeout:
                        pass
                    s.sendall(cmd.encode() + b"\n")
                    try:
                        resp = s.recv(1024).decode().strip()
                    except socket.timeout:
                        resp = "error: timed out"
                    s.close()
                    self.scale_response.emit(cmd, resp or "<no response>")
                except Exception as e:
                    self.scale_response.emit(cmd, f"error: {e}")
            if start_poll:
                self.polling = True
                self.cal1.setEnabled(False)
            if stop_poll:
                self.polling = False
                self.cal1.setEnabled(True)
        threading.Thread(target=task, daemon=True).start()

    def _set_scale_buttons_enabled(self, ok):
        all_btns = self.norm_btns + self.tech_btns + [
            self.cal1, self.cal2, self.cal3, self.cal4,
            self.cal5, self.cal6
        ]
        for b in all_btns:
            b.setEnabled(ok)

    def _on_scale_response(self, cmd, resp):
        if cmd == "RD_WEIGHT":
            try:
                g = int(resp)
                disp = f"{g/1000:.3f} kg" if g >= 1000 else f"{g} g"
            except:
                disp = resp
            self.weight_display.setText(disp)
        log_entry = f"{cmd} → {resp}"
        self._set_scale_buttons_enabled(True)
        current_tab = self.tabs.tabText(self.tabs.currentIndex())
        if current_tab == "Normal" or cmd == "RD_WEIGHT":
            self.normal_log.append(log_entry)
        elif current_tab == "Calibration":
            self.raw_log.append(log_entry)
        else:
            self.tech_log.append(log_entry)
    
    def _on_raw_data(self, data):
        self.raw_log.append(f"RAW → {data}")
    
    def _create_statusbar(self):
        sb = QStatusBar()
        sb.showMessage("Not connected")
        self.setStatusBar(sb)

    def _apply_styles(self):
        self.setStyleSheet(r"""
            QWidget { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                       stop:0 #FAF0E0, stop:1 #E0FFFF); }
            QPushButton { background-color:#007B8F; color:#FFF;
                          border-radius:6px; padding:6px 12px; font-weight:bold; }
            QPushButton:disabled { background-color:#CCC; }
            QPushButton:hover:!disabled { background-color:#0099A8; }
            QTabWidget::pane { background: transparent; }
            QTabBar::tab { background: rgba(255,255,255,0.8); border-radius:4px; padding:6px; }
            QTabBar::tab:selected { background: #00CED1; color:#002B36; }
            QLabel { color:#003D4C; }
            QTextEdit { background-color:#FFF; color:#003D4C;
                        border:1px solid #A0A0A0; border-radius:4px; }
            QLineEdit { border:1px solid #A0A0A0; border-radius:4px; padding:4px; }
            QListWidget { 
                background-color: #b6dedd;
                border: 1px solid #A0A0A0;
                border-radius: 4px;
                padding: 4px;
                font-size: 11px;
            }
            QListWidget::item {
                padding: 4px;
                margin: 2px;
                border-radius: 3px;
            }
            QListWidget::item:selected {
                background-color: #00CED1;
                color: #002B36;
                font-weight: bold;
            }
        """)

if __name__ == "__main__":
    app = QApplication(sys.argv)
    win = LabelAndScaleGUI()
    win.show()
    sys.exit(app.exec_())
