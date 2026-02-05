import sys
import os
import re
import socket
import winreg
import threading
import requests
import psutil
from pathlib import Path
from PyQt6.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QSystemTrayIcon, QComboBox
)
from PyQt6.QtCore import Qt, QTimer, pyqtSignal, QObject
from PyQt6.QtGui import QIcon, QPixmap

# Настройки
GCODE_TEMP_FOLDERS = [
    str(Path.home() / "AppData/Local/Temp/crealityprint_model"),
    str(Path.home() / "AppData/Roaming/Creality/Creative3D/5.0/GCodes"),
]
ESP32_PORTS = [80, 81]  # Порты для поиска катушек
SCAN_TIMEOUT = 1.0
HOLDER_PREFIX = "FD"


class SpoolHolder:
    def __init__(self, ip, name="", net=0, gross=0, filament_id="", material="", manufacturer="", diameter=1.75, density=1.24, weight=1000.0):
        self.ip = ip
        self.name = name or f"Катушка ({ip})"
        self.net = net
        self.gross = gross
        self.filament_id = filament_id
        self.material = material
        self.manufacturer = manufacturer
        self.diameter = diameter
        self.density = density
        self.weight = weight


def add_to_startup():
    try:
        exe_path = sys.executable if getattr(sys, 'frozen', False) else os.path.abspath(__file__)
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Run",
            0, winreg.KEY_SET_VALUE
        )
        winreg.SetValueEx(key, "FilamindChecker", 0, winreg.REG_SZ, f'"{exe_path}"')
        winreg.CloseKey(key)
        return True
    except:
        return False


def is_in_startup():
    try:
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Run",
            0, winreg.KEY_READ
        )
        winreg.QueryValueEx(key, "FilamindChecker")
        winreg.CloseKey(key)
        return True
    except:
        return False


def get_local_ip():
    """Получает все локальные IP адреса и возвращает наиболее подходящий"""
    import socket
    
    # Сначала пробуем стандартный способ
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        primary_ip = s.getsockname()[0]
        s.close()
    except:
        primary_ip = "192.168.1.1"
    
    # Получаем все IP адреса
    hostname = socket.gethostname()
    try:
        all_ips = socket.gethostbyname_ex(hostname)[2]
        # Фильтруем только приватные IP
        private_ips = []
        for ip in all_ips:
            if (ip.startswith('192.168.') or 
                ip.startswith('10.') or 
                ip.startswith('172.')):
                private_ips.append(ip)
        
        # Приоритет подсетей: 192.168.1.x, 192.168.0.x, потом остальные 192.168.x.x
        priority_subnets = ['192.168.1.', '192.168.0.']
        
        for subnet in priority_subnets:
            for ip in private_ips:
                if ip.startswith(subnet):
                    return ip
        
        # Потом любые 192.168.x.x
        for ip in private_ips:
            if ip.startswith('192.168.'):
                return ip
                
        # Потом 10.x.x.x
        for ip in private_ips:
            if ip.startswith('10.'):
                return ip
                
        # И наконец 172.x.x.x
        if private_ips:
            return private_ips[0]
    except:
        pass
    
    return primary_ip


def scan_network_for_holders(callback):
    """Сканирует несколько наиболее вероятных подсетей"""
    # Получаем основной IP
    local_ip = get_local_ip()
    base_ip = ".".join(local_ip.split(".")[:-1])
    
    # Список подсетей для сканирования
    subnets_to_scan = [base_ip]
    
    # Добавляем наиболее популярные подсети если их еще нет
    common_subnets = ["192.168.1", "192.168.0", "10.0.0", "172.16.0"]
    for subnet in common_subnets:
        if subnet not in subnets_to_scan:
            subnets_to_scan.append(subnet)
    
    found = []
    found_lock = threading.Lock()

    def check_ip_port(ip, port):
        try:
            response = requests.get(f"http://{ip}:{port}/data", timeout=SCAN_TIMEOUT)
            data = response.json()
            name = data.get('name', '')
            if 'net' in data and name.upper().startswith(HOLDER_PREFIX):
                holder = SpoolHolder(
                    ip=f"{ip}:{port}",
                    name=name,
                    net=data.get('net', 0),
                    gross=data.get('gross', 0),
                    filament_id=data.get('filament_id', ''),
                    material=data.get('material', ''),
                    manufacturer=data.get('manufacturer', ''),
                    diameter=data.get('diameter', 1.75),
                    density=data.get('density', 1.24),
                    weight=data.get('weight', 1000.0),
                )
                with found_lock:
                    found.append(holder)
        except:
            pass

    # Ограничиваем количество одновременных потоков
    max_threads = 30
    threads = []
    
    # Сканируем каждую подсеть
    for subnet in subnets_to_scan:
        # Для каждой подсети сканируем только наиболее вероятные IP
        common_host_ids = [1, 10, 11, 12, 13, 14, 15, 20, 100, 101, 102, 200, 254]
        
        for host_id in common_host_ids:
            ip = f"{subnet}.{host_id}"
            for port in ESP32_PORTS:
                t = threading.Thread(target=check_ip_port, args=(ip, port))
                t.start()
                threads.append(t)
                
                # Ограничиваем количество активных потоков
                if len(threads) >= max_threads:
                    for thread in threads:
                        thread.join()
                    threads = []

    # Ждем завершения оставшихся потоков
    for t in threads:
        t.join()

    callback(found)


