import sys
import json
import signal
import hashlib
import time
from datetime import datetime
import logging
import os
import psutil
import requests
import serial
from pathlib import Path
from collections import deque
from dotenv import load_dotenv
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                               QLabel, QPushButton, QListWidget, QMessageBox,
                               QInputDialog, QLineEdit)

# Загружаем .env файл
ENV_FILE = Path(__file__).parent / '.env'
load_dotenv(ENV_FILE)



# Настройка логгера
LOG_LEVELS = {
    'DEBUG': logging.DEBUG,
    'INFO': logging.INFO,
    'WARN': logging.WARN,
    'WARNING': logging.WARNING,
    'ERROR': logging.ERROR,
}

log_level = LOG_LEVELS.get(os.getenv('LOG_LEVEL', 'INFO').upper(), logging.INFO)
logging.basicConfig(
    level=log_level,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
logger = logging.getLogger(__name__)


REMOTE_SERVER_API = os.getenv('REMOTE_SERVER_API', 'http://server:8080/api/records')
SERIAL_PORT = '/dev/arduino'
BAUDRATE = 115200
HEARTBEAT_INTERVAL = 60   # секунды

CALIBRATION_FILE = Path(__file__).parent / 'calibration.json'



# Конфигурация heartbeat из .env
HEARTBEAT_SERVER = os.getenv('HB_SERVER', 'http://enter_server:1111')
DEVICE_ID = os.getenv('DEVICE_ID', '1')


class TerminalUI(QWidget):
    def __init__(self):
        super().__init__()
        self.records = []
        self.current_weight = 0.0
        self.last_fix_weight = 0.0
        self.is_on_target = False
        self.current_tag = ""
        self.current_bin_name = ""
        self.last_rfid_time = 0.0
        self.start_time = time.time()

        # Калибровочные константы
        self.zero_offset = 0
        self.counts_per_kg = 1000000.0
        
        # Буфер для фильтрации raw данных
        self.raw_buffer = deque(maxlen=10)
        self.last_raw_value = None

        self.rfid_mapping = self.load_rfid_mapping()
        self.admin_password_hash = self.load_admin_password()
        self.load_calibration()

        self.init_ui()
        self.init_serial()
        self.init_heartbeat()

    def init_heartbeat(self):
        """Инициализирует таймер отправки heartbeat."""
        self.heartbeat_timer = QTimer()
        self.heartbeat_timer.timeout.connect(self.send_heartbeat)
        self.heartbeat_timer.start(HEARTBEAT_INTERVAL * 1000)
        logger.info(f"💓 Heartbeat запущен: интервал {HEARTBEAT_INTERVAL}с, сервер {HEARTBEAT_SERVER}")

    def send_heartbeat(self):
        """Отправляет heartbeat на сервер с системной информацией."""
        try:
            # Собираем системную информацию
            memory = psutil.virtual_memory()
            cpu_percent = psutil.cpu_percent(interval=0.1)
            uptime = int(time.time() - self.start_time)
            
            payload = {
                "device_type": "terminal",
                "device_id": DEVICE_ID,
                "health": {
                    "cpu_percent": cpu_percent,
                    "memory_percent": memory.percent,
                    "memory_available_mb": memory.available // (1024 * 1024),
                    "uptime_seconds": uptime,
                    "serial_connected": self.ser is not None,
                    "is_on_target": self.is_on_target,
                    "current_weight": self.current_weight
                }
            }
            
            url = f"{HEARTBEAT_SERVER}/api/heartbeat"
            response = requests.post(url, json=payload, timeout=5)
            
            if response.status_code == 200:
                logger.debug(f"💓 Heartbeat отправлен -> CPU: {cpu_percent:.1f}%, RAM: {memory.percent:.1f}%, Uptime: {uptime}с")
            else:
                logger.warning(f"💓 Heartbeat ответ: {response.status_code}")
                
        except requests.exceptions.ConnectionError:
            logger.warning(f"💓 Heartbeat не удалось подключиться к {HEARTBEAT_SERVER}")
        except requests.exceptions.Timeout:
            logger.warning(f"💓 Heartbeat таймаут подключения")
        except Exception as e:
            logger.error(f"💓 Heartbeat ошибка: {e}")
    # ---------- .ENV ----------
    def load_rfid_mapping(self):
        """Загружает маппинг RFID UID → название бункера из .env файла."""
        mapping = {}
        for key, value in os.environ.items():
            if key.startswith('RFID_'):
                uid = key.replace('RFID_', '')
                mapping[uid] = value
        if mapping:
            logger.debug(f"📋 Загружен маппинг RFID: {mapping}")
        else:
            logger.warning(f"⚠️ Маппинг RFID пуст. Проверь .env файл.")
        return mapping

    def load_admin_password(self):
        """Загружает пароль администратора из .env файла и вычисляет его хэш."""
        password = os.getenv('PASS', 'admin')
        if password == 'admin':
            logger.warning(f"⚠️ Используется пароль по умолчанию. Установи PASS в .env")
        else:
            logger.debug(f"🔐 Пароль администратора загружен из .env")

        return hashlib.sha256(password.encode()).hexdigest()


    # ---------- КАЛИБРОВКА ----------
    def load_calibration(self):
        """Загружает калибровочные константы из файла."""
        if CALIBRATION_FILE.exists():
            try:
                with open(CALIBRATION_FILE, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    self.zero_offset = data.get('zero_offset', 0)
                    self.counts_per_kg = data.get('counts_per_kg', 1000000.0)
                logger.info(f"📊 Калибровка загружена: zero={self.zero_offset}, counts/kg={self.counts_per_kg:.0f}")
            except Exception as e:
                logger.error(f"❌ Ошибка загрузки калибровки: {e}")
        else:
            logger.warning(f"⚠️ Файл калибровки не найден, используются значения по умолчанию")

    def save_calibration(self):
        """Сохраняет калибровочные константы в файл."""
        try:
            data = {
                'zero_offset': self.zero_offset,
                'counts_per_kg': self.counts_per_kg,
            }
            with open(CALIBRATION_FILE, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
            logger.info(f"💾 Калибровка сохранена: {data}")
        except Exception as e:
            logger.error(f"❌ Ошибка сохранения калибровки: {e}")

    def process_loadcell_raw(self, raw):
        """Обрабатывает сырые данные тензодатчика и пересчитывает в вес."""
        self.raw_buffer.append(raw)
        self.last_raw_value = raw
        
        # Медианный фильтр для устранения выбросов
        sorted_raw = sorted(self.raw_buffer)
        median_raw = sorted_raw[len(sorted_raw) // 2]
        
        # Пересчет в килограммы
        if self.counts_per_kg != 0:
            weight_kg = (median_raw - self.zero_offset) / self.counts_per_kg
        else:
            weight_kg = 0.0
        
        self.current_weight = weight_kg
        self.update_weight_display()
        logger.debug(f"⚖️ Raw: {raw}, Median: {median_raw}, Weight: {self.current_weight:.2f} кг")

    # ---------- ИНТЕРФЕЙС ----------
    def init_ui(self):
        self.setStyleSheet("background-color: #1e1e2e; color: #ffffff;")

        main_layout = QVBoxLayout()
        main_layout.setSpacing(15)
        main_layout.setContentsMargins(20, 20, 20, 20)

        # Верхняя панель с кнопкой администратора
        top_layout = QHBoxLayout()
        top_layout.addStretch()

        self.btn_admin = QPushButton("⚙")
        self.btn_admin.setFixedSize(40, 40)
        self.btn_admin.setStyleSheet("""
            QPushButton {
                font-size: 24px;
                background-color: #45475a;
                color: #f9e2af;
                border-radius: 20px;
                border: 2px solid #f9e2af;
            }
            QPushButton:pressed {
                background-color: #585b70;
            }
        """)
        self.btn_admin.clicked.connect(self.admin_login)
        self.btn_admin.setToolTip("Админ-панель")
        top_layout.addWidget(self.btn_admin)

        main_layout.addLayout(top_layout)

        # Первая строка: два веса в одну линию
        weight_row = QHBoxLayout()
        
        # Общий вес
        self.weight_label = QLabel("Общий вес:\n-- кг")
        self.weight_label.setAlignment(Qt.AlignCenter)
        self.weight_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #89dceb;")
        weight_row.addWidget(self.weight_label)
        
        # Вес относительно фиксации
        self.delta_weight_label = QLabel("Относительно фиксации:\n-- кг")
        self.delta_weight_label.setAlignment(Qt.AlignCenter)
        self.delta_weight_label.setStyleSheet("font-size: 32px; color: #f9e2af;")
        weight_row.addWidget(self.delta_weight_label)
        
        main_layout.addLayout(weight_row)

        # Вторая строка: позиция (крупный шрифт как у общего веса)
        self.status_label = QLabel("Позиция: ожидание...")
        self.status_label.setAlignment(Qt.AlignCenter)
        self.status_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #f38ba8;")
        main_layout.addWidget(self.status_label)

        # Список зафиксированных записей
        self.list_widget = QListWidget()
        self.list_widget.setStyleSheet("font-size: 18px; background-color: #313244;")
        main_layout.addWidget(self.list_widget)

        # Кнопки
        btn_layout = QHBoxLayout()

        self.btn_fix = QPushButton("1. Фиксировать вес")
        self.btn_send = QPushButton("2. Отправить на сервер")
        self.btn_reset = QPushButton("3. Сброс")

        for btn in (self.btn_fix, self.btn_send, self.btn_reset):
            btn.setFixedHeight(90)
            btn.setStyleSheet("""
                QPushButton { font-size: 20px; background-color: #45475a;
                              border-radius: 10px; color: white; }
                QPushButton:pressed { background-color: #585b70; }
                QPushButton:disabled { color: #6c7086; background-color: #313244; }
            """)

        self.btn_fix.clicked.connect(self.fix_weight)
        self.btn_send.clicked.connect(self.send_to_server)
        self.btn_reset.clicked.connect(self.reset_records)

        btn_layout.addWidget(self.btn_fix)
        btn_layout.addWidget(self.btn_send)
        btn_layout.addWidget(self.btn_reset)
        main_layout.addLayout(btn_layout)

        self.setLayout(main_layout)
        self.showFullScreen()

        self.update_fix_button()

    # ---------- SERIAL ----------
    def init_serial(self):
        try:
            self.ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=0.1)
            logger.info(f"✅ Serial порт открыт: {SERIAL_PORT}")
        except Exception as e:
            logger.error(f"❌ Ошибка порта: {e}")
            self.ser = None

        # Буфер для сбора данных из serial
        self.serial_buffer = ""

        # Таймер опроса serial порта
        self.timer = QTimer()
        self.timer.timeout.connect(self.read_serial)
        self.timer.start(100)

        # Таймер проверки устаревания RFID-сигнала (каждые 200 мс)
        self.position_timer = QTimer()
        self.position_timer.timeout.connect(self.check_position_timeout)
        self.position_timer.start(200)

    def read_serial(self):
        """Читает и обрабатывает данные из serial порта."""
        if not self.ser:
            return

        # Читаем все доступные данные в буфер
        if self.ser.in_waiting:
            try:
                chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                self.serial_buffer += chunk
            except Exception as e:
                logger.error(f"❌ Ошибка чтения serial: {e}")
                return

        # Ищем полные JSON-сообщения в буфере
        while True:
            start = self.serial_buffer.find('{')
            if start == -1:
                self.serial_buffer = ""
                break

            end = self.serial_buffer.find('}', start)
            if end == -1:
                # Нет закрывающей скобки — ждём ещё данных
                self.serial_buffer = self.serial_buffer[start:]
                break

            json_str = self.serial_buffer[start:end + 1]
            self.serial_buffer = self.serial_buffer[end + 1:]

            try:
                data = json.loads(json_str)
            except json.JSONDecodeError:
                continue

            dtype = data.get("type")
            logger.debug(f"📨 Получено от ESP32: {json_str}")
            logger.debug(f"📦 Тип данных: {dtype}")

            if dtype == "weight":
                self.current_weight = data.get("value", 0.0)
                self.update_weight_display()
                logger.debug(f"⚖️ Вес обновлён: {self.current_weight:.1f} кг")

            elif dtype == "loadcell":
                raw = data.get("raw")
                if raw is not None:
                    self.process_loadcell_raw(int(raw))

            elif dtype == "rfid":
                uid = data.get("uid", "")
                self.current_tag = uid
                self.last_rfid_time = time.time()

                if uid in self.rfid_mapping:
                    self.current_bin_name = self.rfid_mapping[uid]
                    logger.debug(f"🏷️ RFID считан: UID={uid}, бункер={self.current_bin_name}")
                else:
                    self.current_bin_name = f"неизвестный ({uid})"
                    logger.warning(f"⚠️ RFID метка не найдена в .env: UID={uid}")

                self.check_position()

            elif dtype == "position":
                self.is_on_target = data.get("on_target", False)
                logger.debug(f"📍 Позиция от контроллера: on_target={self.is_on_target}")
                self.update_position_ui()

            elif dtype == "boot":
                logger.info(f"🚀 Boot: PN532={data.get('pn532')}, HX711={data.get('hx711')}")

            elif dtype == "status":
                logger.info(f"📊 Status: {data}")

            elif dtype == "error":
                logger.error(f"❌ Error [{data.get('module')}]: {data.get('message')}")

            else:
                logger.warning(f"⚠️ Неизвестный тип данных: {dtype}")


    def update_weight_display(self):
        """Обновляет отображение обоих весов."""
        self.weight_label.setText(f"Общий вес: {self.current_weight:.1f} кг")
        
        delta = self.current_weight - self.last_fix_weight
        sign = "+" if delta >= 0 else ""
        self.delta_weight_label.setText(f"Относительно фиксации: {sign}{delta:.1f} кг")

    def check_position_timeout(self):
        """Периодически проверяет, не устарел ли RFID-сигнал."""
        if self.last_rfid_time > 0 and (time.time() - self.last_rfid_time) > 1.0:
            if self.is_on_target:
                self.is_on_target = False
                logger.debug(f"⏱️ RFID сигнал устарел (>1 сек), позиция сброшена")
                self.update_position_ui()

    # ---------- ЛОГИКА ПОЗИЦИОНИРОВАНИЯ ----------
    def check_position(self):
        """Проверяем позицию: если за последнюю секунду приходил RFID — мы на метке."""
        elapsed = time.time() - self.last_rfid_time
        self.is_on_target = elapsed < 1.0
        logger.debug(f"🎯 Проверка позиции: прошло {elapsed:.2f} сек, на метке: {self.is_on_target}")
        self.update_position_ui()

    def update_position_ui(self):
        """Обновляет UI статуса позиции."""
        if self.is_on_target:
            self.status_label.setText(f"Позиция: ✅ {self.current_bin_name}")
            self.status_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #a6e3a1;")
        else:
            self.status_label.setText("Позиция: ❌ не на метке")
            self.status_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #f38ba8;")
        self.update_fix_button()

    def update_fix_button(self):
        self.btn_fix.setEnabled(self.is_on_target)

    # ---------- АДМИН-ПАНЕЛЬ ----------
    def admin_login(self):
        password, ok = QInputDialog.getText(
            self, "Доступ", "Пароль:", QLineEdit.Password
        )
        if not ok:
            return

        entered_hash = hashlib.sha256(password.encode()).hexdigest()
        if entered_hash == self.admin_password_hash:
            logger.debug(f"✅ Вход в админ-панель успешен")
            self.show_admin_menu()
        else:
            logger.warning(f"❌ Неверный пароль администратора")
            QMessageBox.warning(self, "Ошибка", "Неверный пароль")

    def show_admin_menu(self):
        """Показывает админ-меню в виде диалога с кнопками."""
        dialog = QMessageBox(self)
        dialog.setWindowTitle("Админ-панель")
        dialog.setText("Выберите действие:")
        dialog.setStyleSheet("""
            QMessageBox {
                background-color: #313244;
                color: #ffffff;
            }
            QMessageBox QLabel {
                color: #ffffff;
                font-size: 18px;
            }
            QPushButton {
                font-size: 16px;
                padding: 10px 20px;
                background-color: #45475a;
                color: white;
                border-radius: 5px;
                margin: 5px;
            }
            QPushButton:hover {
                background-color: #585b70;
            }
        """)

        btn_reset_zero = dialog.addButton("🎯 Сбросить 0", QMessageBox.ActionRole)
        btn_set_coeff = dialog.addButton("📐 Установить коэффициент (counts/kg)", QMessageBox.ActionRole)
        btn_exit = dialog.addButton("🚪 Выйти из программы", QMessageBox.ActionRole)
        btn_cancel = dialog.addButton("Отмена", QMessageBox.RejectRole)

        dialog.exec()

        clicked = dialog.clickedButton()
        if clicked == btn_reset_zero:
            self.admin_reset_zero()
        elif clicked == btn_set_coeff:
            self.admin_set_counts_per_kg()
        elif clicked == btn_exit:
            self.close_app()


    def admin_reset_zero(self):
        """Сбрасывает ноль - устанавливает текущее значение как zero_offset."""
        if self.last_raw_value is None:
            QMessageBox.warning(self, "Ошибка", "Нет данных от тензодатчика")
            return

        reply = QMessageBox.question(
            self,
            "Сброс нуля",
            f"Установить текущее значение {self.last_raw_value} как ноль?",
            QMessageBox.Yes | QMessageBox.No
        )

        if reply == QMessageBox.Yes:
            self.zero_offset = self.last_raw_value
            self.save_calibration()
            self.process_loadcell_raw(self.last_raw_value)  # Пересчитать вес
            QMessageBox.information(self, "Готово", f"Ноль сброшен: {self.zero_offset}")
            logger.info(f"🎯 Ноль сброшен: {self.zero_offset}")

    def admin_set_counts_per_kg(self):
        """Устанавливает коэффициент пересчета counts_per_kg."""
        coeff_str, ok = QInputDialog.getText(
            self,
            "Коэффициент пересчета",
            f"Введите counts_per_kg (текущий: {self.counts_per_kg:.0f}):"
        )

        if not ok:
            return

        try:
            coeff = float(coeff_str.replace(',', '.'))
        except ValueError:
            QMessageBox.warning(self, "Ошибка", "Неверный формат числа")
            return

        if coeff == 0:
            QMessageBox.warning(self, "Ошибка", "Коэффициент не может быть равен нулю")
            return

        self.counts_per_kg = coeff
        self.save_calibration()
        
        if self.last_raw_value is not None:
            self.process_loadcell_raw(self.last_raw_value)  # Пересчитать вес
        
        QMessageBox.information(self, "Готово", f"Коэффициент установлен: {coeff:.0f} counts/kg")
        logger.info(f"📐 Коэффициент установлен: {coeff:.0f} counts/kg")

    def close_app(self):
        """Закрывает приложение и освобождает ресурсы."""
        if hasattr(self, 'heartbeat_timer'):
            self.heartbeat_timer.stop()
        if self.ser:
            self.ser.close()
        QApplication.quit()


    # ---------- КНОПКИ ----------
    def fix_weight(self):
        if not self.is_on_target:
            QMessageBox.warning(self, "Ошибка", "Нельзя фиксировать: вы не на метке!")
            logger.warning(f"❌ Попытка фиксации вне позиции")
            return

        tag_name = self.current_bin_name if self.current_bin_name else "бункер"
        timestamp = datetime.now().isoformat()
        record = {
            "tag": tag_name,
            "weight": self.current_weight,
            "timestamp": timestamp
        }

        self.records.append(record)

        # Отображение в списке
        display_record = f"{tag_name} — {self.current_weight:.1f} кг"
        self.list_widget.addItem(display_record)
        
        # Сохраняем вес для расчёта дельты
        self.last_fix_weight = self.current_weight
        self.update_weight_display()
        
        logger.info(f"✅ Зафиксировано: {record}")

    def send_to_server(self):
        if not self.records:
            QMessageBox.information(self, "Сервер", "Нет записей для отправки.")
            logger.debug(f"📤 Отправка отменена: нет записей")
            return

        payload = {
            "action": "send",
            "device_id": DEVICE_ID,
            "sent_at": datetime.now().isoformat(),
            "records": self.records
        }
        
        def on_success():
            logger.info(f"📤 Отправлено на сервер: {len(self.records)} записей")
            QMessageBox.information(self, "Успех", f"Отправлено {len(self.records)} записей")
            self.records.clear()
            self.list_widget.clear()
        
        self.transmit(payload, success_callback=on_success)

    def reset_records(self):
        payload = {
            "action": "reset",
            "device_id": DEVICE_ID,
            "sent_at": datetime.now().isoformat(),
            "records": self.records
        }
        
        def on_success():
            logger.info(f"🔄 Сброс: очищено {len(self.records)} записей")
            self.records.clear()
            self.list_widget.clear()
            self.last_fix_weight = 0.0
            self.update_weight_display()
        
        self.transmit(payload, success_callback=on_success)

    def transmit(self, payload, success_callback=None):
        """Отправляет payload на сервер и вызывает callback при успехе."""
        try:
            response = requests.post(
                REMOTE_SERVER_API,
                json=payload,
                timeout=10
            )
            
            if response.status_code in (200, 201):
                if success_callback:
                    success_callback()
            else:
                logger.error(f"❌ Ошибка отправки: статус {response.status_code}, ответ: {response.text}")
                QMessageBox.critical(self, "Ошибка", f"Сервер вернул ошибку: {response.status_code}")
                
        except requests.exceptions.ConnectionError:
            logger.error(f"❌ Не удалось подключиться к {REMOTE_SERVER_API}")
            QMessageBox.critical(self, "Ошибка", "Не удалось подключиться к серверу")
        except requests.exceptions.Timeout:
            logger.error(f"❌ Таймаут отправки на {REMOTE_SERVER_API}")
            QMessageBox.critical(self, "Ошибка", "Таймаут подключения к серверу")
        except Exception as e:
            logger.error(f"❌ Ошибка отправки: {e}")
            QMessageBox.critical(self, "Ошибка", f"Ошибка: {e}")

if __name__ == "__main__":
    app = QApplication(sys.argv)

    signal.signal(signal.SIGINT, signal.SIG_DFL)
    _timer = QTimer()
    _timer.start(200)
    _timer.timeout.connect(lambda: None)

    win = TerminalUI()
    sys.exit(app.exec())