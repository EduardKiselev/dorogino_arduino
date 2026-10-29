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
USB_PORTS = ['/dev/ttyUSB0', '/dev/ttyUSB1']  # Порты для автоопределения
BAUDRATE = 115200
HEARTBEAT_INTERVAL = 60   # секунды



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

        # Весовой порт (USB0)
        self.weight_ser = None
        self.weight_buf = ""

        self.rfid_mapping = self.load_rfid_mapping()
        self.admin_password_hash = self.load_admin_password()

        self.init_ui()
        self.detect_and_init_serials()
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
        self.list_widget.setStyleSheet("font-size: 54px; background-color: #313244;")
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
    def detect_and_init_serials(self):
        """Автоопределение портов: весовой контроллер и RFID по формату данных."""
        self.ser = None
        self.weight_ser = None
        self.serial_buffer = ""
        self.weight_buf = ""
        
        # Открываем оба порта
        ports_data = {}
        for port in USB_PORTS:
            try:
                ser = serial.Serial(port, BAUDRATE, timeout=2)
                ports_data[port] = {'serial': ser, 'data': ''}
                logger.info(f"✅ Порт {port} открыт")
            except Exception as e:
                logger.warning(f"⚠️ Не удалось открыть {port}: {e}")
        
        if len(ports_data) < 2:
            logger.error("❌ Нужно минимум 2 порта для автоопределения")
            return
        
        # Читаем первые данные от каждого порта (максимум 5 секунд)
        logger.info("🔍 Автоопределение портов...")
        start_time = time.time()
        detected = {'weight': None, 'rfid': None}
        
        while time.time() - start_time < 5:
            for port, data in ports_data.items():
                if data['serial'].in_waiting:
                    chunk = data['serial'].read(data['serial'].in_waiting).decode('utf-8', errors='ignore')
                    data['data'] += chunk
                    
                    # Определяем формат
                    if '#' in data['data'] and not detected['weight']:
                        # Весовой контроллер: +XXXXX.X#+YYYYY.YD
                        detected['weight'] = port
                        logger.info(f"⚖️ Весовой контроллер определён: {port}")
                    
                    if '{"type"' in data['data'] and not detected['rfid']:
                        # RFID: JSON с type
                        detected['rfid'] = port
                        logger.info(f"🏷️ RFID определён: {port}")
                    
                    # Если оба определены — выходим
                    if detected['weight'] and detected['rfid']:
                        break
            
            if detected['weight'] and detected['rfid']:
                break
            
            time.sleep(0.1)
        
        # Назначаем порты
        if detected['weight']:
            self.weight_ser = ports_data[detected['weight']]['serial']
            logger.info(f"✅ Весовой порт назначен: {detected['weight']}")
        else:
            logger.error("❌ Весовой контроллер не определён")
        
        if detected['rfid']:
            self.ser = ports_data[detected['rfid']]['serial']
            logger.info(f"✅ RFID порт назначен: {detected['rfid']}")
        else:
            logger.error("❌ RFID не определён")
        
        # Закрываем неиспользуемые порты
        for port, data in ports_data.items():
            if port != detected['weight'] and port != detected['rfid']:
                data['serial'].close()
                logger.debug(f"🔒 Неиспользуемый порт {port} закрыт")
        
        # Запускаем таймеры
        if self.ser:
            self.timer = QTimer()
            self.timer.timeout.connect(self.read_serial)
            self.timer.start(100)
            
            self.position_timer = QTimer()
            self.position_timer.timeout.connect(self.check_position_timeout)
            self.position_timer.start(200)
        
        if self.weight_ser:
            self.weight_timer = QTimer()
            self.weight_timer.timeout.connect(self.read_weight_serial)
            self.weight_timer.start(100)

    def read_weight_serial(self):
        """Читает и обрабатывает данные с весового порта (USB0)."""
        if not self.weight_ser:
            return

        if self.weight_ser.in_waiting:
            try:
                chunk = self.weight_ser.read(self.weight_ser.in_waiting).decode('utf-8', errors='ignore')
                self.weight_buf += chunk
            except Exception as e:
                logger.error(f"❌ Ошибка чтения весового порта: {e}")
                return

        # Парсим формат: +XXXXX.X#+YYYYY.YD\r
        while '\r' in self.weight_buf or '\n' in self.weight_buf:
            cr_pos = self.weight_buf.find('\r')
            lf_pos = self.weight_buf.find('\n')

            if cr_pos == -1 and lf_pos == -1:
                break

            if cr_pos == -1:
                end_pos = lf_pos
            elif lf_pos == -1:
                end_pos = cr_pos
            else:
                end_pos = min(cr_pos, lf_pos)

            line = self.weight_buf[:end_pos].strip()
            self.weight_buf = self.weight_buf[end_pos + 1:]

            if not line or '#' not in line:
                continue

            parts = line.split('#')
            if len(parts) == 2:
                try:
                    value2_str = parts[1].rstrip('D')
                    weight = float(value2_str)
                    self.current_weight = weight
                    self.update_weight_display()
                    logger.debug(f"⚖️ Вес: {weight:.1f}")
                except ValueError:
                    pass

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

            if dtype == "rfid":
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

        btn_exit = dialog.addButton("🚪 Выйти из программы", QMessageBox.ActionRole)
        btn_cancel = dialog.addButton("Отмена", QMessageBox.RejectRole)

        dialog.exec()

        clicked = dialog.clickedButton()
        if clicked == btn_exit:
            self.close_app()


    def close_app(self):
        """Закрывает приложение и освобождает ресурсы."""
        if hasattr(self, 'heartbeat_timer'):
            self.heartbeat_timer.stop()
        if self.ser:
            self.ser.close()
        if self.weight_ser:
            self.weight_ser.close()
        QApplication.quit()


    # ---------- КНОПКИ ----------
    def fix_weight(self):
        if not self.is_on_target:
            QMessageBox.warning(self, "Ошибка", "Нельзя фиксировать: вы не на метке!")
            logger.warning(f"❌ Попытка фиксации вне позиции")
            return

        tag_name = self.current_bin_name if self.current_bin_name else "бункер"
        timestamp = datetime.now().isoformat()
        weight_for_record = int(self.current_weight * 1000)
        record = {
            "tag": tag_name,
            "weight": weight_for_record,
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

        # Отправляем все накопленные записи на /mzo_weight
        mzo_url = REMOTE_SERVER_API.replace('/data', '/mzo_weight')
        
        try:
            response = requests.post(
                mzo_url,
                json={
                    "device_id": DEVICE_ID,
                    "sent_at": datetime.now().isoformat(),
                    "records": self.records
                },
                timeout=10
            )
            
            if response.status_code in (200, 201):
                logger.info(f"📤 Отправлено на /mzo_weight: {len(self.records)} записей")
                
                msg = QMessageBox(self)
                msg.setWindowTitle("Успех")
                msg.setText(f"Отправлено {len(self.records)} записей")
                msg.setStyleSheet("""
                    QMessageBox {
                        background-color: #313244;
                        color: #ffffff;
                    }
                    QMessageBox QLabel {
                        color: #ffffff;
                        font-size: 24px;
                        min-width: 400px;
                        padding: 20px;
                    }
                    QPushButton {
                        font-size: 20px;
                        padding: 15px 40px;
                        background-color: #45475a;
                        color: white;
                        border-radius: 10px;
                        margin: 10px;
                    }
                    QPushButton:hover {
                        background-color: #585b70;
                    }
                """)
                msg.exec()
                
                self.records.clear()
                self.list_widget.clear()
                self.last_fix_weight = 0.0
                self.update_weight_display()
            else:
                logger.error(f"❌ Ошибка отправки: статус {response.status_code}, ответ: {response.text}")
                QMessageBox.critical(self, "Ошибка", f"Сервер вернул ошибку: {response.status_code}")
        except requests.exceptions.ConnectionError:
            logger.error(f"❌ Не удалось подключиться к {mzo_url}")
            QMessageBox.critical(self, "Ошибка", "Не удалось подключиться к серверу")
        except requests.exceptions.Timeout:
            logger.error(f"❌ Таймаут отправки на {mzo_url}")
            QMessageBox.critical(self, "Ошибка", "Таймаут подключения к серверу")
        except Exception as e:
            logger.error(f"❌ Ошибка отправки: {e}")
            QMessageBox.critical(self, "Ошибка", f"Ошибка: {e}")

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