def get_holder_data(ip_port):
    try:
        response = requests.get(f"http://{ip_port}/data", timeout=1)
        data = response.json()
        return SpoolHolder(
            ip=ip_port,
            name=data.get('name', ''),
            net=data.get('net', 0),
            gross=data.get('gross', 0),
            filament_id=data.get('filament_id', ''),
            material=data.get('material', ''),
            manufacturer=data.get('manufacturer', ''),
            diameter=data.get('diameter', 1.75),
            density=data.get('density', 1.24),
            weight=data.get('weight', 1000.0),
        )
    except:
        return None


def parse_gcode(filepath):
    """Парсит G-code и ищет вес филамента и имя модели"""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()

        # Ищем имя модели из "; printing object XXX.stl id:0"
        model_names = []
        for match in re.finditer(r'; printing object (.+?)\.stl id:', content):
            name = match.group(1)
            if name and name not in model_names:
                model_names.append(name)

        model_name = ", ".join(model_names) if model_names else None

        # Ищем вес в конце файла
        weight = None
        match = re.search(r';\s*filament used \[g\]\s*=\s*([\d.]+)', content, re.IGNORECASE)
        if match:
            weight = float(match.group(1))
        else:
            match = re.search(r';Filament used:\s*([\d.]+)\s*g', content, re.IGNORECASE)
            if match:
                weight = float(match.group(1))

        return weight, model_name
    except:
        return None, None


def find_active_gcode():
    """Находит gcode активной модели по самой свежей папке"""
    latest_file = None
    latest_mtime = 0

    # Проверяем все возможные папки
    for folder in GCODE_TEMP_FOLDERS:
        if not os.path.exists(folder):
            continue

        # Ищем все gcode файлы
        for root, dirs, files in os.walk(folder):
            for f in files:
                if f.endswith('.gcode'):
                    filepath = os.path.join(root, f)
                    try:
                        # Берём время модификации файла или родительской папки
                        file_mtime = os.path.getmtime(filepath)
                        parent_mtime = os.path.getmtime(os.path.dirname(filepath))
                        mtime = max(file_mtime, parent_mtime)
                        
                        if mtime > latest_mtime:
                            latest_mtime = mtime
                            latest_file = filepath
                    except:
                        pass

    return latest_file, latest_mtime


class Signals(QObject):
    update_ui = pyqtSignal()
    holders_found = pyqtSignal(list)


class FilamindCheckerWidget(QWidget):
    def __init__(self):
        super().__init__()
        self.required = 0.0
        self.model_name = ""
        self.last_file = ""
        self.last_mtime = 0
        self.holders = []
        self.selected_holder = None
        self.signals = Signals()
        self.signals.update_ui.connect(self.update_display)
        self.signals.holders_found.connect(self.on_holders_found)
        self.init_ui()
        self.init_tray()
        self.scan_holders()

    def init_ui(self):
        self.setWindowTitle("Filamind Checker")
        self.setFixedSize(360, 320)  # Увеличиваем высоту чтобы всё влезло
        self.setWindowFlags(
            Qt.WindowType.WindowStaysOnTopHint |
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

        container = QWidget(self)
        container.setStyleSheet("""
            QWidget {
                background-color: #2b2b2b;
                border-radius: 10px;
                color: white;
            }
            QLabel { background: transparent; }
            QPushButton {
                background-color: #404040;
                border: none;
                padding: 5px 10px;
                border-radius: 5px;
                color: white;
                font-size: 13px;
            }
            QPushButton:hover { background-color: #505050; }
            QPushButton#checkBtn {
                background-color: #4CAF50;
                font-weight: bold;
                padding: 8px;
                font-size: 14px;
            }
            QPushButton#checkBtn:hover { background-color: #45a049; }
            QComboBox {
                background-color: #404040;
                border: none;
                padding: 5px;
                border-radius: 5px;
                color: white;
                font-size: 13px;
            }
            QComboBox::drop-down { border: none; }
            QComboBox QAbstractItemView {
                background-color: #353535;
                color: white;
                selection-background-color: #505050;
            }
        """)
        container.setGeometry(0, 0, 360, 320)

        layout = QVBoxLayout(container)
        layout.setContentsMargins(15, 10, 15, 12)  # Немного больше отступ снизу
        layout.setSpacing(6)

        # Заголовок
        title = QLabel("Filamind Checker")
        title.setStyleSheet("font-size: 14px; color: #888;")
        layout.addWidget(title)

        # Имя модели
        self.model_label = QLabel("")
        self.model_label.setStyleSheet("font-size: 13px; color: #aaa;")
        self.model_label.setWordWrap(True)
        layout.addWidget(self.model_label)

        # Информация о филаменте (производитель, материал, диаметр)
        self.filament_info_label = QLabel("")
        self.filament_info_label.setStyleSheet("font-size: 13px; color: #aaa;")
        self.filament_info_label.setWordWrap(True)
        layout.addWidget(self.filament_info_label)

        # Выбор катушки
        holder_layout = QHBoxLayout()
        holder_label = QLabel("Катушка:")
        holder_label.setStyleSheet("font-size: 13px;")
        holder_layout.addWidget(holder_label)
        self.holder_combo = QComboBox()
        self.holder_combo.addItem("Поиск...")
        self.holder_combo.currentIndexChanged.connect(self.on_holder_selected)
        holder_layout.addWidget(self.holder_combo, 1)
        layout.addLayout(holder_layout)

        # Кнопка проверки
        self.check_btn = QPushButton("Проверить")
        self.check_btn.setObjectName("checkBtn")
        self.check_btn.clicked.connect(self.do_check)
        layout.addWidget(self.check_btn)

        # Статус
        self.status_label = QLabel("Нажмите 'Проверить'")
        self.status_label.setStyleSheet("font-size: 18px; font-weight: bold;")
        self.status_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.status_label)

        # Процент
        self.percent_label = QLabel("")
        self.percent_label.setStyleSheet("font-size: 15px; color: #888;")
        self.percent_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.percent_label)

        # Данные - с достаточным местом
        data_layout = QHBoxLayout()
        data_layout.setSpacing(20)

        left = QVBoxLayout()
        left_label = QLabel("На катушке:")
        left_label.setStyleSheet("font-size: 13px;")
        left.addWidget(left_label)
        self.available_label = QLabel("-- г")
        self.available_label.setStyleSheet("font-size: 22px; font-weight: bold; padding-top: 5px; padding-bottom: 10px;")
        left.addWidget(self.available_label)
        data_layout.addLayout(left)

        right = QVBoxLayout()
        right_label = QLabel("Нужно:")
        right_label.setStyleSheet("font-size: 13px;")
        right.addWidget(right_label)
        self.required_label = QLabel("-- г")
        self.required_label.setStyleSheet("font-size: 22px; font-weight: bold; padding-top: 5px; padding-bottom: 10px;")
        right.addWidget(self.required_label)
        data_layout.addLayout(right)

        layout.addLayout(data_layout)

        # Длина внизу с отступом
        self.length_label = QLabel("")
        self.length_label.setStyleSheet("font-size: 11px; color: #666; padding-top: 8px;")
        self.length_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.length_label.setMinimumHeight(20)  # Минимальная высота для текста
        layout.addWidget(self.length_label)

        # Позиция
        screen = QApplication.primaryScreen().geometry()
        self.move(screen.width() - 380, 100)

    def init_tray(self):
        self.tray = QSystemTrayIcon(self)
        pixmap = QPixmap(16, 16)
        pixmap.fill(Qt.GlobalColor.green)
        self.tray.setIcon(QIcon(pixmap))
        self.tray.setToolTip("Filamind Checker")
        self.tray.show()

    def scan_holders(self):
        self.holder_combo.clear()
        self.holder_combo.addItem("Поиск...")
        threading.Thread(
            target=scan_network_for_holders,
            args=(lambda h: self.signals.holders_found.emit(h),),
            daemon=True
        ).start()

    def on_holders_found(self, holders):
        # Сохраняем текущий выбор до обновления списка
        prev_selected_ip = self.selected_holder.ip if self.selected_holder else None
        
        self.holders = holders
        self.holder_combo.blockSignals(True)  # Блокируем сигналы чтобы не сбросить выбор
        self.holder_combo.clear()

        if not holders:
            self.holder_combo.addItem("Не найдено")
            self.selected_holder = None
            self.status_label.setText("Катушки не найдены")
            self.status_label.setStyleSheet("font-size: 14px; font-weight: bold; color: #f44336;")
            self.available_label.setText("-- г")
        else:
            for h in holders:
                self.holder_combo.addItem(f"{h.name} ({h.net}г)", h.ip)
            
            # Восстанавливаем выбор если катушка ещё доступна
            restored = False
            if prev_selected_ip:
                for i, h in enumerate(holders):
                    if h.ip == prev_selected_ip:
                        self.selected_holder = h
                        self.holder_combo.setCurrentIndex(i)
                        restored = True
                        break
            
            if not restored:
                self.selected_holder = holders[0]
                self.holder_combo.setCurrentIndex(0)

        self.holder_combo.blockSignals(False)
        self.check_btn.setEnabled(True)
        self.check_btn.setText("Проверить")
        self.update_display()

    def on_holder_selected(self, index):
        if 0 <= index < len(self.holders):
            self.selected_holder = self.holders[index]
            self.update_display()

    def do_check(self):
        """Кнопка проверки — обновление данных катушек + поиск gcode"""
        self.check_btn.setEnabled(False)
        self.check_btn.setText("Проверка...")
        self.status_label.setText("Обновление данных...")
        self.status_label.setStyleSheet("font-size: 14px; font-weight: bold; color: #888;")

        def check_thread():
            # Обновляем данные существующих катушек
            updated_holders = []
            for holder in self.holders:
                updated_holder = get_holder_data(holder.ip)
                if updated_holder:
                    updated_holders.append(updated_holder)
                else:
                    # Если катушка не отвечает, оставляем старые данные
                    updated_holders.append(holder)

            # Если катушек нет, делаем быстрое сканирование популярных IP
            if not updated_holders:
                # Пробуем популярные подсети и IP адреса
                common_targets = [
                    "192.168.1.12", "192.168.1.10", "192.168.1.11", "192.168.1.1",
                    "192.168.0.12", "192.168.0.10", "192.168.0.11", "192.168.0.1",
                    "10.0.0.12", "10.0.0.10", "10.0.0.11", "10.0.0.1"
                ]
                
                for ip in common_targets:
                    for port in ESP32_PORTS:
                        holder = get_holder_data(f"{ip}:{port}")
                        if holder:
                            updated_holders.append(holder)
                            break  # Нашли катушку, можно остановиться

            # Ищем активный gcode
            filepath, mtime = find_active_gcode()
            if filepath:
                weight, model_name = parse_gcode(filepath)
                if weight:
                    self.required = weight
                    self.model_name = model_name or ""
                    self.last_file = filepath
                    self.last_mtime = mtime

            self.signals.holders_found.emit(updated_holders)

        threading.Thread(target=check_thread, daemon=True).start()

    def update_display(self):
        self.check_btn.setEnabled(True)
        self.check_btn.setText("Проверить")

        available = self.selected_holder.net if self.selected_holder else 0
        required = self.required

        self.available_label.setText(f"{available} г" if available else "-- г")
        self.required_label.setText(f"{required} г" if required else "-- г")
        
        # Имя модели
        if self.model_name:
            self.model_label.setText(f"Модель: {self.model_name}")
        else:
            self.model_label.setText("")

        # Информация о филаменте (производитель, материал, диаметр)
        if self.selected_holder and self.selected_holder.material:
            info_parts = []
            if self.selected_holder.manufacturer:
                info_parts.append(self.selected_holder.manufacturer)
            if self.selected_holder.material:
                info_parts.append(self.selected_holder.material)
            if self.selected_holder.diameter:
                info_parts.append(f"{self.selected_holder.diameter}mm")
            
            if info_parts:
                self.filament_info_label.setText(" | ".join(info_parts))
            else:
                self.filament_info_label.setText("")
        else:
            self.filament_info_label.setText("")

        # Обновляем комбобокс
        if self.selected_holder:
            for i, h in enumerate(self.holders):
                if h.ip == self.selected_holder.ip:
                    self.holder_combo.setItemText(i, f"{self.selected_holder.name} ({self.selected_holder.net}г)")
                    break

        # Только длина внизу
        length_text = ""
        if self.selected_holder and available > 0:
            density = self.selected_holder.density  # г/см³
            diameter = self.selected_holder.diameter  # мм
            # Формула: Длина (м) = Объем / Площадь_сечения
            # Объем (см³) = Вес (г) / Плотность (г/см³)
            # Площадь_сечения (см²) = π × (Диаметр/2)²
            # 1 мм = 0.1 см, поэтому диаметр_см = диаметр_мм / 10
            diameter_cm = diameter / 10.0  # мм -> см
            radius_cm = diameter_cm / 2.0
            area_cm2 = 3.14159265359 * radius_cm * radius_cm  # см²
            volume_cm3 = available / density  # см³
            length_cm = volume_cm3 / area_cm2  # см
            length_m = length_cm / 100.0  # см -> м
            length_text = f"Длина: ~{length_m:.1f}м"

        self.length_label.setText(length_text)

        # Расчет процента от начального веса филамента (из профиля ESP32)
        initial_weight = self.selected_holder.weight if self.selected_holder else 1000.0
        percent_remaining = (available / initial_weight) * 100 if available > 0 and initial_weight > 0 else 0

        if required and available:
            if available >= required:
                self.status_label.setText("✓ ХВАТИТ")
                self.status_label.setStyleSheet("font-size: 18px; font-weight: bold; color: #4CAF50;")
                self.percent_label.setText(f"Осталось: {percent_remaining:.0f}%")
                self.percent_label.setStyleSheet("font-size: 14px; color: #4CAF50;")
            else:
                deficit = round(required - available, 2)
                self.status_label.setText(f"✗ НЕ ХВАТИТ (-{deficit}г)")
                self.status_label.setStyleSheet("font-size: 18px; font-weight: bold; color: #f44336;")
                self.percent_label.setText(f"Осталось: {percent_remaining:.0f}%")
                self.percent_label.setStyleSheet("font-size: 14px; color: #f44336;")
        elif required and not available:
            self.status_label.setText(f"Нужно: {required}г")
            self.status_label.setStyleSheet("font-size: 16px; font-weight: bold; color: #ff9800;")
            self.percent_label.setText("")
        elif not self.selected_holder:
            self.status_label.setText("Катушки не найдены")
            self.status_label.setStyleSheet("font-size: 14px; font-weight: bold; color: #888;")
            self.percent_label.setText("")
        else:
            self.status_label.setText("Нажмите 'Проверить'")
            self.status_label.setStyleSheet("font-size: 14px; font-weight: bold; color: #888;")
            if available > 0:
                self.percent_label.setText(f"Осталось: {percent_remaining:.0f}%")
                self.percent_label.setStyleSheet("font-size: 14px; color: #888;")
            else:
                self.percent_label.setText("")

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.drag_pos = event.globalPosition().toPoint() - self.frameGeometry().topLeft()

    def mouseMoveEvent(self, event):
        if event.buttons() == Qt.MouseButton.LeftButton:
            self.move(event.globalPosition().toPoint() - self.drag_pos)


def is_creality_running():
    """Проверяет запущен ли Creality Print"""
    for proc in psutil.process_iter(['name']):
        try:
            if 'creality' in proc.info['name'].lower():
                return True
        except:
            pass
    return False


def main():
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)

    if not is_in_startup():
        add_to_startup()

    widget = FilamindCheckerWidget()

    # Показываем только если Creality запущен
    if is_creality_running():
        widget.show()

    # Таймер проверки Creality каждые 3 сек
    def check_creality():
        if is_creality_running():
            if not widget.isVisible():
                widget.show()
                widget.scan_holders()
        else:
            if widget.isVisible():
                widget.hide()
                widget.required = 0.0
                widget.model_name = ""
                widget.update_display()

    creality_timer = QTimer()
    creality_timer.timeout.connect(check_creality)
    creality_timer.start(3000)

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
